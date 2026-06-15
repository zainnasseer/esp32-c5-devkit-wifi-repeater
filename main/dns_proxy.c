/**
 * @file dns_proxy.c
 * @brief DNS Proxy Server for WiFi Repeater
 * 
 * This module implements a transparent DNS proxy that intercepts DNS queries
 * from WiFi clients connected to the access point, logs them with client
 * identification (MAC address and IP), and forwards them to an upstream DNS server.
 * 
 * The proxy works by:
 * 1. Listening on UDP port 53 (standard DNS port) for queries from AP clients
 * 2. Parsing DNS query packets to extract domain names and query types
 * 3. Looking up the client's MAC address from their IP (via ap_clients module)
 * 4. Logging the query with client information (via dns_log module)
 * 5. Forwarding the query to the configured upstream DNS server
 * 6. Relaying the response back to the original client
 * 
 * This allows the WiFi repeater to monitor and log all DNS requests made by
 * connected clients for troubleshooting, analytics, or security purposes.
 */

#include "dns_proxy.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

#include "ap_clients.h"
#include "dns_log.h"
#include "sys_monitor.h"

/* DNS proxy configuration constants */
#define DNS_PROXY_PORT 53              // Standard DNS port
#define DNS_PROXY_BUF_SIZE 1024        // Buffer size for DNS packets (max UDP DNS is 512, but allowing extra)
#define DNS_PROXY_STACK_SIZE 8192      // FreeRTOS task stack size (Increased to 8K for large buffers)
#define DNS_PROXY_TASK_PRIO 5          // Task priority (medium priority)

static const char *TAG = "dns_proxy";

/* DNS proxy state variables */
static TaskHandle_t s_dns_task = NULL;           // Handle to the DNS proxy FreeRTOS task
static volatile bool s_dns_running = false;      // Flag indicating if the proxy is actively running
static esp_ip4_addr_t s_upstream_dns = {0};      // IP address of the upstream DNS server to forward queries to
static bool s_has_upstream = false;              // Flag indicating if an upstream DNS is configured

/* Captive portal mode state */
static bool            s_captive_mode = false;    // When true, hijack all DNS queries
static esp_ip4_addr_t  s_captive_ip;              // Device IP to resolve all domains to

/**
 * @brief Format a MAC address as a human-readable string
 * 
 * Converts a 6-byte MAC address into the standard colon-separated format
 * (e.g., "aa:bb:cc:dd:ee:ff")
 * 
 * @param mac Pointer to 6-byte MAC address array
 * @param out Output buffer for formatted string
 * @param out_len Size of output buffer (should be at least 18 bytes for full MAC)
 */
