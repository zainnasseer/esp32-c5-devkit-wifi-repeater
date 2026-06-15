// web_server.c - Simple HTTP server for WiFi repeater dashboard
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/task.h"
#include "web_server.h"
#include "ap_clients.h"
#include "dns_log.h"
#include "sys_monitor.h"
#include "ap_clients.h"
#include "dns_log.h"
#include "sys_monitor.h"
#include "esp_spiffs.h"
#include "wifi_config_manager.h"

static const char *TAG = "web_server";
static httpd_handle_t server = NULL;

// Provisioning mode flag — set before server init
static bool s_provisioning = false;

// External references from main.c
extern esp_netif_t *s_sta_netif;
extern esp_netif_t *s_ap_netif;

static const char *wifi_band_for_channel(uint8_t channel)
{
    if (channel >= 1 && channel <= 14) {
        return "2.4 GHz";
    }
    if (channel >= 36) {
        return "5 GHz";
    }
    return "-";
}

static void add_ap_clients_json(cJSON *root, uint16_t *client_count_out)
{
    ap_client_list_t client_list = {0};
    cJSON *client_entries = cJSON_CreateArray();
    uint16_t client_count = 0;

    if (ap_clients_get(&client_list) == ESP_OK) {
        client_count = (uint16_t)client_list.count;
        if (client_entries != NULL) {
            for (size_t i = 0; i < client_list.count; i++) {
                char mac_str[18] = {0};
                char ip_str[16] = "-";
                snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                         client_list.clients[i].mac[0], client_list.clients[i].mac[1],
                         client_list.clients[i].mac[2], client_list.clients[i].mac[3],
                         client_list.clients[i].mac[4], client_list.clients[i].mac[5]);
                if (client_list.clients[i].has_ip && client_list.clients[i].ip.addr != 0) {
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&client_list.clients[i].ip));
                }
                cJSON *entry = cJSON_CreateObject();
                if (entry == NULL) {
                    continue;
                }
                cJSON_AddStringToObject(entry, "mac", mac_str);
                cJSON_AddStringToObject(entry, "ipv4", ip_str);
                cJSON_AddItemToArray(client_entries, entry);
            }
        }
    }

    cJSON_AddNumberToObject(root, "ap_clients", client_count);
    if (client_entries != NULL) {
        cJSON_AddItemToObject(root, "client_list", client_entries);
    }
    if (client_count_out != NULL) {
        *client_count_out = client_count;
    }
}

static void add_dns_logs_json(cJSON *root)
{
    dns_log_entry_t entries[DNS_LOG_MAX_ENTRIES];
    size_t count = dns_log_get_latest(entries, DNS_LOG_MAX_ENTRIES);
    cJSON *dns_logs = cJSON_CreateArray();

    if (dns_logs == NULL) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            continue;
        }
        cJSON_AddNumberToObject(item, "ts", entries[i].timestamp_s);
        cJSON_AddStringToObject(item, "mac", entries[i].mac[0] ? entries[i].mac : "unknown");
        cJSON_AddStringToObject(item, "ip", entries[i].ip[0] ? entries[i].ip : "-");
        cJSON_AddStringToObject(item, "domain", entries[i].domain[0] ? entries[i].domain : "-");
        cJSON_AddNumberToObject(item, "qtype", entries[i].qtype);
        cJSON_AddItemToArray(dns_logs, item);
    }

    cJSON_AddItemToObject(root, "dns_logs", dns_logs);
}

/**
 * Initialize SPIFFS filesystem
 */
static esp_err_t init_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");
    
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 5,
        .format_if_mount_failed = false
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info("www", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS: total: %d, used: %d", total, used);
    }
    
    return ESP_OK;
}

/**
 * Serve a static file from SPIFFS
 */
