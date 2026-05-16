/**
 * sys_monitor.c - System Monitoring and Benchmarking Implementation
 * 
 * This module implements comprehensive system monitoring for the ESP32 WiFi repeater.
 * It collects CPU usage, memory statistics, network throughput, WiFi metrics, and
 * benchmark timing data. All metrics are logged periodically and available via API.
 */

#include "sys_monitor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/stats.h"
#include <string.h>

static const char *TAG = "sys_monitor";

// External references from main.c
extern esp_netif_t *s_sta_netif;
extern esp_netif_t *s_ap_netif;

// Internal state
static sys_monitor_stats_t g_stats = {0};  // Current statistics
static bool g_initialized = false;        // Initialization flag
static TaskHandle_t g_monitor_task = NULL; // Monitoring task handle

// Previous values for delta calculations
static uint64_t g_prev_tx_bytes = 0;
static uint64_t g_prev_rx_bytes = 0;
static int64_t g_prev_sample_time = 0;
static uint64_t g_start_time_us = 0;

// Benchmark accumulators
static uint32_t g_dns_total_latency = 0;
static uint32_t g_http_total_latency = 0;

// Forward declarations
static void update_cpu_stats(void);
static void update_mem_stats(void);
static void update_net_stats(void);
static void update_wifi_stats(void);
static void monitor_task(void *arg);

// ===== Initialization =====

esp_err_t sys_monitor_init(void)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "System monitor already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing system monitoring...");
    
    // Clear statistics
    memset(&g_stats, 0, sizeof(g_stats));
    
    // Record start time
    g_start_time_us = esp_timer_get_time();
    g_prev_sample_time = g_start_time_us;
    
    // Create monitoring task
    BaseType_t ret = xTaskCreate(
        monitor_task,
        "sys_monitor",
        4096,
        NULL,
        3,  // Lower priority
        &g_monitor_task
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitoring task");
        return ESP_FAIL;
    }
    
    g_initialized = true;
    ESP_LOGI(TAG, "System monitoring initialized successfully");
    
    return ESP_OK;
}

// ===== CPU Statistics =====

/**
 * Update CPU statistics using FreeRTOS runtime stats
 * 
 * This calculates CPU usage per task and overall system CPU usage.
 * Requires configGENERATE_RUN_TIME_STATS=1 in FreeRTOS configuration.
 * If not enabled, CPU stats will be set to 0.
 */
static void update_cpu_stats(void)
{
#if configGENERATE_RUN_TIME_STATS
    // Get task count
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    
    if (task_count > 16) {
        task_count = 16;  // Limit to our array size
    }
    
    g_stats.cpu.task_count = task_count;
    
    // Allocate buffer for task status array
    TaskStatus_t *task_status_array = pvPortMalloc(task_count * sizeof(TaskStatus_t));
    if (task_status_array == NULL) {
        ESP_LOGW(TAG, "Failed to allocate memory for task status");
        return;
    }
    
    // Get task runtime statistics
    uint32_t total_runtime = 0;
    UBaseType_t actual_count = uxTaskGetSystemState(task_status_array, task_count, &total_runtime);
    
    // Calculate per-task CPU usage
    uint32_t total_cpu = 0;
    for (UBaseType_t i = 0; i < actual_count && i < 16; i++) {
        // Copy task name
        strncpy(g_stats.cpu.tasks[i].task_name, task_status_array[i].pcTaskName, 15);
        g_stats.cpu.tasks[i].task_name[15] = '\0';
        
        // Store runtime counter
        g_stats.cpu.tasks[i].runtime_counter = task_status_array[i].ulRunTimeCounter;
        
        // Calculate CPU percentage (avoid division by zero)
        if (total_runtime > 0) {
            g_stats.cpu.tasks[i].cpu_percent = 
                (uint8_t)((task_status_array[i].ulRunTimeCounter * 100UL) / total_runtime);
        } else {
            g_stats.cpu.tasks[i].cpu_percent = 0;
        }
        
        // Get stack high watermark (bytes remaining)
        g_stats.cpu.tasks[i].stack_hwm = task_status_array[i].usStackHighWaterMark * sizeof(StackType_t);
        
        if (strncmp(g_stats.cpu.tasks[i].task_name, "IDLE", 4) != 0) {
             total_cpu += g_stats.cpu.tasks[i].cpu_percent;
        }
    }
    
    // Total CPU percentage (cap at 100)
    g_stats.cpu.total_cpu_percent = (total_cpu > 100) ? 100 : total_cpu;
    
    vPortFree(task_status_array);
#else
    // FreeRTOS runtime stats not enabled - set to 0
    // To enable: run 'idf.py menuconfig' -> Component config -> FreeRTOS -> 
    // Kernel -> Enable FreeRTOS trace facility and Enable FreeRTOS stats formatting functions
    g_stats.cpu.task_count = 0;
    g_stats.cpu.total_cpu_percent = 0;
    ESP_LOGW(TAG, "CPU stats disabled - configGENERATE_RUN_TIME_STATS not enabled in FreeRTOS config");
#endif
}

