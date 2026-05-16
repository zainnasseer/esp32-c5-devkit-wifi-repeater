#ifndef AP_CLIENTS_H
#define AP_CLIENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif_types.h"
#include "esp_wifi_ap_get_sta_list.h"

typedef struct {
    uint8_t mac[6];
    esp_ip4_addr_t ip;
    bool has_ip;
} ap_client_info_t;

typedef struct {
    size_t count;
    ap_client_info_t clients[ESP_WIFI_MAX_CONN_NUM];
} ap_client_list_t;

esp_err_t ap_clients_init(void);
esp_err_t ap_clients_get(ap_client_list_t *list);

#endif // AP_CLIENTS_H