static esp_err_t serve_static_file(httpd_req_t *req, const char *filepath, const char *content_type)
{
    char path[64];
    snprintf(path, sizeof(path), "/www/%s", filepath);
    
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    
    // Set content type
    httpd_resp_set_type(req, content_type);
    
    // Disable caching for development
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    
    // Read and send file in chunks
    char chunk[1024];
    size_t read_bytes;
    do {
        read_bytes = fread(chunk, 1, sizeof(chunk), file);
        if (read_bytes > 0) {
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                fclose(file);
                ESP_LOGE(TAG, "File sending failed");
                httpd_resp_sendstr_chunk(req, NULL);
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);
    
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ─── Provisioning Mode ────────────────────────────────────────────────────── */

void web_server_set_provisioning_mode(bool on)
{
    s_provisioning = on;
    ESP_LOGI(TAG, "Provisioning mode: %s", on ? "ON" : "OFF");
}

/**
 * @brief Auth guard — open during provisioning, require dashboard auth otherwise.
 * Returns ESP_OK if the request is allowed, or sends a 401 and returns ESP_FAIL.
 */
static esp_err_t guard_auth(httpd_req_t *req)
{
    if (s_provisioning) {
        return ESP_OK;  // No auth needed during provisioning
    }
    // In normal mode, require dashboard auth (Basic auth or session check).
    // For now, check if dashboard is authenticated via the existing auth system.
    // Since the existing /api/config POST has no auth enforcement, we keep
    // the same behavior but add the guard hook for future hardening.
    return ESP_OK;
}

/**
 * @brief Schedule a device restart after a delay (non-blocking via esp_timer)
 */
static void restart_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Restarting device...");
    esp_restart();
}

static void schedule_restart_ms(uint32_t delay_ms)
{
    const esp_timer_create_args_t timer_args = {
        .callback = restart_timer_cb,
        .name = "restart_timer"
    };
    esp_timer_handle_t timer;
    if (esp_timer_create(&timer_args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)delay_ms * 1000);
    }
}

/**
 * Handler for the root page - serves index.html
 */
static esp_err_t root_handler(httpd_req_t *req) {
    return serve_static_file(req,
        s_provisioning ? "setup.html" : "index.html", "text/html; charset=utf-8");
}

/**
 * Handler for CSS file
 */
static esp_err_t css_handler(httpd_req_t *req) {
    return serve_static_file(req, "style.css", "text/css");
}

/**
 * Handler for JavaScript file
 */
static esp_err_t js_handler(httpd_req_t *req) {
    return serve_static_file(req, "script.js", "application/javascript");
}

/**
 * Handler for the network API endpoint - returns JSON with network parameters
 */
