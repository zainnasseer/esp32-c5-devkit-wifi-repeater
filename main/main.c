// main.c - Simple Wi-Fi AP+STA repeater skeleton for ESP32-C5
//
// - Connects to your home router as STA (uplink)
// - Creates a Wi-Fi AP for clients
// - If NAT is enabled in menuconfig, it enables NAT at runtime
//
// Edit the STA_SSID, STA_PASS, AP_SSID, and AP_PASS definitions to match your network.

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"  

// Added for bandwidth statistics and NAT port mapping
#include "esp_netif_net_stack.h"  // for esp_netif_get_netif_impl()
#include "lwip/netif.h"            // underlying lwIP netif structure
#include "lwip/stats.h"            // for mib2_counters fields (optional)
#include "lwip/ip.h"               // for IP_PROTO_TCP, IP_PROTO_UDP

#if CONFIG_LWIP_IPV4_NAPT
#include "lwip/lwip_napt.h"  // NAT/NAPT helpers (enabled via menuconfig)
#endif

#include "web_server.h"  // Web dashboard server
#include "dns_proxy.h"  // DNS query logging proxy
#include "sys_monitor.h"  // System monitoring and benchmarking
#include "driver/gpio.h"  // For GPIO functions
#include "wifi_config_manager.h"  // WiFi configuration manager with NVS storage
#include "ap_clients.h" // For ap_clients_init

static const char *TAG = "wifi_repeater";

#define ESP_WIFI_MAXIMUM_RETRY  -1  // -1 = retry forever

static int s_retry_num = 0;
esp_netif_t *s_sta_netif = NULL;
esp_netif_t *s_ap_netif  = NULL;

// Handle for bandwidth monitoring task
static TaskHandle_t s_bw_task_handle = NULL;

// Forward declarations for new functionality
static void bandwidth_task(void *arg);
static void log_network_parameters(void);
static void nat_add_port_mapping(uint8_t proto, uint16_t ext_port, uint32_t int_ip, uint16_t int_port);

// Forward declaration
static void wifi_init_repeater(void);

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting to router...");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
            ESP_LOGI(TAG, "STA connected, channel=%d", event->channel);
            // Keep AP channel aligned with uplink to avoid disconnection loops
            uint8_t primary = event->channel;
            esp_wifi_set_channel(primary, WIFI_SECOND_CHAN_NONE);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "STA disconnected (reason=%d)", disc ? disc->reason : -1);
            // Stop bandwidth monitoring task when disconnected
            if (s_bw_task_handle != NULL) {
                vTaskDelete(s_bw_task_handle);
                s_bw_task_handle = NULL;
                ESP_LOGI(TAG, "Stopped bandwidth monitoring task");
            }
            if (ESP_WIFI_MAXIMUM_RETRY < 0 || s_retry_num < ESP_WIFI_MAXIMUM_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                if (ESP_WIFI_MAXIMUM_RETRY < 0) {
                    ESP_LOGI(TAG, "Retrying to connect to router (attempt %d)...",
                             s_retry_num);
                } else {
                    ESP_LOGI(TAG, "Retrying to connect to router (%d/%d)...",
                             s_retry_num, ESP_WIFI_MAXIMUM_RETRY);
                }
            } else {
                ESP_LOGE(TAG, "Failed to connect to router after %d retries",
                         ESP_WIFI_MAXIMUM_RETRY);
            }
            break;
        }

     case WIFI_EVENT_AP_STACONNECTED: {
    wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
    ESP_LOGI(TAG, "Client " MACSTR " joined, AID=%d",
             MAC2STR(event->mac), event->aid);
    break;
}

case WIFI_EVENT_AP_STADISCONNECTED: {
    wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
    ESP_LOGI(TAG, "Client " MACSTR " left, AID=%d",
             MAC2STR(event->mac), event->aid);
    break;
}


        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_num = 0;

        ESP_LOGI(TAG, "STA got IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask: " IPSTR ", GW: " IPSTR,
                 IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw));

