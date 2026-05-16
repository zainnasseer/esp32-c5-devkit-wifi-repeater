#include "ap_clients.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h" // For IP_EVENT macros
#include "esp_mac.h"   // For MACSTR

static const char *TAG = "ap_clients";

// Local cache of client IPs
typedef struct {
    uint8_t mac[6];
    esp_ip4_addr_t ip;
    int64_t last_seen; // Timestamp to help with cleanup if needed (optional)
} client_ip_map_t;

static client_ip_map_t s_client_ips[ESP_WIFI_MAX_CONN_NUM];

static void handle_ap_staipassigned(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    ip_event_assigned_ip_to_client_t *event = (ip_event_assigned_ip_to_client_t *)event_data;
    
    ESP_LOGI(TAG, "Client IP assigned: " MACSTR " -> " IPSTR,
             MAC2STR(event->mac), IP2STR(&event->ip));

    // Store in cache
    // Simple strategy: check if MAC exists, update it. If not, find empty slot.
    int idx = -1;
    
    // 1. Try to find existing entry
    for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM; i++) {
        if (memcmp(s_client_ips[i].mac, event->mac, 6) == 0) {
            idx = i;
            break;
        }
    }
    
    // 2. If not found, find empty slot (IP = 0)
    if (idx == -1) {
        for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM; i++) {
            if (s_client_ips[i].ip.addr == 0) {
                idx = i;
                break;
            }
        }
    }
    
    // 3. Update if we have a slot
    if (idx != -1) {
        memcpy(s_client_ips[idx].mac, event->mac, 6);
        s_client_ips[idx].ip = event->ip;
    } else {
        ESP_LOGW(TAG, "Client IP cache full, could not store " MACSTR, MAC2STR(event->mac));
    }
}

// Handler for when a station disconnects -> clear their IP
static void handle_ap_stadisconnected(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data)
{
    wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
    
    for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM; i++) {
        if (memcmp(s_client_ips[i].mac, event->mac, 6) == 0) {
            memset(&s_client_ips[i], 0, sizeof(client_ip_map_t));
            break;
        }
    }
}

esp_err_t ap_clients_init(void)
{
    // Clear cache
    memset(s_client_ips, 0, sizeof(s_client_ips));
    
    // Register for IP assignment events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                                        &handle_ap_staipassigned, NULL, NULL));
                                                        
    // Register for disconnection events to clean up
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                                        &handle_ap_stadisconnected, NULL, NULL));
                                                        
    return ESP_OK;
}

esp_err_t ap_clients_get(ap_client_list_t *list)
{
    if (list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(list, 0, sizeof(*list));

    wifi_sta_list_t sta_list = {0};
    esp_err_t err = esp_wifi_ap_get_sta_list(&sta_list);
    if (err != ESP_OK) {
        return err;
    }

    size_t count = (size_t)sta_list.num;
    if (count > ESP_WIFI_MAX_CONN_NUM) {
        count = ESP_WIFI_MAX_CONN_NUM;
    }
    list->count = count;

    for (size_t i = 0; i < count; i++) {
        // Copy MAC
        memcpy(list->clients[i].mac, sta_list.sta[i].mac, sizeof(list->clients[i].mac));
        
        // Lookup IP in our cache
        list->clients[i].has_ip = false;
        for (int j = 0; j < ESP_WIFI_MAX_CONN_NUM; j++) {
            if (s_client_ips[j].ip.addr != 0 &&
                memcmp(s_client_ips[j].mac, list->clients[i].mac, 6) == 0) {
                list->clients[i].ip = s_client_ips[j].ip;
                list->clients[i].has_ip = true;
                break;
            }
        }
    }

    return ESP_OK;
}