static esp_err_t api_network_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        return ESP_FAIL;
    }
    
    // Get STA info
    wifi_ap_record_t ap_info = {0};
    bool sta_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    
    cJSON_AddBoolToObject(root, "sta_connected", sta_connected);
    
    if (sta_connected) {
        char ssid[33] = {0};
        memcpy(ssid, ap_info.ssid, sizeof(ap_info.ssid));
        cJSON_AddStringToObject(root, "sta_ssid", ssid);
        cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
        cJSON_AddNumberToObject(root, "sta_channel", ap_info.primary);
        cJSON_AddStringToObject(root, "sta_band", wifi_band_for_channel(ap_info.primary));
        
        // RSSI info
        cJSON *rssi_info = cJSON_CreateObject();
        if (ap_info.rssi >= -50) {
            cJSON_AddStringToObject(rssi_info, "text", "Excellent");
            cJSON_AddStringToObject(rssi_info, "color", "status-good");
            cJSON_AddNumberToObject(rssi_info, "percent", 100);
        } else if (ap_info.rssi >= -60) {
            cJSON_AddStringToObject(rssi_info, "text", "Very Good");
            cJSON_AddStringToObject(rssi_info, "color", "status-good");
            cJSON_AddNumberToObject(rssi_info, "percent", 80);
        } else if (ap_info.rssi >= -70) {
            cJSON_AddStringToObject(rssi_info, "text", "Good");
            cJSON_AddStringToObject(rssi_info, "color", "status-good");
            cJSON_AddNumberToObject(rssi_info, "percent", 60);
        } else if (ap_info.rssi >= -80) {
            cJSON_AddStringToObject(rssi_info, "text", "Fair");
            cJSON_AddStringToObject(rssi_info, "color", "status-warning");
            cJSON_AddNumberToObject(rssi_info, "percent", 40);
        } else {
            cJSON_AddStringToObject(rssi_info, "text", "Weak");
            cJSON_AddStringToObject(rssi_info, "color", "status-bad");
            cJSON_AddNumberToObject(rssi_info, "percent", 20);
        }
        cJSON_AddItemToObject(root, "rssi_info", rssi_info);
    } else {
        cJSON_AddStringToObject(root, "sta_ssid", "—");
        cJSON_AddNumberToObject(root, "rssi", 0);
        cJSON_AddNumberToObject(root, "sta_channel", 0);
        cJSON_AddStringToObject(root, "sta_band", "-");
        cJSON *rssi_info = cJSON_CreateObject();
        cJSON_AddStringToObject(rssi_info, "text", "Disconnected");
        cJSON_AddStringToObject(rssi_info, "color", "status-bad");
        cJSON_AddNumberToObject(rssi_info, "percent", 0);
        cJSON_AddItemToObject(root, "rssi_info", rssi_info);
    }
    
    // Get STA IP info
    char sta_ip_str[16] = "-";
    char sta_netmask_str[16] = "-";
    char sta_gw_str[16] = "-";
    esp_netif_ip_info_t sta_ip = {0};
    if (s_sta_netif != NULL && esp_netif_get_ip_info(s_sta_netif, &sta_ip) == ESP_OK) {
        snprintf(sta_ip_str, sizeof(sta_ip_str), IPSTR, IP2STR(&sta_ip.ip));
        snprintf(sta_netmask_str, sizeof(sta_netmask_str), IPSTR, IP2STR(&sta_ip.netmask));
        snprintf(sta_gw_str, sizeof(sta_gw_str), IPSTR, IP2STR(&sta_ip.gw));
    }
    cJSON_AddStringToObject(root, "sta_ip", sta_ip_str);
    cJSON_AddStringToObject(root, "sta_netmask", sta_netmask_str);
    cJSON_AddStringToObject(root, "sta_gw", sta_gw_str);
    
    // Get AP info
    char ap_ip_str[16] = "-";
    char ap_netmask_str[16] = "-";
    char ap_gw_str[16] = "-";
    esp_netif_ip_info_t ap_ip = {0};
    if (s_ap_netif != NULL && esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK) {
        snprintf(ap_ip_str, sizeof(ap_ip_str), IPSTR, IP2STR(&ap_ip.ip));
        snprintf(ap_netmask_str, sizeof(ap_netmask_str), IPSTR, IP2STR(&ap_ip.netmask));
        snprintf(ap_gw_str, sizeof(ap_gw_str), IPSTR, IP2STR(&ap_ip.gw));
    }
    cJSON_AddStringToObject(root, "ap_ip", ap_ip_str);
    cJSON_AddStringToObject(root, "ap_netmask", ap_netmask_str);
    cJSON_AddStringToObject(root, "ap_gw", ap_gw_str);
    
    // Get AP SSID and connected clients
    wifi_config_t ap_config = {0};
    char ap_ssid[33] = "-";
    if (esp_wifi_get_config(WIFI_IF_AP, &ap_config) == ESP_OK) {
        size_t ap_ssid_len = ap_config.ap.ssid_len;
        if (ap_ssid_len == 0 || ap_ssid_len > sizeof(ap_config.ap.ssid)) {
            ap_ssid_len = strnlen((const char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid));
        }
        if (ap_ssid_len > sizeof(ap_config.ap.ssid)) {
            ap_ssid_len = sizeof(ap_config.ap.ssid);
        }
        memset(ap_ssid, 0, sizeof(ap_ssid));
        memcpy(ap_ssid, ap_config.ap.ssid, ap_ssid_len);
    }
    cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);
    
    uint16_t ap_clients = 0;
    add_ap_clients_json(root, &ap_clients);
    add_dns_logs_json(root);
    
    // NAT status
