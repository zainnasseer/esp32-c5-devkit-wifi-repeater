/**
 * @file wifi_config_manager.c
 * @brief Implementation of WiFi Configuration Manager
 */

#include "wifi_config_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi_config";

// NVS namespace and keys
#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_STA_SSID     "sta_ssid"
#define NVS_KEY_STA_PASS     "sta_pass"
#define NVS_KEY_AP_SSID      "ap_ssid"
#define NVS_KEY_AP_PASS      "ap_pass"
#define NVS_KEY_AP_MAX_CONN  "ap_max_conn"

// Default configuration
#define DEFAULT_STA_SSID       "Kite Roastery"
#define DEFAULT_STA_PASS       "Kite2025"
#define DEFAULT_AP_SSID        "Zains_ESP_Repeater"
#define DEFAULT_AP_PASS        "12345678"
#define DEFAULT_AP_MAX_CONN    4

esp_err_t wifi_config_init(void)
{
    // NVS is typically initialized in app_main, but we can ensure it here
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi configuration manager initialized");
    }
    
    return ret;
}

esp_err_t wifi_config_get_default(repeater_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(config, 0, sizeof(repeater_config_t));
    
    strncpy(config->sta_ssid, DEFAULT_STA_SSID, WIFI_CFG_MAX_SSID_LEN);
    strncpy(config->sta_password, DEFAULT_STA_PASS, WIFI_CFG_MAX_PASSWORD_LEN);
    strncpy(config->ap_ssid, DEFAULT_AP_SSID, WIFI_CFG_MAX_SSID_LEN);
    strncpy(config->ap_password, DEFAULT_AP_PASS, WIFI_CFG_MAX_PASSWORD_LEN);
    config->ap_max_connections = DEFAULT_AP_MAX_CONN;
    
    ESP_LOGI(TAG, "Loaded default configuration");
    return ESP_OK;
}