static void format_mac(const uint8_t *mac, char *out, size_t out_len)
{
    if (out_len == 0) {
        return;
    }
    if (mac == NULL) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/**
 * @brief Look up the MAC address for a given IP address
 * 
 * Searches the list of connected AP clients to find which client has the
 * specified IP address, then returns their MAC address. This is used to
 * identify clients making DNS requests by correlating their source IP with
 * their MAC address from the AP client table.
 * 
 * @param ip IP address to look up
 * @param mac_out Output buffer for formatted MAC address string
 * @param mac_len Size of output buffer
 * @return true if MAC was found for this IP, false otherwise
 */
static bool lookup_mac_for_ip(const esp_ip4_addr_t *ip, char *mac_out, size_t mac_len)
{
    ap_client_list_t list = {0};

    // Validate input parameters
    if (ip == NULL || mac_out == NULL || mac_len == 0) {
        return false;
    }
    
    // Get the current list of connected AP clients
    if (ap_clients_get(&list) != ESP_OK) {
        return false;
    }
    
    // Search through clients to find one with matching IP
    for (size_t i = 0; i < list.count; i++) {
        if (!list.clients[i].has_ip) {
            continue;  // Skip clients without assigned IPs
        }
        if (list.clients[i].ip.addr == ip->addr) {
            // Found matching client, format and return their MAC
            format_mac(list.clients[i].mac, mac_out, mac_len);
            return true;
        }
    }
    return false;  // No matching client found
}

/**
 * @brief Parse a DNS query packet to extract the domain name and query type
 * 
 * DNS query packets use a specific wire format defined in RFC 1035:
 * - First 12 bytes: DNS header (transaction ID, flags, counts)
 * - After header: QNAME (domain name in label format)
 * - After QNAME: QTYPE (2 bytes) and QCLASS (2 bytes)
 * 
 * The domain name is encoded as "labels" where each label is:
 * - 1 byte: length of label
 * - N bytes: label characters
 * - Next label or 0x00 to terminate
 * 
 * For example, "www.example.com" is encoded as:
 * \x03www\x07example\x03com\x00
 * 
 * This function extracts the domain name and converts it to human-readable format.
 * 
 * @param buf DNS packet buffer
 * @param len Length of DNS packet
 * @param qname Output buffer for domain name (human-readable)
 * @param qname_len Size of qname buffer
 * @param qtype_out Output for query type (A=1, AAAA=28, etc.)
 * @return true if successfully parsed, false on parse error
 */
static bool parse_dns_query(const uint8_t *buf,
                            size_t len,
                            char *qname,
                            size_t qname_len,
                            uint16_t *qtype_out)
{
    size_t offset = 12;   // Skip DNS header (first 12 bytes)
    size_t out_pos = 0;   // Current position in output string

    // Validate input parameters
    if (qname_len == 0 || qname == NULL || buf == NULL || len < 12) {
        return false;  // Invalid input or packet too short
    }

    // Check if there are any questions in the DNS query
    // Bytes 4-5 contain the question count (QDCOUNT)
    if (buf[4] == 0 && buf[5] == 0) {
        return false;  // No questions in this DNS packet
    }

    // Parse the QNAME (domain name) in label format
    while (offset < len) {
        uint8_t label_len = buf[offset++];
        
        // Label length of 0 marks the end of the domain name
        if (label_len == 0) {
            break;
        }
        
        // Check if this is a compression pointer (top 2 bits set)
        // We don't support compression in this simple parser
        if ((label_len & 0xC0) != 0) {
            return false;  // Compressed names not supported
        }
        
        // Ensure label doesn't exceed packet boundary
        if (offset + label_len > len) {
            return false;  // Malformed packet
        }
        
        // Add dot separator between labels (except before first label)
        if (out_pos != 0 && out_pos < qname_len - 1) {
            qname[out_pos++] = '.';
        }
        
        // Copy label characters to output
        for (uint8_t i = 0; i < label_len; i++) {
            char c = (char)buf[offset++];
            // Replace non-printable characters with '?'
            if (!isprint((unsigned char)c)) {
                c = '?';
            }
            if (out_pos < qname_len - 1) {
                qname[out_pos++] = c;
            }
        }
    }

    // Ensure we extracted at least some domain name
    if (out_pos == 0) {
        return false;
    }
    qname[out_pos] = '\0';  // Null-terminate the string

    // Parse QTYPE (2 bytes after the QNAME)
    // QTYPE indicates the type of DNS query (A, AAAA, MX, etc.)
    if (offset + 4 > len) {
        return false;  // Not enough bytes for QTYPE and QCLASS
    }
    if (qtype_out != NULL) {
        // QTYPE is big-endian 16-bit value
        uint16_t qtype = (uint16_t)((buf[offset] << 8) | buf[offset + 1]);
        *qtype_out = qtype;
    }
    return true;
}

/**
 * @brief Main DNS proxy task that handles all DNS query forwarding
 * 
 * This FreeRTOS task implements the core DNS proxy logic:
 * 1. Creates two UDP sockets (one for listening, one for upstream)
 * 2. Listens for DNS queries on port 53 from AP clients
 * 3. For each query:
 *    - Parses it to extract domain name and type
 *    - Looks up client MAC from their IP
 *    - Logs the query with client info
 *    - Forwards to upstream DNS server
 *    - Waits for response
 *    - Sends response back to original client
 * 4. Continues until s_dns_running is set to false
 * 
 * @param arg Unused task parameter
 */
// ===== DNS Cache Implementation =====

#define DNS_CACHE_SIZE 20
#define DNS_CACHE_TTL_SEC 60

typedef struct {
    char qname[256];
    uint16_t qtype;
    uint8_t response[DNS_PROXY_BUF_SIZE];
    int response_len;
    int64_t expiry_time;
    bool valid;
} dns_cache_entry_t;

static dns_cache_entry_t s_dns_cache[DNS_CACHE_SIZE];

static void dns_cache_init(void) {
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        s_dns_cache[i].valid = false;
    }
}