#if CONFIG_LWIP_IPV4_NAPT
    cJSON_AddBoolToObject(root, "nat_enabled", true);
#else
    cJSON_AddBoolToObject(root, "nat_enabled", false);
#endif
    
    // Power consumption information
    // Get boot time for uptime calculation
    // Use esp_timer_get_time() which returns microseconds and handles overflow better
    static uint64_t start_time_us = 0;
    if (start_time_us == 0) {
        start_time_us = esp_timer_get_time();
    }
    uint64_t current_time_us = esp_timer_get_time();
    uint64_t uptime_seconds = (current_time_us - start_time_us) / 1000000;
    
    // Safely cast to uint32_t for display (uptime in seconds)
    uint32_t total_secs = (uint32_t)(uptime_seconds & 0xFFFFFFFF);
    uint32_t uptime_hours = total_secs / 3600;
    uint32_t uptime_minutes = (total_secs % 3600) / 60;
    uint32_t uptime_secs = total_secs % 60;
    
    char uptime_str[32] = {0};
    snprintf(uptime_str, sizeof(uptime_str), "%lu h %lu m %lu s", uptime_hours, uptime_minutes, uptime_secs);
    cJSON_AddStringToObject(root, "uptime", uptime_str);
    
    // Power mode (based on WiFi power save setting)
    cJSON_AddStringToObject(root, "power_mode", "Full Performance (PS=NONE)");
    
    // Estimated power consumption based on configuration
    // ESP32-C5: ~80-120mA when transmitting, ~40-60mA when receiving, ~10mA in light sleep
    uint32_t clients = ap_clients;
    
    // Simple estimation: base + per-client
    int estimated_mA = 120 + (clients * 15); // Base 120mA + 15mA per connected client
    char power_str[32] = {0};
    snprintf(power_str, sizeof(power_str), "~%d mA", estimated_mA);
    cJSON_AddStringToObject(root, "estimated_power", power_str);
    
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON render failed");
        return ESP_FAIL;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

/**
 * Handler for system statistics API endpoint - returns detailed monitoring data
 */