// ===== Memory Statistics =====

/**
 * Update memory statistics (heap usage)
 * 
 * This queries the ESP32 heap allocator for current memory usage.
 */
static void update_mem_stats(void)
{
    // Get free heap size
    g_stats.mem.free_heap = esp_get_free_heap_size();
    
    // Get minimum free heap (lowest point ever)
    g_stats.mem.min_free_heap = esp_get_minimum_free_heap_size();
    
    // Get largest free block
    g_stats.mem.largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    
    // Calculate total heap (approximate)
    // For ESP32, total heap is typically around 300KB, but varies by model
    // We'll estimate it as free + used
    static uint32_t total_heap = 0;
    if (total_heap == 0) {
        // First time: estimate total heap from initial free heap
        total_heap = g_stats.mem.free_heap + 32768;  // Add some margin for used heap at startup
    }
    g_stats.mem.total_heap = total_heap;
    
    // Calculate heap usage percentage
    uint32_t used_heap = total_heap - g_stats.mem.free_heap;
    g_stats.mem.heap_usage_percent = (used_heap * 100) / total_heap;
    
    if (g_stats.mem.heap_usage_percent > 100) {
        g_stats.mem.heap_usage_percent = 100;
    }
}

/**
 * Update network statistics (throughput and packet counts)
 * 
 * This reads lwIP statistics when available and calculates throughput.
 * Note: Requires LWIP_STATS to be enabled for accurate monitoring.
 */
static void update_net_stats(void)
{
    // Get current time for throughput calculation
    int64_t current_time = esp_timer_get_time();
    int64_t time_delta_us = current_time - g_prev_sample_time;
    
    // Use esp_wifi_internal_reg_rxcb or similar is not exposed publicly
    // But we can iterate over netifs to sum up stats if available
    // Standard LWIP stats are global. Let's try to get them from netif list if LWIP_STATS is on
    
    uint64_t current_tx_bytes = 0;
    uint64_t current_rx_bytes = 0;
    uint32_t current_tx_packets = 0;
    uint32_t current_rx_packets = 0;
    uint32_t current_tx_errors = 0;
    uint32_t current_rx_errors = 0;

#if LWIP_STATS
    // Use global link stats if confirmed working, or IP stats as fallback
    // IP stats usually work better as they are counted by the stack
    current_tx_packets = lwip_stats.ip.xmit;
    current_rx_packets = lwip_stats.ip.recv;
    current_tx_errors = lwip_stats.ip.drop; // IP doesn't have err for TX usually, mostly drop
    current_rx_errors = lwip_stats.ip.chkerr + lwip_stats.ip.lenerr + lwip_stats.ip.memerr + lwip_stats.ip.rterr + lwip_stats.ip.proterr + lwip_stats.ip.opterr + lwip_stats.ip.err + lwip_stats.ip.drop;

    // Estimate bytes from IP packets (avg 800 bytes? - heuristic)
    current_tx_bytes = (uint64_t)current_tx_packets * 800;
    current_rx_bytes = (uint64_t)current_rx_packets * 800;
    
    // Try to get more accurate byte counts from MIB2 stats if enabled
    // But direct byte counters are often missing in lightweight stacks
#endif

    g_stats.net.tx_packets = current_tx_packets;
    g_stats.net.rx_packets = current_rx_packets;
    g_stats.net.tx_errors = current_tx_errors;
    g_stats.net.rx_errors = current_rx_errors;
    
    // Update cumulative byte counts
    g_stats.net.tx_bytes = current_tx_bytes;
    g_stats.net.rx_bytes = current_rx_bytes;
    
    // Calculate throughput (bytes per second)
    if (time_delta_us > 0 && g_prev_sample_time > 0) {
        // Handle wrap-around or reset
        uint64_t tx_delta = (current_tx_bytes >= g_prev_tx_bytes) ? 
                           (current_tx_bytes - g_prev_tx_bytes) : 0;
        uint64_t rx_delta = (current_rx_bytes >= g_prev_rx_bytes) ? 
                           (current_rx_bytes - g_prev_rx_bytes) : 0;
        
        // Convert to bytes/sec: (bytes * 1,000,000) / microseconds
        if (time_delta_us > 1000) { // Avoid division by zero or tiny deltas
            g_stats.net.tx_bytes_per_sec = (uint32_t)((tx_delta * 1000000ULL) / time_delta_us);
            g_stats.net.rx_bytes_per_sec = (uint32_t)((rx_delta * 1000000ULL) / time_delta_us);
        }
    }
    
    // Update previous values for next delta calculation
    g_prev_tx_bytes = current_tx_bytes;
    g_prev_rx_bytes = current_rx_bytes;
    g_prev_sample_time = current_time;
}

