/**
 * @file wifi_config_manager.h
 * @brief WiFi Configuration Manager - Persistent storage for WiFi credentials
 * 
 * This module provides functions to save and load WiFi configuration from
 * NVS (Non-Volatile Storage), allowing credentials to persist across reboots.
 */

#ifndef WIFI_CONFIG_MANAGER_H
#define WIFI_CONFIG_MANAGER_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum lengths for WiFi credentials
#define WIFI_CFG_MAX_SSID_LEN       32
#define WIFI_CFG_MAX_PASSWORD_LEN   64

/**
 * @brief WiFi configuration structure
 * 
 * Contains credentials for both Station (STA) and Access Point (AP) modes
 */
typedef struct {
    char sta_ssid[WIFI_CFG_MAX_SSID_LEN + 1];       // Station SSID (uplink router)
    char sta_password[WIFI_CFG_MAX_PASSWORD_LEN + 1]; // Station password
    char ap_ssid[WIFI_CFG_MAX_SSID_LEN + 1];        // Access Point SSID
    char ap_password[WIFI_CFG_MAX_PASSWORD_LEN + 1];  // Access Point password
    uint8_t ap_max_connections;                      // Max AP connections
} repeater_config_t;

/**
 * @brief Initialize the WiFi configuration manager
 * 
 * Must be called before any other wifi_config_* functions.
 * This initializes NVS if not already initialized.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_config_init(void);

/**
 * @brief Load WiFi configuration from NVS
 * 
 * If no configuration is stored in NVS, returns default configuration.
 * 
 * @param[out] config Pointer to configuration structure to fill
 * @return ESP_OK on success
 *         ESP_ERR_NVS_NOT_FOUND if no config stored (defaults will be loaded)
 *         Other error codes on failure
 */
esp_err_t wifi_config_load(repeater_config_t *config);

/**
 * @brief Save WiFi configuration to NVS
 * 
 * @param[in] config Pointer to configuration to save
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_config_save(const repeater_config_t *config);

/**
 * @brief Get default WiFi configuration
 * 
 * @param[out] config Pointer to configuration structure to fill with defaults
 * @return ESP_OK on success
 */
esp_err_t wifi_config_get_default(repeater_config_t *config);

/**
 * @brief Erase WiFi configuration from NVS
 * 
 * This will cause the next wifi_config_load() to return defaults.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_config_erase(void);

/**
 * @brief Validate WiFi configuration
 * 
 * Checks if SSID and password lengths are valid.
 * 
 * @param[in] config Configuration to validate
 * @return ESP_OK if valid, ESP_ERR_INVALID_ARG otherwise
 */
esp_err_t wifi_config_validate(const repeater_config_t *config);

/**
 * @brief Sync defaults to NVS if they differ from stored values
 * 
 * Compares NVS SSID with default SSID, and updates NVS if different.
 * This is useful for deploying credential updates without manual intervention.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_config_sync_defaults(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_CONFIG_MANAGER_H