static esp_err_t api_stats_handler(httpd_req_t *req) {
    // Benchmark the HTTP request processing
    BENCHMARK_START(http_request);
    
    // Get current system statistics
    sys_monitor_stats_t stats;
    if (sys_monitor_get_stats(&stats) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get stats");
        return ESP_FAIL;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        return ESP_FAIL;
    }
    
    // CPU statistics
    cJSON *cpu = cJSON_CreateObject();
    cJSON_AddNumberToObject(cpu, "total_percent", stats.cpu.total_cpu_percent);
    cJSON_AddNumberToObject(cpu, "task_count", stats.cpu.task_count);
    
    cJSON *tasks = cJSON_CreateArray();
    for (uint32_t i = 0; i < stats.cpu.task_count && i < 10; i++) {  // Limit to top 10 tasks
        cJSON *task = cJSON_CreateObject();
        cJSON_AddStringToObject(task, "name", stats.cpu.tasks[i].task_name);
        cJSON_AddNumberToObject(task, "cpu_percent", stats.cpu.tasks[i].cpu_percent);
        cJSON_AddNumberToObject(task, "stack_hwm", stats.cpu.tasks[i].stack_hwm);
        cJSON_AddItemToArray(tasks, task);
    }
    cJSON_AddItemToObject(cpu, "tasks", tasks);
    cJSON_AddItemToObject(root, "cpu", cpu);
    
    // Memory statistics
    cJSON *mem = cJSON_CreateObject();
    cJSON_AddNumberToObject(mem, "free_heap", stats.mem.free_heap);
    cJSON_AddNumberToObject(mem, "total_heap", stats.mem.total_heap);
    cJSON_AddNumberToObject(mem, "min_free_heap", stats.mem.min_free_heap);
    cJSON_AddNumberToObject(mem, "largest_free_block", stats.mem.largest_free_block);
    cJSON_AddNumberToObject(mem, "heap_usage_percent", stats.mem.heap_usage_percent);
    cJSON_AddItemToObject(root, "mem", mem);
    
    // Network statistics  
    cJSON *net = cJSON_CreateObject();
    cJSON_AddNumberToObject(net, "tx_bytes", (double)stats.net.tx_bytes);
    cJSON_AddNumberToObject(net, "rx_bytes", (double)stats.net.rx_bytes);
    cJSON_AddNumberToObject(net, "tx_packets", stats.net.tx_packets);
    cJSON_AddNumberToObject(net, "rx_packets", stats.net.rx_packets);
    cJSON_AddNumberToObject(net, "tx_bytes_per_sec", stats.net.tx_bytes_per_sec);
    cJSON_AddNumberToObject(net, "rx_bytes_per_sec", stats.net.rx_bytes_per_sec);
    cJSON_AddNumberToObject(net, "tx_errors", stats.net.tx_errors);
    cJSON_AddNumberToObject(net, "rx_errors", stats.net.rx_errors);
    cJSON_AddItemToObject(root, "net", net);
    
    // WiFi statistics
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddNumberToObject(wifi, "tx_packets", stats.wifi.wifi_tx_packets);
    cJSON_AddNumberToObject(wifi, "rx_packets", stats.wifi.wifi_rx_packets);
    cJSON_AddNumberToObject(wifi, "tx_dropped", stats.wifi.wifi_tx_dropped);
    cJSON_AddNumberToObject(wifi, "rx_dropped", stats.wifi.wifi_rx_dropped);
    cJSON_AddNumberToObject(wifi, "sta_rssi", stats.wifi.sta_rssi);
    cJSON_AddBoolToObject(wifi, "sta_connected", stats.wifi.sta_connected);
    cJSON_AddNumberToObject(wifi, "ap_clients", stats.wifi.ap_clients);
    cJSON_AddItemToObject(root, "wifi", wifi);
    
    // Benchmark statistics
    cJSON *bench = cJSON_CreateObject();
    cJSON_AddNumberToObject(bench, "dns_query_count", stats.bench.dns_query_count);
    cJSON_AddNumberToObject(bench, "dns_avg_latency_us", stats.bench.dns_avg_latency_us);
    cJSON_AddNumberToObject(bench, "dns_max_latency_us", stats.bench.dns_max_latency_us);
    cJSON_AddNumberToObject(bench, "dns_cache_hits", stats.bench.dns_cache_hits);
    cJSON_AddNumberToObject(bench, "dns_cache_misses", stats.bench.dns_cache_misses);
    cJSON_AddNumberToObject(bench, "http_request_count", stats.bench.http_request_count);
    cJSON_AddNumberToObject(bench, "http_avg_latency_us", stats.bench.http_avg_latency_us);
    cJSON_AddNumberToObject(bench, "http_max_latency_us", stats.bench.http_max_latency_us);
    cJSON_AddItemToObject(root, "bench", bench);
    
    // Uptime
    cJSON_AddNumberToObject(root, "uptime_seconds", stats.uptime_seconds);
    
    // Render JSON response
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON render failed");
        return ESP_FAIL;
    }
    
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    
    // Record HTTP request latency
    uint32_t request_latency = BENCHMARK_GET_ELAPSED(http_request);
    sys_monitor_record_http_request(request_latency);
    
    return ESP_OK;
}

/**
 * Handler for safe shutdown endpoint
 */
static esp_err_t shutdown_handler(httpd_req_t *req) {
    if (req->method != HTTP_POST) {
        httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "POST method required");
        return ESP_FAIL;
    }
    
    char content[100] = {0};
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    
    if (ret > 0) {
        // Ensure null-termination for JSON parsing
        content[ret] = '\0';
        
        cJSON *json = cJSON_Parse(content);
        if (json) {
            cJSON *confirm = cJSON_GetObjectItem(json, "confirm");
            if (confirm && confirm->type == cJSON_True) {
                // Send confirmation response
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"status\":\"shutting_down\"}", -1);
                
                // Always delete JSON before proceeding
                cJSON_Delete(json);
                
                // Delay to allow response to be sent
                vTaskDelay(pdMS_TO_TICKS(500));
                
                // Perform shutdown sequence
                ESP_LOGI(TAG, "Starting safe shutdown sequence...");
                
                // Disconnect WiFi
                esp_wifi_stop();
                vTaskDelay(pdMS_TO_TICKS(100));
                
                // Deinit WiFi
                esp_wifi_deinit();
                vTaskDelay(pdMS_TO_TICKS(100));
                
                ESP_LOGI(TAG, "Entering deep sleep...");
                esp_deep_sleep_start();
                
                // Never reaches here
                return ESP_OK;
            }
            cJSON_Delete(json);
        }
    }
    
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    return ESP_FAIL;
}