static int dns_cache_lookup(const char *qname, uint16_t qtype, uint8_t *buf, int buf_size) {
    int64_t now = esp_timer_get_time();
    
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (s_dns_cache[i].valid && 
            s_dns_cache[i].qtype == qtype && 
            strncmp(s_dns_cache[i].qname, qname, 255) == 0) {
            
            // Check expiry
            if (now > s_dns_cache[i].expiry_time) {
                s_dns_cache[i].valid = false; // Expired
                return -1;
            }
            
            // Cache hit
            if (s_dns_cache[i].response_len > buf_size) {
                return -1; // Buffer too small
            }
            
            memcpy(buf, s_dns_cache[i].response, s_dns_cache[i].response_len);
            return s_dns_cache[i].response_len;
        }
    }
    return -1; // Not found
}

static void dns_cache_store(const char *qname, uint16_t qtype, const uint8_t *response, int len) {
    if (len > DNS_PROXY_BUF_SIZE) return;

    int64_t now = esp_timer_get_time();
    int idx = -1;
    
    // Strategy: Find first empty or expired slot, or overwrite oldest
    int64_t oldest_expiry = INT64_MAX;
    int oldest_idx = 0;

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!s_dns_cache[i].valid || now > s_dns_cache[i].expiry_time) {
            idx = i;
            break;
        }
        if (s_dns_cache[i].expiry_time < oldest_expiry) {
            oldest_expiry = s_dns_cache[i].expiry_time;
            oldest_idx = i;
        }
    }
    
    if (idx == -1) {
        idx = oldest_idx; // Overwrite oldest if full
    }
    
    // Store entry
    strncpy(s_dns_cache[idx].qname, qname, 255);
    s_dns_cache[idx].qname[255] = '\0';
    s_dns_cache[idx].qtype = qtype;
    memcpy(s_dns_cache[idx].response, response, len);
    s_dns_cache[idx].response_len = len;
    s_dns_cache[idx].expiry_time = now + (DNS_CACHE_TTL_SEC * 1000000LL);
    s_dns_cache[idx].valid = true;
}

/* ─── Captive Portal DNS Response Builders ─────────────────────────────────── */

/**
 * @brief Find the end offset of the DNS question section
 *
 * Walks past the 12-byte header and the QNAME labels to find the byte
 * after QTYPE + QCLASS (4 bytes). Returns -1 on malformed input.
 */
static int question_end(const uint8_t *buf, int len)
{
    int off = 12;
    while (off < len && buf[off] != 0) {
        off += buf[off] + 1;  // walk labels
    }
    return (off < len) ? off + 1 + 4 : -1;  // null byte + QTYPE(2) + QCLASS(2)
}

/**
 * @brief Build a DNS A-record response pointing to the captive portal IP
 *
 * Copies the query header + question, sets QR=1/RA=1/ANCOUNT=1, appends
 * a compressed A record pointing to the given IPv4 address with TTL=60s.
 */