#if CONFIG_LWIP_IPV4_NAPT
        /*
         * When the station gets an IP from the upstream router, configure NAT on the
         * soft-AP interface. According to lwIP NAPT documentation, NAT must be
         * enabled on the interface connecting to the private network (here, the
         * access point) so that traffic from AP clients is translated to the
         * station uplink. We also propagate the learned DNS server to the DHCP
         * server on the AP so that clients can resolve hostnames via the same
         * upstream resolver.
         */
        ESP_LOGI(TAG, "Configuring NAT and DNS for AP interface");
        esp_netif_ip_info_t ap_ip;
        if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK) {
            // Enable NAT on AP interface
            ESP_LOGI(TAG, "Enabling NAT on AP IP: " IPSTR, IP2STR(&ap_ip.ip));
            ip_napt_enable(ap_ip.ip.addr, 1);
            ESP_LOGI(TAG, "NAT enabled on AP");
        } else {
            ESP_LOGW(TAG, "Failed to get AP IP info for NAT");
        }
        // Determine upstream DNS server for the proxy
        esp_netif_dns_info_t sta_dns = { 0 };
        esp_err_t dns_get = esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &sta_dns);

        esp_netif_dns_info_t upstream_dns = { 0 };
        upstream_dns.ip.type = IPADDR_TYPE_V4;

        if (dns_get == ESP_OK && sta_dns.ip.u_addr.ip4.addr != 0) {
            upstream_dns.ip.u_addr.ip4.addr = sta_dns.ip.u_addr.ip4.addr;
            ESP_LOGI(TAG, "Using upstream DNS server: " IPSTR, IP2STR(&upstream_dns.ip.u_addr.ip4));
        } else {
            upstream_dns.ip.u_addr.ip4.addr = PP_HTONL(LWIP_MAKEU32(8,8,8,8));
            ESP_LOGI(TAG, "Using fallback DNS server: 8.8.8.8");
        }

        dns_proxy_set_upstream(&upstream_dns.ip.u_addr.ip4);
        esp_err_t proxy_err = dns_proxy_start();
        bool proxy_enabled = (proxy_err == ESP_OK);
        if (!proxy_enabled) {
            ESP_LOGW(TAG, "DNS proxy start failed (%d); advertising upstream DNS", proxy_err);
        }
        
        // Configure DHCP server to advertise gateway and DNS to clients
        // IMPORTANT: AP clients MUST know the AP (192.168.4.1) is their default gateway
        ESP_LOGI(TAG, "Configuring DHCP server to advertise gateway and DNS to AP clients");
        
        // Step 1: Stop DHCP server if running
        esp_netif_dhcps_stop(s_ap_netif);
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Step 2: Set gateway and DNS on the AP interface
        esp_netif_ip_info_t ap_ip_with_dns = ap_ip;
        ap_ip_with_dns.gw = ap_ip.ip;  // AP is the gateway for clients
        esp_netif_set_ip_info(s_ap_netif, &ap_ip_with_dns);
        ESP_LOGI(TAG, "Set AP gateway to: " IPSTR, IP2STR(&ap_ip_with_dns.gw));
        
        // Step 3: Advertise DNS server to AP clients (proxy if available)
        esp_netif_dns_info_t dns_offer = { 0 };
        dns_offer.ip.type = IPADDR_TYPE_V4;
        dns_offer.ip.u_addr.ip4.addr = proxy_enabled ? ap_ip.ip.addr : upstream_dns.ip.u_addr.ip4.addr;
        ESP_LOGI(TAG, "Setting DNS offer to: " IPSTR, IP2STR(&dns_offer.ip.u_addr.ip4));
        esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns_offer);
        esp_netif_dns_info_t ap_dns = { 0 };
        if (esp_netif_get_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &ap_dns) == ESP_OK) {
            ESP_LOGI(TAG, "AP DNS (netif): " IPSTR, IP2STR(&ap_dns.ip.u_addr.ip4));
        } else {
            ESP_LOGW(TAG, "Failed to read AP DNS (netif)");
        }
        
        // Set a secondary DNS matching the primary to keep logging consistent
        esp_netif_dns_info_t dns_backup = { 0 };
        dns_backup.ip.type = IPADDR_TYPE_V4;
        dns_backup.ip.u_addr.ip4.addr = dns_offer.ip.u_addr.ip4.addr;
        esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_BACKUP, &dns_backup);
        ESP_LOGI(TAG, "Set secondary DNS to: " IPSTR, IP2STR(&dns_backup.ip.u_addr.ip4));
        
        // Step 4: Enable DHCP offers BEFORE starting server
        uint8_t enable_router = 1;
        esp_err_t opt3_res = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                                                   (void *)&enable_router, sizeof(enable_router));
        ESP_LOGI(TAG, "Enable DHCP router offer (opt3): %d", opt3_res);
        
        uint8_t enable_dns = 1;
        esp_err_t opt6_res = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                                   (void *)&enable_dns, sizeof(enable_dns));
        ESP_LOGI(TAG, "Enable DHCP DNS offer (opt6): %d", opt6_res);
        
        // Step 5: Start DHCP server with new configuration
        vTaskDelay(pdMS_TO_TICKS(50));  // Small delay to ensure options are applied
        esp_err_t dhcp_start = esp_netif_dhcps_start(s_ap_netif);
        ESP_LOGI(TAG, "DHCP server started with result: %d", dhcp_start);
        vTaskDelay(pdMS_TO_TICKS(200));

        // Step 6: Read back DHCP options for verification
        uint8_t dhcp_router_offer = 0;
        esp_err_t opt3_get = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_GET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                                                   (void *)&dhcp_router_offer, sizeof(dhcp_router_offer));
        if (opt3_get == ESP_OK) {
            ESP_LOGI(TAG, "DHCP opt3 (router) offer enabled: %u, router IP: " IPSTR,
                     (unsigned)dhcp_router_offer, IP2STR(&ap_ip.ip));
        } else {
            ESP_LOGW(TAG, "Failed to read DHCP opt3 (router): %d", opt3_get);
        }

        uint8_t dhcp_dns_offer = 0;
        esp_err_t opt6_get = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_GET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                                   (void *)&dhcp_dns_offer, sizeof(dhcp_dns_offer));
        if (opt6_get == ESP_OK) {
            ESP_LOGI(TAG, "DHCP opt6 (DNS) offer enabled: %u, DNS: " IPSTR,
                     (unsigned)dhcp_dns_offer, IP2STR(&dns_offer.ip.u_addr.ip4));
        } else {
            ESP_LOGW(TAG, "Failed to read DHCP opt6 (DNS): %d", opt6_get);
        }
        
        ESP_LOGI(TAG, "DHCP configuration complete - GW: " IPSTR ", DNS: " IPSTR, IP2STR(&ap_ip.ip), IP2STR(&dns_offer.ip.u_addr.ip4));
        // Optional: add example port mapping for demonstration (comment out if not needed)
        // Map external TCP port 8080 on the STA IP to internal HTTP server at 192.168.4.2:80
        // uint32_t internal_ip = PP_HTONL(LWIP_MAKEU32(192, 168, 4, 2));
        // nat_add_port_mapping(IP_PROTO_TCP, 8080, internal_ip, 80);
        // Start bandwidth monitoring task once NAT is configured
        if (s_bw_task_handle == NULL) {
            xTaskCreate(bandwidth_task, "bw_monitor", 4096, NULL, 5, &s_bw_task_handle);
            ESP_LOGI(TAG, "Started bandwidth monitoring task");
        }
