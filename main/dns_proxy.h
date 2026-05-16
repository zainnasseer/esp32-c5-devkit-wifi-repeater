/**
 * @file dns_proxy.h
 * @brief DNS Proxy Public API
 * 
 * This header file defines the public interface for the DNS proxy module.
 * It declares the functions that other parts of the WiFi repeater can use
 * to control the DNS proxy functionality.
 * 
 * Usage example:
 * @code
 *   // Set the upstream DNS server (e.g., Google DNS 8.8.8.8)
 *   esp_ip4_addr_t dns_server = { .addr = 0x08080808 };
 *   dns_proxy_set_upstream(&dns_server);
 * 
 *   // Start the proxy
 *   dns_proxy_start();
 * 
 *   // Later, stop the proxy
 *   dns_proxy_stop();
 * @endcode
 */

#ifndef DNS_PROXY_H
#define DNS_PROXY_H

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

/**
 * @brief Start the DNS proxy server
 * 
 * Starts a FreeRTOS task that listens on UDP port 53 for DNS queries from
 * WiFi clients. Queries are logged with client information and forwarded
 * to the configured upstream DNS server.
 * 
 * @return ESP_OK if started successfully
 * @return ESP_ERR_NO_MEM if task creation failed
 * 
 * @note This function is idempotent - calling it when already running returns ESP_OK
 * @note The upstream DNS server must be set with dns_proxy_set_upstream() before
 *       queries can be forwarded
 */
esp_err_t dns_proxy_start(void);

/**
 * @brief Stop the DNS proxy server
 * 
 * Signals the DNS proxy task to stop processing queries and terminate.
 * The function returns immediately; the task will clean up and exit shortly after.
 * 
 * @note Safe to call even if the proxy is not running
 */
void dns_proxy_stop(void);

/**
 * @brief Set the upstream DNS server
 * 
 * Configures the IP address of the DNS server that queries should be forwarded to.
 * This is typically the DNS server obtained from the STA (station) interface when
 * connected to an upstream WiFi network.
 * 
 * @param dns_ip Pointer to IPv4 address of upstream DNS server, or NULL to clear
 * 
 * @note If no upstream DNS is configured, the proxy will drop incoming queries
 *       and log a warning
 */
void dns_proxy_set_upstream(const esp_ip4_addr_t *dns_ip);

#endif // DNS_PROXY_H