static int build_captive_response(const uint8_t *q, int qlen,
                                  uint8_t *out, int out_cap, uint32_t ip_be)
{
    int qend = question_end(q, qlen);
    if (qend < 0 || qend + 16 > out_cap) return -1;
    memcpy(out, q, qend);
    out[2] = 0x81; out[3] = 0x80;           // QR=1, RD=1, RA=1, RCODE=0
    out[6] = 0x00; out[7] = 0x01;           // ANCOUNT = 1
    out[8] = out[9] = out[10] = out[11] = 0; // NSCOUNT/ARCOUNT = 0
    int o = qend;
    out[o++] = 0xC0; out[o++] = 0x0C;       // name pointer -> offset 12
    out[o++] = 0x00; out[o++] = 0x01;       // TYPE A
    out[o++] = 0x00; out[o++] = 0x01;       // CLASS IN
    out[o++] = 0x00; out[o++] = 0x00;
    out[o++] = 0x00; out[o++] = 0x3C;       // TTL 60s
    out[o++] = 0x00; out[o++] = 0x04;       // RDLENGTH 4
    memcpy(out + o, &ip_be, 4); o += 4;     // RDATA (network byte order)
    return o;
}

/**
 * @brief Build a DNS NODATA response (NOERROR, 0 answers)
 *
 * Used for AAAA and other non-A queries in captive mode so clients
 * fall back to IPv4 instead of stalling.
 */
static int build_nodata_response(const uint8_t *q, int qlen,
                                 uint8_t *out, int out_cap)
{
    int qend = question_end(q, qlen);
    if (qend < 0 || qend > out_cap) return -1;
    memcpy(out, q, qend);
    out[2] = 0x81; out[3] = 0x80;           // QR=1, RA=1, RCODE=0
    out[6] = out[7] = 0x00;                 // ANCOUNT = 0 (NODATA)
    out[8] = out[9] = out[10] = out[11] = 0;
    return qend;
}


/**
 * @brief Main DNS proxy task that handles all DNS query forwarding
 * 
 * This FreeRTOS task implements the core DNS proxy logic:
 * 1. Creates two UDP sockets (one for listening, one for upstream)
 * 2. Listens for DNS queries on port 53 from AP clients
 * 3. For each query:
 *    - Parses it to extract domain name and type
 *    - Looks up client MAC from their IP
 *    - Logs the query with client info
 *    - **Checks Cache**: If found, returns cached response immediately.
 *    - If not in cache:
 *        - Forwards to upstream DNS server
 *        - Waits for response
 *        - **Stores in Cache**
 *        - Sends response back to original client
 * 4. Continues until s_dns_running is set to false
 * 
 * @param arg Unused task parameter
 */