#else
        ESP_LOGW(TAG, "NAT is NOT enabled in menuconfig (LWIP IPv4 NAPT disabled)");
        ESP_LOGW(TAG, "AP clients may not have internet routing yet.");
#endif
    }
}

/*
 * Network monitoring task
 *
 * Periodically logs network parameters for both the AP and STA interfaces.
 * This includes SSID, channel, RSSI of the uplink, number of associated
 * clients on the AP, as well as IP configuration of both interfaces.
 */
static void bandwidth_task(void *arg)
{
    for (;;) {
        // Log network parameters every second
        log_network_parameters();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/*
 * Log various network parameters for both station and access point interfaces.
 * This includes SSID, channel, RSSI of the uplink, number of associated
 * clients on the AP, as well as IP configuration of both interfaces.
 */
static void log_network_parameters(void)
{
    // Uplink (STA) AP info: SSID, channel, RSSI
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        char ssid[33] = {0};
        memcpy(ssid, ap_info.ssid, sizeof(ap_info.ssid));
        ESP_LOGI(TAG, "STA connected to AP SSID: %s, Channel: %d, RSSI: %d dBm", ssid, ap_info.primary, ap_info.rssi);
    } else {
        ESP_LOGI(TAG, "STA not connected to any AP");
    }
    // Number of clients connected to our soft-AP
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        ESP_LOGI(TAG, "AP clients connected: %d", sta_list.num);
    }
    // Print IP information for both interfaces
    esp_netif_ip_info_t ap_ip, sta_ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK) {
        ESP_LOGI(TAG, "AP IP: " IPSTR "/" IPSTR ", GW: " IPSTR,
                 IP2STR(&ap_ip.ip), IP2STR(&ap_ip.netmask), IP2STR(&ap_ip.gw));
    }
    if (esp_netif_get_ip_info(s_sta_netif, &sta_ip) == ESP_OK) {
        ESP_LOGI(TAG, "STA IP: " IPSTR "/" IPSTR ", GW: " IPSTR,
                 IP2STR(&sta_ip.ip), IP2STR(&sta_ip.netmask), IP2STR(&sta_ip.gw));
    }
}