// ===== WiFi Statistics =====

/**
 * Update WiFi-specific statistics
 * 
 * This queries the WiFi driver for connection state and signal strength.
 * Note: ESP-IDF doesn't provide detailed packet-level WiFi stats via a direct API,
 * so we focus on connection state and RSSI.
 */
static void update_wifi_stats(void)
{
    // Note: ESP-IDF doesn't expose WiFi TX/RX packet counters directly
    // We set these to 0 as they're not available
    g_stats.wifi.wifi_tx_packets = 0;
    g_stats.wifi.wifi_rx_packets = 0;
    g_stats.wifi.wifi_tx_dropped = 0;
    g_stats.wifi.wifi_rx_dropped = 0;
    
    // Get STA connection status and RSSI
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        g_stats.wifi.sta_connected = 1;
        g_stats.wifi.sta_rssi = ap_info.rssi;
    } else {
        g_stats.wifi.sta_connected = 0;
        g_stats.wifi.sta_rssi = 0;
    }
    
    // Get number of connected AP clients
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        g_stats.wifi.ap_clients = sta_list.num;
    } else {
        g_stats.wifi.ap_clients = 0;
    }
}

// ===== Main Update Function =====

esp_err_t sys_monitor_update(void)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Update uptime
    uint64_t current_time_us = esp_timer_get_time();
    g_stats.uptime_seconds = (uint32_t)((current_time_us - g_start_time_us) / 1000000ULL);
    
    // Update all statistics
    update_cpu_stats();
    update_mem_stats();
    update_net_stats();
    update_wifi_stats();
    
    return ESP_OK;
}

// ===== Statistics Retrieval =====

