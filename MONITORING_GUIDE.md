# WiFi Repeater Monitoring Guide

## Overview

The WiFi repeater includes a comprehensive system monitoring and benchmarking subsystem that tracks CPU usage, memory consumption, network throughput, WiFi statistics, and performance metrics. This guide explains how to use and interpret these metrics.

## Monitored Metrics

### CPU Statistics

**What it tracks:**
- Overall CPU usage percentage (0-100%)
- Per-task CPU usage breakdown
- Number of active FreeRTOS tasks
- Stack high watermark (remaining stack space) for each task

**How to interpret:**
- **CPU < 50%**: System is operating normally with good headroom
- **CPU 50-80%**: Moderate load, system is handling traffic efficiently
- **CPU > 80%**: High load, may experience performance degradation
- **Stack HWM < 512 bytes**: Task may be at risk of stack overflow

**Access via API:**
```bash
curl http://192.168.4.1/api/stats
```
Look for the `cpu` object in the JSON response.

### Memory Statistics

**What it tracks:**
- Free heap memory (bytes available)
- Total heap size (estimated)
- Minimum free heap ever reached (lowest watermark)
- Largest contiguous free block
- Heap usage percentage

**How to interpret:**
- **Free heap > 100KB**: Healthy memory situation
- **Free heap 50-100KB**: Moderate usage, monitor for leaks
- **Free heap < 50KB**: Critical, may cause allocation failures
- **Min free heap**: Important for understanding peak usage

**Common thresholds:**
- ESP32-C5 typically has ~300KB total heap
- Aim to keep at least 30% (90KB+) free during normal operation

### Network Statistics

**What it tracks:**
- Total bytes transmitted (TX) and received (RX)
- Total packets transmitted and received
- Current throughput (bytes per second)
- Transmission and reception errors

**How to interpret:**
- **Throughput**: Shows real-time network activity
- **TX/RX errors**: Should remain low (< 1% of packets)
- **Packet counts**: Cumulative since boot

**Performance expectations:**
- WiFi 802.11n can typically achieve 20-50 Mbps throughput
- Higher error rates indicate signal issues or congestion

### WiFi-Specific Statistics

**What it tracks:**
- WiFi TX/RX packet counts
- Dropped packets (TX and RX)
-STA RSSI (signal strength)
- STA connection status
- Number of AP clients connected

**How to interpret:**
- **RSSI (dBm)**:
  - \> -50 dBm: Excellent signal
  - -50 to -60 dBm: Very good
  - -60 to -70 dBm: Good
  - -70 to -80 dBm: Fair (may experience slowdowns)
  - < -80 dBm: Weak (unreliable connection)
- **Dropped packets**: Should be minimal (< 0.1%)

### Benchmark Statistics

**What it tracks:**
- DNS query count, average latency, and maximum latency
- HTTP request count, average latency, and maximum latency

**How to interpret:**
- **DNS latency**:
  - < 50ms: Excellent
  - 50-100ms: Good
  - \> 100ms: Slow(logged as warning)
- **HTTP latency**:
  - < 20ms: Excellent
  - 20-50ms: Good
  - \> 50ms: Slow (logged as warning)

## Accessing Monitoring Data

### Via Web Dashboard

1. Connect to the repeater WiFi network
2. Open a browser and navigate to `http://192.168.4.1`
3. The dashboard auto-refreshes every 2 seconds with latest metrics
4. Look for the "Features & Power" card for basic system info
5. (Note: Enhanced monitoring cards will be added in future dashboard update)

### Via API Endpoints

**Network Status** (`GET /api/network`):
```bash
curl http://192.168.4.1/api/network
```
Returns: STA/AP status, IP info, client list, DNS logs, uptime

**System Statistics** (`GET /api/stats`):
```bash
curl http://192.168.4.1/api/stats | json_pp
```
Returns: Complete monitoring data (CPU, memory, network, WiFi, benchmarks)

### Via Serial Terminal

1. Connect via USB and open serial monitor:
   ```bash
   cd /Users/zainnasseer/development/esp/wifi_repeater
   idf.py monitor
   ```

2. Monitoring logs appear every 5 seconds with format:
   ```
   I (12345) sys_monitor: ========== System Monitor Report ==========
   I (12345) sys_monitor: CPU: Total usage: 45%
   I (12345) sys_monitor: CPU: Active tasks: 8
   I (12345) sys_monitor:   Task 'IDLE': CPU=15%, Stack HWM=768 bytes
   ...
   I (12345) sys_monitor: MEM: Free heap: 145280 bytes (45.2% used)
   I (12345) sys_monitor: NET: TX: 1234 pkt, 567890 bytes, 1200 B/s | RX: ...
   ...
   ```