/*
 * Register a static port mapping for NAT. This forwards packets from the
 * upstream (station side) on a given external port to an internal device
 * behind the repeater on the access point network. The protocol can be
 * IP_PROTO_TCP or IP_PROTO_UDP. Ext_port and int_port are in host byte order.
 */
static void __attribute__((unused)) nat_add_port_mapping(uint8_t proto, uint16_t ext_port, uint32_t int_ip, uint16_t int_port)
{
#if CONFIG_LWIP_IPV4_NAPT
    // Retrieve our station (external) IP
    esp_netif_ip_info_t sta_ip;
    if (esp_netif_get_ip_info(s_sta_netif, &sta_ip) == ESP_OK) {
        uint32_t maddr = sta_ip.ip.addr;
        // ip_portmap_add expects ports in host order and addresses in network order
        // Note: ip_portmap_add returns 1 on success
        if (ip_portmap_add(proto, maddr, ext_port, int_ip, int_port)) {
            ip4_addr_t int_ip_addr = { .addr = int_ip };
            ESP_LOGI(TAG, "Added port mapping: %s %u -> " IPSTR ":%u",
                     (proto == IP_PROTO_TCP) ? "TCP" : "UDP", ext_port, IP2STR(&int_ip_addr), int_port);
        } else {
            ESP_LOGW(TAG, "Failed to add port mapping for port %u", ext_port);
        }
    } else {
        ESP_LOGW(TAG, "Cannot obtain STA IP; port mapping not added");
    }
#else
    ESP_LOGW(TAG, "Port mapping not available: NAPT disabled in lwIP");
#endif
}

