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
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum lengths for WiFi credentials
#define WIFI_CFG_MAX_SSID_LEN       32
#define WIFI_CFG_MAX_PASSWORD_LEN   64

// Maximum number of previously-used STA SSIDs to remember
#define WIFI_CFG_MAX_SSID_HISTORY   5

// Maximum lengths for dashboard auth credentials
#define WIFI_CFG_MAX_USERNAME_LEN   32
#define WIFI_CFG_MAX_DASH_PASS_LEN  64

/**
 * @brief WiFi configuration structure
 * 
 * Contains credentials for both Station (STA) and Access Point (AP) modes
 */
typedef struct {
    char sta_ssid[WIFI_CFG_MAX_SSID_LEN + 1];          // Station SSID (uplink router)
    char sta_password[WIFI_CFG_MAX_PASSWORD_LEN + 1];  // Station password
    char ap_ssid[WIFI_CFG_MAX_SSID_LEN + 1];           // Access Point SSID
    char ap_password[WIFI_CFG_MAX_PASSWORD_LEN + 1];   // Access Point password
    uint8_t ap_max_connections;                        // Max AP connections
} repeater_config_t;

/**
 * @brief Dashboard authentication credentials
 */
typedef struct {
    char username[WIFI_CFG_MAX_USERNAME_LEN + 1]; // Dashboard username
    char password[WIFI_CFG_MAX_DASH_PASS_LEN + 1]; // Dashboard password
} dashboard_auth_t;

/**
 * @brief Previously-used STA SSID history
 */
typedef struct {
    char ssids[WIFI_CFG_MAX_SSID_HISTORY][WIFI_CFG_MAX_SSID_LEN + 1];
    uint8_t count; // Number of valid entries
} ssid_history_t;

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

/**
 * @brief Reset NVS config to hardcoded defaults
 *
 * Erases any NVS-stored credentials and writes the compile-time defaults back
 * so the next boot (or live apply) uses the credentials baked into the firmware.
 * This resolves conflicts between web-dashboard-saved values and the source code.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_config_reset_to_defaults(void);

/**
 * @brief Load dashboard authentication credentials from NVS
 *
 * Falls back to compile-time defaults if not stored.
 *
 * @param[out] auth Pointer to auth struct to fill
 * @return ESP_OK on success
 */
esp_err_t wifi_config_load_auth(dashboard_auth_t *auth);

/**
 * @brief Save dashboard authentication credentials to NVS
 *
 * @param[in] auth Pointer to auth struct to save
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_config_save_auth(const dashboard_auth_t *auth);

/**
 * @brief Verify dashboard credentials against stored values
 *
 * @param[in] username Username to verify
 * @param[in] password Password to verify
 * @return true if credentials match, false otherwise
 */
bool wifi_config_verify_auth(const char *username, const char *password);

/**
 * @brief Load SSID history from NVS
 *
 * @param[out] history Pointer to history struct to fill
 * @return ESP_OK on success
 */
esp_err_t wifi_config_load_ssid_history(ssid_history_t *history);

/**
 * @brief Add an SSID to the history (saves to NVS, deduplicates, newest first)
 *
 * @param[in] ssid SSID string to add
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_config_add_ssid_to_history(const char *ssid);

#ifdef __cplusplus
}
#endif

#endif // WIFI_CONFIG_MANAGER_H