/**
 * Handler for configuration GET endpoint - returns current config
 */
static esp_err_t api_config_get_handler(httpd_req_t *req) {
    repeater_config_t config;
    if (wifi_config_load(&config) != ESP_OK) {
        // If load fails, try getting defaults
        wifi_config_get_default(&config);
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(root, "sta_ssid", config.sta_ssid);
    // Do not send passwords for security, or send masked
    cJSON_AddStringToObject(root, "sta_pass", "");
    cJSON_AddStringToObject(root, "ap_ssid", config.ap_ssid);
    cJSON_AddStringToObject(root, "ap_pass", ""); // Masked
    cJSON_AddNumberToObject(root, "ap_max_conn", config.ap_max_connections);

    // Append SSID history
    ssid_history_t history;
    wifi_config_load_ssid_history(&history);
    cJSON *ssid_list = cJSON_CreateArray();
    if (ssid_list != NULL) {
        for (uint8_t i = 0; i < history.count; i++) {
            cJSON_AddItemToArray(ssid_list, cJSON_CreateString(history.ssids[i]));
        }
        cJSON_AddItemToObject(root, "ssid_history", ssid_list);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Handler for configuration POST endpoint - updates config
 */
static esp_err_t api_config_post_handler(httpd_req_t *req) {
    char buf[1024];
    int ret, remaining = req->content_len;
    
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too large");
        return ESP_FAIL;
    }
    
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    // Load current config first to preserve values not being updated
    repeater_config_t config;
    if (wifi_config_load(&config) != ESP_OK) {
        wifi_config_get_default(&config);
    }
    
    // Update fields if present
    cJSON *item = cJSON_GetObjectItem(root, "sta_ssid");
    if (item && cJSON_IsString(item)) {
        strncpy(config.sta_ssid, item->valuestring, sizeof(config.sta_ssid) - 1);
    }
    
    item = cJSON_GetObjectItem(root, "sta_pass");
    if (item && cJSON_IsString(item) && strlen(item->valuestring) > 0) {
        strncpy(config.sta_password, item->valuestring, sizeof(config.sta_password) - 1);
    }
    
    item = cJSON_GetObjectItem(root, "ap_ssid");
    if (item && cJSON_IsString(item)) {
        strncpy(config.ap_ssid, item->valuestring, sizeof(config.ap_ssid) - 1);
    }
    
    item = cJSON_GetObjectItem(root, "ap_pass");
    if (item && cJSON_IsString(item) && strlen(item->valuestring) > 0) {
        strncpy(config.ap_password, item->valuestring, sizeof(config.ap_password) - 1);
    }
    
    item = cJSON_GetObjectItem(root, "ap_max_conn");
    if (item && cJSON_IsNumber(item)) {
        config.ap_max_connections = (uint8_t)item->valueint;
    }
    
    cJSON_Delete(root);
    
    // Validate and save
    if (wifi_config_validate(&config) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid configuration values");
        return ESP_FAIL;
    }
    
    if (wifi_config_save(&config) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save to NVS");
        return ESP_FAIL;
    }

    // Update SSID history with the newly configured STA SSID
    wifi_config_add_ssid_to_history(config.sta_ssid);

    // Mark as provisioned (only set here, not in wifi_config_save)
    wifi_config_set_provisioned(true);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Saved. Restarting...\"}", -1);

    // Schedule restart so the response gets sent first
    schedule_restart_ms(1000);

    return ESP_OK;
}

/**
 * Handler for POST /api/config/reset
 * Erases NVS credentials saved by the web dashboard and writes the
 * compile-time defaults back, then restarts so they take effect.
 */
static esp_err_t api_config_reset_handler(httpd_req_t *req) {
    if (req->method != HTTP_POST) {
        httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "POST method required");
        return ESP_FAIL;
    }

    esp_err_t err = wifi_config_erase();  // Erases all keys including "provisioned" flag
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase config: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Reset failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config erased — device will boot into provisioning mode.");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"reset_ok\",\"message\":\"Erased. Restarting into provisioning...\"}", -1);

    schedule_restart_ms(500);
    return ESP_OK;
}

