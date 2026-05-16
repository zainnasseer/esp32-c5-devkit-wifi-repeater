/**
 * sys_monitor.h - System Monitoring and Benchmarking for ESP32 WiFi Repeater
 * 
 * This module provides comprehensive system monitoring including:
 * - CPU usage tracking (per-task and overall)
 * - Memory monitoring (heap usage, free memory, largest block)
 * - Network statistics (throughput, packet counts)
 * - WiFi statistics (TX/RX packets, errors, retries)
 * - Benchmarking utilities for measuring operation latency
 * 
 * Usage:
 *   1. Call sys_monitor_init() during application startup
 *   2. Metrics are automatically collected and logged every 5 seconds
 *   3. Call sys_monitor_get_stats() to retrieve current metrics
 *   4. Use BENCHMARK_START/BENCHMARK_END macros for timing operations
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

// ===== CPU Statistics =====

/**
 * Per-task CPU statistics
 */
typedef struct {
    char task_name[16];        // FreeRTOS task name
    uint32_t runtime_counter;  // Task runtime in ticks
    uint8_t cpu_percent;       // CPU usage percentage (0-100)
    uint32_t stack_hwm;        // Stack high watermark (bytes remaining)
} sys_monitor_task_stats_t;

/**
 * Overall CPU statistics
 */
typedef struct {
    uint8_t total_cpu_percent;    // Total CPU usage (0-100)
    uint32_t task_count;          // Number of active tasks
    sys_monitor_task_stats_t tasks[16];  // Per-task stats (max 16 tasks)
} sys_monitor_cpu_stats_t;

// ===== Memory Statistics =====

/**
 * Heap memory statistics
 */
typedef struct {
    uint32_t free_heap;           // Current free heap (bytes)
    uint32_t total_heap;          // Total heap size (bytes)
    uint32_t min_free_heap;       // Minimum free heap ever (bytes)
    uint32_t largest_free_block;  // Largest contiguous free block (bytes)
    uint8_t heap_usage_percent;   // Heap usage percentage (0-100)
} sys_monitor_mem_stats_t;

// ===== Network Statistics =====

/**
 * Network throughput and packet statistics
 */
typedef struct {
    uint64_t tx_bytes;            // Total transmitted bytes
    uint64_t rx_bytes;            // Total received bytes
    uint32_t tx_packets;          // Total transmitted packets
    uint32_t rx_packets;          // Total received packets
    uint32_t tx_bytes_per_sec;    // Current TX throughput (bytes/sec)
    uint32_t rx_bytes_per_sec;    // Current RX throughput (bytes/sec)
    uint32_t tx_errors;           // Transmission errors
    uint32_t rx_errors;           // Reception errors
} sys_monitor_net_stats_t;

// ===== WiFi Statistics =====

/**
 * WiFi-specific statistics
 */
typedef struct {
    uint32_t wifi_tx_packets;     // WiFi transmitted packets
    uint32_t wifi_rx_packets;     // WiFi received packets
    uint32_t wifi_tx_dropped;     // WiFi TX dropped packets
    uint32_t wifi_rx_dropped;     // WiFi RX dropped packets
    int8_t sta_rssi;              // STA RSSI (dBm)
    uint8_t sta_connected;        // STA connection status (0=disconnected, 1=connected)
    uint8_t ap_clients;           // Number of AP clients
} sys_monitor_wifi_stats_t;

// ===== Benchmark Statistics =====

/**
 * Benchmark timing statistics
 */
typedef struct {
    uint32_t dns_query_count;     // Total DNS queries processed
    uint32_t dns_avg_latency_us;  // Average DNS query latency (microseconds)
    uint32_t dns_max_latency_us;  // Maximum DNS query latency (microseconds)
    uint32_t dns_cache_hits;      // Total DNS cache hits
    uint32_t dns_cache_misses;    // Total DNS cache misses
    uint32_t http_request_count;  // Total HTTP requests processed
    uint32_t http_avg_latency_us; // Average HTTP request latency (microseconds)
    uint32_t http_max_latency_us; // Maximum HTTP request latency (microseconds)
} sys_monitor_benchmark_stats_t;

// ===== Complete System Statistics =====

/**
 * Complete system monitoring data
 */
typedef struct {
    sys_monitor_cpu_stats_t cpu;
    sys_monitor_mem_stats_t mem;
    sys_monitor_net_stats_t net;
    sys_monitor_wifi_stats_t wifi;
    sys_monitor_benchmark_stats_t bench;
    uint32_t uptime_seconds;      // System uptime in seconds
} sys_monitor_stats_t;

// ===== API Functions =====

/**
 * Initialize the system monitoring subsystem
 * 
 * This function sets up the monitoring infrastructure and starts the
 * periodic logging task. Call this once during application startup.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sys_monitor_init(void);

/**
 * Update all system statistics
 * 
 * This function collects current statistics from the system. It is called
 * automatically by the monitoring task, but can be called manually if needed.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sys_monitor_update(void);

/**
 * Retrieve current system statistics
 * 
 * @param[out] stats Pointer to structure to receive statistics
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sys_monitor_get_stats(sys_monitor_stats_t *stats);

/**
 * Log current system statistics to console
 * 
 * Prints a formatted summary of all monitoring data to the ESP_LOG output.
 */
void sys_monitor_log_stats(void);

/**
 * Record a DNS query benchmark
 * 
 * @param latency_us Query latency in microseconds
 */
void sys_monitor_record_dns_query(uint32_t latency_us);

/**
 * Record an HTTP request benchmark
 * 
 * @param latency_us Request latency in microseconds
 */
void sys_monitor_record_http_request(uint32_t latency_us);

/**
 * Record a DNS cache hit
 */
void sys_monitor_record_dns_cache_hit(void);

/**
 * Record a DNS cache miss
 */
void sys_monitor_record_dns_cache_miss(void);

// ===== Benchmark Macros =====

/**
 * Benchmark timing macros for measuring operation latency
 * 
 * Usage:
 *   BENCHMARK_START(my_operation);
 *   // ... code to benchmark ...
 *   BENCHMARK_END(my_operation, "Operation description");
 * 
 * This will log the elapsed time and can be used to measure performance.
 */
#define BENCHMARK_START(name) \
    int64_t _benchmark_start_##name = esp_timer_get_time()

#define BENCHMARK_END(name, description) \
    do { \
        int64_t _benchmark_end = esp_timer_get_time(); \
        int64_t _benchmark_elapsed = _benchmark_end - _benchmark_start_##name; \
        ESP_LOGI("BENCHMARK", "%s: %lld us", description, _benchmark_elapsed); \
    } while(0)

/**
 * Get elapsed time in microseconds since benchmark start
 * 
 * Usage:
 *   BENCHMARK_START(my_operation);
 *   // ... code ...
 *   uint32_t elapsed = BENCHMARK_GET_ELAPSED(my_operation);
 */
#define BENCHMARK_GET_ELAPSED(name) \
    ((uint32_t)(esp_timer_get_time() - _benchmark_start_##name))

#ifdef __cplusplus
}
#endif