static void wifi_init_repeater(void)
{
    ESP_LOGI(TAG, "Initializing netif and event loop...");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default STA and AP network interfaces
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    (void)s_sta_netif;
    (void)s_ap_netif;
    // Ensure default route uses the STA uplink
    esp_netif_set_default_netif(s_sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register Wi-Fi and IP events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Load WiFi configuration from NVS
    repeater_config_t wifi_cfg;
    esp_err_t cfg_err = wifi_config_load(&wifi_cfg);
    
    if (cfg_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No WiFi config in NVS, using defaults (first boot)");
        // Save defaults to NVS for future boots
        wifi_config_save(&wifi_cfg);
        ESP_LOGI(TAG, "Default configuration saved to NVS");
    } else if (cfg_err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi configuration loaded from NVS");
    } else {
        ESP_LOGW(TAG, "Error loading WiFi config, using defaults");
    }

    // ===== Configure STA (uplink to your router) =====
    wifi_config_t sta_config = {
        .sta = {
            .ssid = "",
            .password = "",
            // Require at least WPA2; WPA3 disabled to avoid AP compatibility issues
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false, // keep PMF optional to avoid association refusals
            },
        },
    };
    
    // Copy loaded credentials to STA config
    strncpy((char *)sta_config.sta.ssid, wifi_cfg.sta_ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, wifi_cfg.sta_password, sizeof(sta_config.sta.password));

    // ===== Configure AP (for your phone / laptop) =====
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .channel = 1,
            .password = "",
            .max_connection = 0,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    
    // Copy loaded credentials to AP config
    strncpy((char *)ap_config.ap.ssid, wifi_cfg.ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(wifi_cfg.ap_ssid);
    strncpy((char *)ap_config.ap.password, wifi_cfg.ap_password, sizeof(ap_config.ap.password));
    ap_config.ap.max_connection = wifi_cfg.ap_max_connections;

    size_t ap_pass_len = strlen((char *)ap_config.ap.password);
    if (ap_pass_len == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    } else if (ap_pass_len < 8) {
        ESP_LOGW(TAG, "AP password is shorter than 8 chars; falling back to open auth");
        ap_config.ap.password[0] = '\0';
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    ESP_LOGI(TAG, "Starting Wi-Fi in AP+STA mode...");
    ESP_ERROR_CHECK(esp_wifi_start());
    // Keep radio fully awake to reduce latency and improve NAT reliability
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // ── Throughput optimizations ────────────────────────────────────────────
    // Use HT40 (40 MHz) bandwidth on both interfaces for ~double PHY rate
    // (e.g. 150 Mbps instead of 72 Mbps on 802.11n MCS7).
    // Falls back gracefully if the uplink AP doesn't support HT40.
    esp_err_t bw_err;
    bw_err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
    if (bw_err != ESP_OK) {
        ESP_LOGW(TAG, "STA HT40 not available, using HT20 (%s)", esp_err_to_name(bw_err));
    } else {
        ESP_LOGI(TAG, "STA bandwidth: HT40");
    }
    bw_err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40);
    if (bw_err != ESP_OK) {
        ESP_LOGW(TAG, "AP HT40 not available, using HT20 (%s)", esp_err_to_name(bw_err));
    } else {
        ESP_LOGI(TAG, "AP bandwidth: HT40");
    }

    // Set maximum TX power for best link budget / throughput
    // ESP32-C5 supports up to 84 (21 dBm) in units of 0.25 dBm
    esp_wifi_set_max_tx_power(84);
    ESP_LOGI(TAG, "TX power set to maximum (21 dBm)");
    // ───────────────────────────────────────────────────────────────────────

    ESP_LOGI(TAG, "Wi-Fi AP SSID: %s, password: %s",
             wifi_cfg.ap_ssid, wifi_cfg.ap_password);
    ESP_LOGI(TAG, "Connecting STA to SSID: %s", wifi_cfg.sta_ssid);
}

#define START_BUTTON_GPIO GPIO_NUM_0  // Define GPIO pin for the start button
#define BUTTON_DEBOUNCE_TIME_MS 50    // Debounce time for button press

static void start_button_task(void *arg) {
    gpio_set_direction(START_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(START_BUTTON_GPIO);
    gpio_pulldown_dis(START_BUTTON_GPIO);

    bool button_pressed = false;
    uint32_t press_start_time = 0;
    const uint32_t LONG_PRESS_TIME_MS = 3000;  // 3 seconds for WiFi reset
    
    while (true) {
        int button_state = gpio_get_level(START_BUTTON_GPIO);
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        if (button_state == 0 && !button_pressed) {  // Button just pressed (active low)
            button_pressed = true;
            press_start_time = current_time;
            ESP_LOGI(TAG, "Boot button pressed...");
        } else if (button_state == 0 && button_pressed) {  // Button still held
            uint32_t press_duration = current_time - press_start_time;
            if (press_duration >= LONG_PRESS_TIME_MS) {
                // Long press detected - erase WiFi config
                ESP_LOGW(TAG, "Long press detected! Erasing WiFi configuration...");
                esp_err_t erase_err = wifi_config_erase();
                if (erase_err == ESP_OK) {
                    ESP_LOGI(TAG, "WiFi config erased successfully. Restarting in 2 seconds...");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    esp_restart();
                } else {
                    ESP_LOGE(TAG, "Failed to erase WiFi config: %s", esp_err_to_name(erase_err));
                }
                // Wait for button release to avoid multiple triggers
                while (gpio_get_level(START_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                button_pressed = false;
            }
        } else if (button_state == 1 && button_pressed) {  // Button released
            uint32_t press_duration = current_time - press_start_time;
            if (press_duration < LONG_PRESS_TIME_MS) {
                ESP_LOGI(TAG, "Short button press (held for %lu ms)", press_duration);
            }
            button_pressed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_TIME_MS));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "NVS initialized");
    
    // Initialize WiFi configuration manager
    ESP_ERROR_CHECK(wifi_config_init());

    // Initialize system monitoring
    ESP_LOGI(TAG, "Initializing system monitor...");
    sys_monitor_init();

    wifi_init_repeater();

    // Initialize AP clients tracking
    ESP_ERROR_CHECK(ap_clients_init());

    // Initialize web server for dashboard
    ESP_LOGI(TAG, "Initializing web server...");
    web_server_init();

    // Start button task
    xTaskCreate(start_button_task, "start_button_task", 2048, NULL, 5, NULL);

    // Nothing else to do in app_main; everything is event-driven.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