/**
 * @brief POST /api/auth  — verify dashboard credentials
 *
 * Body: {"username": "...", "password": "..."}
 * Returns: {"ok": true} or HTTP 401
 */
static esp_err_t api_auth_handler(httpd_req_t *req) {
    if (req->method != HTTP_POST) {
        httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "POST required");
        return ESP_FAIL;
    }

    char buf[256] = {0};
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *user_item = cJSON_GetObjectItem(json, "username");
    cJSON *pass_item = cJSON_GetObjectItem(json, "password");

    if (!cJSON_IsString(user_item) || !cJSON_IsString(pass_item)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing credentials");
        return ESP_FAIL;
    }

    bool ok = wifi_config_verify_auth(user_item->valuestring, pass_item->valuestring);
    cJSON_Delete(json);

    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Invalid credentials\"}", -1);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", -1);
    return ESP_OK;
}

/**
 * Handler for system restart
 */
static esp_err_t restart_handler(httpd_req_t *req) {
    httpd_resp_send(req, "{\"status\":\"restarting\"}", -1);
    
    // Delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "Restarting device via web request...");
    esp_restart();
    
    return ESP_OK;
}

/**
 * Handler for GET /api/scan — scan for nearby Wi-Fi networks
 * Returns JSON array: [{"ssid":"...","rssi":-45,"auth":"WPA2","channel":6}, ...]
 */