esp_err_t sys_monitor_get_stats(sys_monitor_stats_t *stats)
{
    if (!g_initialized || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Copy current statistics to output
    memcpy(stats, &g_stats, sizeof(sys_monitor_stats_t));
    
    return ESP_OK;
}

// ===== Logging =====

void sys_monitor_log_stats(void)
{
    if (!g_initialized) {
        return;
    }
    
    ESP_LOGI(TAG, "========== System Monitor Report ==========");
    
    // CPU Statistics
    ESP_LOGI(TAG, "CPU: Total usage: %u%%", g_stats.cpu.total_cpu_percent);
    ESP_LOGI(TAG, "CPU: Active tasks: %lu", g_stats.cpu.task_count);
    for (uint32_t i = 0; i < g_stats.cpu.task_count && i < 5; i++) {
        ESP_LOGI(TAG, "  Task '%s': CPU=%u%%, Stack HWM=%lu bytes",
                 g_stats.cpu.tasks[i].task_name,
                 g_stats.cpu.tasks[i].cpu_percent,
                 g_stats.cpu.tasks[i].stack_hwm);
    }
    
    // Memory Statistics
    ESP_LOGI(TAG, "MEM: Free heap: %lu bytes (%.1f%% used)",
             g_stats.mem.free_heap,
             (float)g_stats.mem.heap_usage_percent);
    ESP_LOGI(TAG, "MEM: Largest free block: %lu bytes, Min free ever: %lu bytes",
             g_stats.mem.largest_free_block,
             g_stats.mem.min_free_heap);
    
    // Network Statistics
    ESP_LOGI(TAG, "NET: TX: %lu pkt, %llu bytes, %lu B/s | RX: %lu pkt, %llu bytes, %lu B/s",
             g_stats.net.tx_packets,
             g_stats.net.tx_bytes,
             g_stats.net.tx_bytes_per_sec,
             g_stats.net.rx_packets,
             g_stats.net.rx_bytes,
             g_stats.net.rx_bytes_per_sec);
    ESP_LOGI(TAG, "NET: Errors - TX: %lu, RX: %lu",
             g_stats.net.tx_errors,
             g_stats.net.rx_errors);
    
    // WiFi Statistics
    ESP_LOGI(TAG, "WiFi: STA %s (RSSI: %d dBm), AP clients: %u",
             g_stats.wifi.sta_connected ? "connected" : "disconnected",
             g_stats.wifi.sta_rssi,
             g_stats.wifi.ap_clients);
    ESP_LOGI(TAG, "WiFi: TX: %lu pkt (%lu dropped), RX: %lu pkt (%lu dropped)",
             g_stats.wifi.wifi_tx_packets,
             g_stats.wifi.wifi_tx_dropped,
             g_stats.wifi.wifi_rx_packets,
             g_stats.wifi.wifi_rx_dropped);
    
    // Benchmark Statistics
    if (g_stats.bench.dns_query_count > 0) {
        ESP_LOGI(TAG, "BENCH: DNS queries: %lu, avg latency: %lu us, max: %lu us",
                 g_stats.bench.dns_query_count,
                 g_stats.bench.dns_avg_latency_us,
                 g_stats.bench.dns_max_latency_us);
        ESP_LOGI(TAG, "BENCH: DNS usage - Cache HITS: %lu, MISSES: %lu",
                 g_stats.bench.dns_cache_hits,
                 g_stats.bench.dns_cache_misses);
    }
    if (g_stats.bench.http_request_count > 0) {
        ESP_LOGI(TAG, "BENCH: HTTP requests: %lu, avg latency: %lu us, max: %lu us",
                 g_stats.bench.http_request_count,
                 g_stats.bench.http_avg_latency_us,
                 g_stats.bench.http_max_latency_us);
    }
    
    ESP_LOGI(TAG, "Uptime: %lu seconds", g_stats.uptime_seconds);
    ESP_LOGI(TAG, "===========================================");
    printf("\n"); // Visual separator for readability
}

// ===== Benchmark Recording =====

void sys_monitor_record_dns_query(uint32_t latency_us)
{
    g_stats.bench.dns_query_count++;
    g_dns_total_latency += latency_us;
    
    // Update average
    g_stats.bench.dns_avg_latency_us = g_dns_total_latency / g_stats.bench.dns_query_count;
    
    // Update max
    if (latency_us > g_stats.bench.dns_max_latency_us) {
        g_stats.bench.dns_max_latency_us = latency_us;
    }
    
    // Log slow queries
    if (latency_us > 100000) {  // >100ms
        ESP_LOGW(TAG, "Slow DNS query detected: %lu ms", latency_us / 1000);
    }
}

void sys_monitor_record_http_request(uint32_t latency_us)
{
    g_stats.bench.http_request_count++;
    g_http_total_latency += latency_us;
    
    // Update average
    g_stats.bench.http_avg_latency_us = g_http_total_latency / g_stats.bench.http_request_count;
    
    // Update max
    if (latency_us > g_stats.bench.http_max_latency_us) {
        g_stats.bench.http_max_latency_us = latency_us;
    }
    
    // Log slow requests
    if (latency_us > 50000) {  // >50ms
        ESP_LOGW(TAG, "Slow HTTP request detected: %lu ms", latency_us / 1000);
    }
}

void sys_monitor_record_dns_cache_hit(void)
{
    g_stats.bench.dns_cache_hits++;
}

void sys_monitor_record_dns_cache_miss(void)
{
    g_stats.bench.dns_cache_misses++;
}

// ===== Monitoring Task =====

/**
 * Background task that periodically updates and logs system statistics
 * 
 * This task runs every 5 seconds and logs all monitoring data.
 */
static void monitor_task(void *arg)
{
    ESP_LOGI(TAG, "System monitoring task started");
    
    while (1) {
        // Wait 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        
        // Update all statistics
        sys_monitor_update();
        
        // Log to console
        sys_monitor_log_stats();
    }
}