## Understanding Code Functionality

### Monitoring Architecture

The monitoring system consists of three main components:

1. **sys_monitor.c/h**: Core monitoring module
   - Collects statistics from FreeRTOS, lwIP, and ESP-IDF APIs
   - Runs a background task that logs every 5 seconds
   - Provides thread-safe API for retrieving current stats

2. **Benchmark macros**: Performance measurement
   - `BENCHMARK_START(name)`: Start timing an operation
   - `BENCHMARK_END(name, description)`: Stop and log elapsed time
   - `BENCHMARK_GET_ELAPSED(name)`: Get elapsed microseconds

3. **Web server integration**: Dashboard and API
   - `/api/stats` endpoint serves JSON with all metrics
   - Designed for frontend visualization (charts, graphs)

### Key Functions

**sys_monitor_init()**
- Call once during app startup (after NVS, before WiFi)
- Creates monitoring task,initializes baseline values
- Returns `ESP_OK` on success

**sys_monitor_update()**
- Updates all statistics from system APIs
- Called automatically every 5 seconds by monitoring task
- Can be called manually for on-demand updates

**sys_monitor_get_stats(sys_monitor_stats_t *stats)**
- Thread-safe retrieval of current statistics
- Used by web server to serve `/api/stats` API

**sys_monitor_record_dns_query(uint32_t latency_us)**
- Records DNS query latency for benchmarking
- Automatically logs warnings for queries > 100ms

**sys_monitor_record_http_request(uint32_t latency_us)**
- Records HTTP request latency
- Automatically logs warnings for requests > 50ms

### Performance Overhead

The monitoring subsystem has minimal impact on system performance:

- **CPU usage**: ~1-2% (mostly during 5-second logging intervals)
- **Memory**: ~8KB RAM for task stack and data structures
- **Benchmark overhead**: < 1 microsecond per measurement

## Troubleshooting

### High CPU Usage

**Symptoms**: CPU consistently > 80%

**Possible causes:**
- Many connected clients generating traffic
- DNS proxy handling high query rate
- Web dashboard being rapidly refreshed

**Solutions:**
- Limit number of AP clients (`AP_MAX_CONN` in main.c)
- Increase logging interval in `sys_monitor.c` (change 5000ms to 10000ms)

### Low Free Memory

**Symptoms**: Free heap < 50KB, allocation failures

**Possible causes:**
- Memory leak in application code
- Too many simultaneous connections
- Large JSON responses

**Solutions:**
- Check `min_free_heap` to understand peak usage
- Review task stack sizes (may be over-allocated)
- Monitor over time to identify memory leaks

### High Network Errors

**Symptoms**: TX/RX errors > 1% of packets

**Possible causes:**
- Poor WiFi signal (check RSSI)
- RF interference
- Too many retransmissions

**Solutions:**
- Reposition repeater for better signal
- Change WiFi channel to avoid interference
- Reduce transmit power if signal is too strong

## Customization

### Adjusting Log Interval

Edit `sys_monitor.c`, line 372:
```c
vTaskDelay(pdMS_TO_TICKS(5000));  // Change 5000 to desired milliseconds
```

### Changing Warning Thresholds

**DNS latency warning** (edit `sys_monitor.c`, line 295):
```c
if (latency_us > 100000) {  // Change 100000 to desired microseconds
```

**HTTP latency warning** (edit `sys_monitor.c`, line 309):
```c
if (latency_us > 50000) {  // Change 50000 to desired microseconds
```

### Adding Custom Metrics

1. Add new fields to `sys_monitor_stats_t` in `sys_monitor.h`
2. Implement collection logic in `sys_monitor_update()` in `sys_monitor.c`
3. Add JSON serialization in `api_stats_handler()` in `web_server.c`
4. Update logging in `sys_monitor_log_stats()`

## Related Files

- `main/sys_monitor.h` - Monitoring API and data structures
- `main/sys_monitor.c` - Monitoring implementation
- `main/web_server.c` - `/api/stats` endpoint handler
- `main/dns_proxy.c` - DNS query benchmarking
- `main/main.c` - Monitoring initialization