esp_err_t wifi_config_validate(const repeater_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate STA SSID
    size_t sta_ssid_len = strlen(config->sta_ssid);
    if (sta_ssid_len == 0 || sta_ssid_len > WIFI_CFG_MAX_SSID_LEN) {
        ESP_LOGE(TAG, "Invalid STA SSID length: %d", sta_ssid_len);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate STA password (can be empty for open networks, but max 64 chars)
    size_t sta_pass_len = strlen(config->sta_password);
    if (sta_pass_len > WIFI_CFG_MAX_PASSWORD_LEN) {
        ESP_LOGE(TAG, "Invalid STA password length: %d", sta_pass_len);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate AP SSID
    size_t ap_ssid_len = strlen(config->ap_ssid);
    if (ap_ssid_len == 0 || ap_ssid_len > WIFI_CFG_MAX_SSID_LEN) {
        ESP_LOGE(TAG, "Invalid AP SSID length: %d", ap_ssid_len);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate AP password (must be 8+ chars if not empty for WPA2)
    size_t ap_pass_len = strlen(config->ap_password);
    if (ap_pass_len > 0 && ap_pass_len < 8) {
        ESP_LOGE(TAG, "AP password must be at least 8 characters or empty");
        return ESP_ERR_INVALID_ARG;
    }
    if (ap_pass_len > WIFI_CFG_MAX_PASSWORD_LEN) {
        ESP_LOGE(TAG, "Invalid AP password length: %d", ap_pass_len);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate max connections
    if (config->ap_max_connections == 0 || config->ap_max_connections > 10) {
        ESP_LOGE(TAG, "Invalid AP max connections: %d (must be 1-10)", 
                 config->ap_max_connections);
        return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}

esp_err_t wifi_config_load(repeater_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // No config stored, use defaults
        ESP_LOGI(TAG, "No stored configuration found, using defaults");
        wifi_config_get_default(config);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        wifi_config_get_default(config);
        return err;
    }
    
    // Read configuration from NVS
    bool config_complete = true;
    size_t required_size;
    
    // Read STA SSID
    required_size = WIFI_CFG_MAX_SSID_LEN + 1;
    err = nvs_get_str(nvs_handle, NVS_KEY_STA_SSID, config->sta_ssid, &required_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA SSID not found in NVS");
        config_complete = false;
    }
    
    // Read STA password
    required_size = WIFI_CFG_MAX_PASSWORD_LEN + 1;
    err = nvs_get_str(nvs_handle, NVS_KEY_STA_PASS, config->sta_password, &required_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA password not found in NVS");
        config_complete = false;
    }
    
    // Read AP SSID
    required_size = WIFI_CFG_MAX_SSID_LEN + 1;
    err = nvs_get_str(nvs_handle, NVS_KEY_AP_SSID, config->ap_ssid, &required_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AP SSID not found in NVS");
        config_complete = false;
    }
    
    // Read AP password
    required_size = WIFI_CFG_MAX_PASSWORD_LEN + 1;
    err = nvs_get_str(nvs_handle, NVS_KEY_AP_PASS, config->ap_password, &required_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AP password not found in NVS");
        config_complete = false;
    }
    
    // Read AP max connections
    uint8_t max_conn;
    err = nvs_get_u8(nvs_handle, NVS_KEY_AP_MAX_CONN, &max_conn);
    if (err == ESP_OK) {
        config->ap_max_connections = max_conn;
    } else {
        ESP_LOGW(TAG, "AP max connections not found in NVS");
        config_complete = false;
    }
    
    nvs_close(nvs_handle);
    
    // If configuration is incomplete, use defaults
    if (!config_complete) {
        ESP_LOGW(TAG, "Incomplete configuration in NVS, using defaults");
        wifi_config_get_default(config);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    
    // Validate loaded configuration
    err = wifi_config_validate(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invalid configuration loaded from NVS, using defaults");
        wifi_config_get_default(config);
        return err;
    }
    
    ESP_LOGI(TAG, "WiFi configuration loaded from NVS");
    ESP_LOGI(TAG, "  STA SSID: %s", config->sta_ssid);
    ESP_LOGI(TAG, "  AP SSID: %s", config->ap_ssid);
    ESP_LOGI(TAG, "  AP Max Connections: %d", config->ap_max_connections);
    
    return ESP_OK;
}

esp_err_t wifi_config_save(const repeater_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate before saving
    esp_err_t err = wifi_config_validate(config);
    if (err != ESP_OK) {
        return err;
    }
    
    nvs_handle_t nvs_handle;
    
    // Open NVS in read-write mode
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // Save STA SSID
    err = nvs_set_str(nvs_handle, NVS_KEY_STA_SSID, config->sta_ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save STA SSID: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save STA password
    err = nvs_set_str(nvs_handle, NVS_KEY_STA_PASS, config->sta_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save STA password: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save AP SSID
    err = nvs_set_str(nvs_handle, NVS_KEY_AP_SSID, config->ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save AP SSID: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save AP password
    err = nvs_set_str(nvs_handle, NVS_KEY_AP_PASS, config->ap_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save AP password: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save AP max connections
    err = nvs_set_u8(nvs_handle, NVS_KEY_AP_MAX_CONN, config->ap_max_connections);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save AP max connections: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "WiFi configuration saved to NVS");
    ESP_LOGI(TAG, "  STA SSID: %s", config->sta_ssid);
    ESP_LOGI(TAG, "  AP SSID: %s", config->ap_ssid);
    
    return ESP_OK;
}

esp_err_t wifi_config_erase(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // Open NVS in read-write mode
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // Erase all keys in our namespace
    err = nvs_erase_all(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS erase: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "WiFi configuration erased from NVS");
    return ESP_OK;
}

/**
 * @brief Check if NVS config matches defaults, and update if different
 * 
 * This function compares the STA SSID in NVS with the default SSID.
 * If they differ, it erases the old config and saves the defaults.
 * This is useful for updating credentials without causing boot loops.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_config_sync_defaults(void)
{
    repeater_config_t nvs_cfg;
    repeater_config_t default_cfg;
    
    // Load current NVS config
    esp_err_t err = wifi_config_load(&nvs_cfg);
    
    // Get defaults
    wifi_config_get_default(&default_cfg);
    
    if (err == ESP_OK) {
        // Compare STA SSID to see if update is needed
        if (strcmp(nvs_cfg.sta_ssid, default_cfg.sta_ssid) != 0) {
            ESP_LOGW(TAG, "NVS SSID '%s' differs from default '%s'", 
                     nvs_cfg.sta_ssid, default_cfg.sta_ssid);
            ESP_LOGI(TAG, "Updating NVS with new defaults...");
            
            // Erase old config
            err = wifi_config_erase();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to erase old config: %s", esp_err_to_name(err));
                return err;
            }
            
            // Save new defaults
            err = wifi_config_save(&default_cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save new defaults: %s", esp_err_to_name(err));
                return err;
            }
            
            ESP_LOGI(TAG, "WiFi config updated successfully");
        } else {
            ESP_LOGI(TAG, "NVS config matches defaults, no update needed");
        }
    }
    
    return ESP_OK;
}

esp_err_t wifi_config_reset_to_defaults(void)
{
    ESP_LOGI(TAG, "Resetting WiFi config to hardcoded defaults...");

    // Step 1: Erase whatever is in NVS
    esp_err_t err = wifi_config_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS config: %s", esp_err_to_name(err));
        return err;
    }

    // Step 2: Write the compile-time defaults back to NVS
    repeater_config_t defaults;
    wifi_config_get_default(&defaults);

    err = wifi_config_save(&defaults);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write defaults to NVS: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WiFi config reset to defaults: STA='%s', AP='%s'",
             defaults.sta_ssid, defaults.ap_ssid);
    return ESP_OK;
}