static void dns_proxy_task(void *arg)
{
    int listen_sock = -1;       // Socket for listening to queries from AP clients
    int upstream_sock = -1;     // Socket for forwarding queries to upstream DNS
    struct sockaddr_in listen_addr = {0};
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };  // 1 second timeout for recv operations

    (void)arg;  // Unused parameter
    
    // Initialize cache
    dns_cache_init();

    // Create UDP socket for listening to DNS queries from clients
    listen_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create listen socket: errno=%d", errno);
        goto done;
    }
    // Set receive timeout to prevent blocking indefinitely
    setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Bind to DNS port 53 on all network interfaces
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(DNS_PROXY_PORT);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);  // Listen on all interfaces
    if (bind(listen_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS proxy: errno=%d", errno);
        goto done;
    }

    // Create separate socket for forwarding queries to upstream DNS
    upstream_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (upstream_sock < 0) {
        ESP_LOGE(TAG, "Failed to create upstream socket: errno=%d", errno);
        goto done;
    }
    setsockopt(upstream_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    ESP_LOGI(TAG, "DNS proxy started on UDP/%d", DNS_PROXY_PORT);

    // Main proxy loop - continues until stopped
    while (s_dns_running) {
        uint8_t buf[DNS_PROXY_BUF_SIZE];  // Buffer for DNS packet data
        struct sockaddr_in from = {0};     // Source address of incoming query
        socklen_t from_len = sizeof(from);
        
        // Start benchmark timer for this query
        BENCHMARK_START(dns_query);
        
        // Receive DNS query from a client
        int len = recvfrom(listen_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len < 0) {
            // Handle common non-error conditions (timeout, interrupt)
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
                continue;  // No data available, try again
            }
            ESP_LOGW(TAG, "DNS recv error: errno=%d", errno);
            continue;
        }

        // Check if upstream DNS is configured
        if (!s_has_upstream) {
            ESP_LOGW(TAG, "No upstream DNS configured; dropping query");
            continue;
        }

        // Extract client information for logging
        esp_ip4_addr_t client_ip = { .addr = from.sin_addr.s_addr };
        char client_ip_str[16] = {0};
        char client_mac[18] = {0};
        char qname[256] = {0};   // Domain name being queried
        uint16_t qtype = 0;      // Query type (A, AAAA, etc.)
        
        // Try to find the MAC address for this client's IP
        bool have_mac = lookup_mac_for_ip(&client_ip, client_mac, sizeof(client_mac));
        snprintf(client_ip_str, sizeof(client_ip_str), IPSTR, IP2STR(&client_ip));

        // Parse the DNS query to extract domain name and type
        bool parsed = parse_dns_query(buf, (size_t)len, qname, sizeof(qname), &qtype);

        if (parsed) {
            // Successfully parsed - log with full details
            ESP_LOGI(TAG, "DNS query from %s (%s): %s (type %u)",
                     have_mac ? client_mac : "unknown", client_ip_str, qname, qtype);
            dns_log_add(have_mac ? client_mac : "unknown", client_ip_str, qname, qtype);
            
            // --- CAPTIVE PORTAL INTERCEPT ---
            if (s_captive_mode) {
                uint8_t captive_resp[DNS_PROXY_BUF_SIZE];
                int rlen = (qtype == 1 /* A-record */)
                    ? build_captive_response(buf, len, captive_resp, sizeof(captive_resp), s_captive_ip.addr)
                    : build_nodata_response(buf, len, captive_resp, sizeof(captive_resp));
                if (rlen > 0) {
                    sendto(listen_sock, captive_resp, rlen, 0, (struct sockaddr *)&from, from_len);
                }
                uint32_t query_latency = BENCHMARK_GET_ELAPSED(dns_query);
                sys_monitor_record_dns_query(query_latency);
                continue;  // Skip upstream forwarding
            }

            // --- CACHE LOOKUP ---
            uint8_t resp_buf[DNS_PROXY_BUF_SIZE];
            int cached_len = dns_cache_lookup(qname, qtype, resp_buf, sizeof(resp_buf));
            if (cached_len > 0) {
                ESP_LOGI(TAG, "DNS Cache HIT for %s", qname);
                
                // Fix Transaction ID in cached response to match current query
                // Transaction ID is the first 2 bytes (bytes 0-1)
                resp_buf[0] = buf[0];
                resp_buf[1] = buf[1];
                
                // Send cached response
                sendto(listen_sock, resp_buf, cached_len, 0, (struct sockaddr *)&from, from_len);
                
                // Benchmarking for cache hit
                uint32_t query_latency = BENCHMARK_GET_ELAPSED(dns_query);
                sys_monitor_record_dns_query(query_latency);
                sys_monitor_record_dns_cache_hit(); // Record hit
                continue; // Done with this query
            } else {
                sys_monitor_record_dns_cache_miss(); // Record miss
            }
        } else {
            // Couldn't parse - log as unparsed
            ESP_LOGI(TAG, "DNS query from %s (%s): <unparsed>",
                     have_mac ? client_mac : "unknown", client_ip_str);
            dns_log_add(have_mac ? client_mac : "unknown", client_ip_str, "<unparsed>", 0);
        }

        // Forward the query to the upstream DNS server
        struct sockaddr_in upstream = {0};
        upstream.sin_family = AF_INET;
        upstream.sin_port = htons(DNS_PROXY_PORT);
        upstream.sin_addr.s_addr = s_upstream_dns.addr;
        if (sendto(upstream_sock, buf, len, 0,
                   (struct sockaddr *)&upstream, sizeof(upstream)) < 0) {
            ESP_LOGW(TAG, "Failed to forward DNS query: errno=%d", errno);
            continue;
        }

        // Wait for response from upstream DNS server
        struct sockaddr_in upstream_from = {0};
        socklen_t upstream_from_len = sizeof(upstream_from);
        int resp_len = recvfrom(upstream_sock, buf, sizeof(buf), 0,
                                (struct sockaddr *)&upstream_from, &upstream_from_len);
        if (resp_len < 0) {
            ESP_LOGW(TAG, "No DNS response (timeout): errno=%d", errno);
            continue;  // Response timeout - client will retry
        }
        
        // --- CACHE STORE ---
        if (parsed && resp_len > 0) {
            // Only cache valid responses (checking ID match is good practice but we trust upstream for now)
            dns_cache_store(qname, qtype, buf, resp_len);
        }
        
        // Send the response back to the original client
        if (sendto(listen_sock, buf, resp_len, 0, (struct sockaddr *)&from, from_len) < 0) {
            ESP_LOGW(TAG, "Failed to send DNS response: errno=%d", errno);
        }
        
        // Record query latency benchmark
        uint32_t query_latency = BENCHMARK_GET_ELAPSED(dns_query);
        sys_monitor_record_dns_query(query_latency);
    }

done:
    // Cleanup - close sockets and reset state
    if (listen_sock >= 0) {
        close(listen_sock);
    }
    if (upstream_sock >= 0) {
        close(upstream_sock);
    }
    s_dns_running = false;
    s_dns_task = NULL;
    ESP_LOGI(TAG, "DNS proxy stopped");
    vTaskDelete(NULL);  // Delete this FreeRTOS task
}