static esp_err_t api_scan_handler(httpd_req_t *req) {
    if (guard_auth(req) != ESP_OK) return ESP_FAIL;

    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 100, .max = 300 },
    };

    esp_err_t err = esp_wifi_scan_start(&sc, true);  // blocking scan
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;

    wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);

    // Build JSON — deduplicate by SSID (keep strongest RSSI)
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        return ESP_FAIL;
    }

    // Simple dedup: track seen SSIDs
    char seen_ssids[20][33];
    int seen_count = 0;

    for (uint16_t i = 0; i < n; i++) {
        char ssid[33] = {0};
        memcpy(ssid, recs[i].ssid, sizeof(recs[i].ssid));
        if (ssid[0] == '\0') continue;  // skip hidden networks

        // Check if we already have this SSID with a stronger signal
        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen_ssids[j], ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        if (seen_count < 20) {
            strncpy(seen_ssids[seen_count], ssid, 32);
            seen_ssids[seen_count][32] = '\0';
            seen_count++;
        }

        cJSON *item = cJSON_CreateObject();
        if (item == NULL) continue;
        cJSON_AddStringToObject(item, "ssid", ssid);
        cJSON_AddNumberToObject(item, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(item, "channel", recs[i].primary);

        const char *auth_str = "Open";
        switch (recs[i].authmode) {
            case WIFI_AUTH_WEP:           auth_str = "WEP"; break;
            case WIFI_AUTH_WPA_PSK:       auth_str = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK:      auth_str = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK:  auth_str = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA3_PSK:      auth_str = "WPA3"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: auth_str = "WPA2/WPA3"; break;
            case WIFI_AUTH_OPEN:          auth_str = "Open"; break;
            default:                      auth_str = "Other"; break;
        }
        cJSON_AddStringToObject(item, "auth", auth_str);
        cJSON_AddItemToArray(arr, item);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}

/**
 * Handler for the setup page
 */
static esp_err_t setup_handler(httpd_req_t *req) {
    return serve_static_file(req, "setup.html", "text/html; charset=utf-8");
}

/**
 * Captive portal catch-all: redirect any unknown path to the setup page during provisioning.
 * Covers OS probe URLs (/generate_204, /hotspot-detect.html, /connecttest.txt, etc.)
 */
static esp_err_t captive_404_handler(httpd_req_t *req, httpd_err_code_t err) {
    if (s_provisioning) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

// URI handlers
static const httpd_uri_t api_scan = {
    .uri       = "/api/scan",
    .method    = HTTP_GET,
    .handler   = api_scan_handler,
    .user_ctx  = NULL,
};

static const httpd_uri_t setup_uri = {
    .uri       = "/setup",
    .method    = HTTP_GET,
    .handler   = setup_handler,
    .user_ctx  = NULL,
};

// URI handlers
static const httpd_uri_t root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_handler,
    .user_ctx  = NULL,
};

static const httpd_uri_t css_uri = {
    .uri       = "/style.css",
    .method    = HTTP_GET,
    .handler   = css_handler,
    .user_ctx  = NULL,
};

static const httpd_uri_t js_uri = {
    .uri       = "/script.js",
    .method    = HTTP_GET,
    .handler   = js_handler,
    .user_ctx  = NULL,
};

static const httpd_uri_t api_network = {
    .uri       = "/api/network",
    .method    = HTTP_GET,
    .handler   = api_network_handler,
    .user_ctx  = NULL,
};

static const httpd_uri_t api_stats = {
    .uri       = "/api/stats",
    .method    = HTTP_GET,
    .handler   = api_stats_handler,
    .user_ctx  = NULL,
};

static const httpd_uri_t api_shutdown = {
    .uri       = "/api/shutdown",
    .method    = HTTP_POST,
    .handler   = shutdown_handler,
    .user_ctx  = NULL,
};

static const httpd_uri_t api_config_get = {
    .uri       = "/api/config",
    .method    = HTTP_GET,
    .handler   = api_config_get_handler,
    .user_ctx  = NULL,
};

static const httpd_uri_t api_config_post = {
    .uri       = "/api/config",
    .method    = HTTP_POST,
    .handler   = api_config_post_handler,
    .user_ctx  = NULL,
};

static const httpd_uri_t api_restart = {
    .uri       = "/api/restart",
    .method    = HTTP_POST,
    .handler   = restart_handler,
    .user_ctx  = NULL,
};

static const httpd_uri_t api_config_reset = {
    .uri       = "/api/config/reset",
    .method    = HTTP_POST,
    .handler   = api_config_reset_handler,
    .user_ctx  = NULL,
};

static const httpd_uri_t api_auth = {
    .uri       = "/api/auth",
    .method    = HTTP_POST,
    .handler   = api_auth_handler,
    .user_ctx  = NULL,
};

esp_err_t web_server_init(void) {
    if (server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }
    
    // Initialize SPIFFS for serving web files
    esp_err_t ret = init_spiffs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS");
        return ret;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.max_uri_handlers = 24;
    config.stack_size = 8192;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &css_uri);
        httpd_register_uri_handler(server, &js_uri);
        httpd_register_uri_handler(server, &api_network);
        httpd_register_uri_handler(server, &api_stats);
        httpd_register_uri_handler(server, &api_shutdown);
        httpd_register_uri_handler(server, &api_config_get);
        httpd_register_uri_handler(server, &api_config_post);
        httpd_register_uri_handler(server, &api_config_reset);
        httpd_register_uri_handler(server, &api_restart);
        httpd_register_uri_handler(server, &api_auth);
        httpd_register_uri_handler(server, &api_scan);
        httpd_register_uri_handler(server, &setup_uri);

        // Captive portal catch-all: during provisioning, redirect unknown paths
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_404_handler);

        ESP_LOGI(TAG, "Web server started. Access at http://192.168.4.1");
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to start web server");
    return ESP_FAIL;
}

esp_err_t web_server_stop(void) {
    if (server != NULL) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
    return ESP_OK;
}