/**
 * @brief Start the DNS proxy server
 * 
 * Creates and starts the DNS proxy FreeRTOS task. The proxy will begin
 * listening for DNS queries on port 53 and forwarding them to the configured
 * upstream DNS server (if set).
 * 
 * This function is idempotent - calling it multiple times is safe.
 * 
 * @return ESP_OK on success, ESP_ERR_NO_MEM if task creation fails
 */
esp_err_t dns_proxy_start(void)
{
    // If already running, return success
    if (s_dns_task != NULL) {
        return ESP_OK;
    }
    
    // Set running flag before creating task
    s_dns_running = true;
    
    // Create the DNS proxy FreeRTOS task
    if (xTaskCreate(dns_proxy_task, "dns_proxy", DNS_PROXY_STACK_SIZE, NULL,
                    DNS_PROXY_TASK_PRIO, &s_dns_task) != pdPASS) {
        // Task creation failed - reset state
        s_dns_running = false;
        s_dns_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * @brief Stop the DNS proxy server
 * 
 * Signals the DNS proxy task to stop. The task will finish processing its
 * current query (if any) and then clean up and exit.
 * 
 * This function is non-blocking - it sets a flag and returns immediately.
 * The actual task will terminate shortly after.
 * 
 * This function is safe to call even if the proxy is not running.
 */
void dns_proxy_stop(void)
{
    if (s_dns_task == NULL) {
        return;  // Not running, nothing to do
    }
    s_dns_running = false;  // Signal task to stop
}

/**
 * @brief Configure the upstream DNS server
 * 
 * Sets the IP address of the upstream DNS server that queries will be
 * forwarded to. This must be called before the proxy can forward queries.
 * 
 * Without an upstream DNS configured, the proxy will simply drop incoming
 * queries and log a warning.
 * 
 * @param dns_ip IP address of upstream DNS server, or NULL to clear
 */
void dns_proxy_set_upstream(const esp_ip4_addr_t *dns_ip)
{
    if (dns_ip == NULL) {
        // Clear upstream DNS configuration
        s_has_upstream = false;
        memset(&s_upstream_dns, 0, sizeof(s_upstream_dns));
        return;
    }
    // Set upstream DNS server
    s_upstream_dns = *dns_ip;
    s_has_upstream = (dns_ip->addr != 0);  // Only mark as valid if non-zero
}

void dns_proxy_set_captive_mode(bool enable, const esp_ip4_addr_t *device_ip)
{
    s_captive_mode = enable;
    if (device_ip) {
        s_captive_ip = *device_ip;
    }
    if (enable && device_ip) {
        ESP_LOGI(TAG, "Captive portal mode ENABLED (IP: " IPSTR ")",
                 IP2STR(device_ip));
    } else if (!enable) {
        ESP_LOGI(TAG, "Captive portal mode DISABLED");
    }
}
