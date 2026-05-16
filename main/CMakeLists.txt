set(EXTRA_COMPONENT_DIRS components/esp-nat)

idf_component_register(SRCS "main.c" "web_server.c" "dns_proxy.c" "dns_log.c" "sys_monitor.c" "ap_clients.c" "wifi_config_manager.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif lwip nvs_flash esp_http_server cjson esp_timer esp_driver_gpio spiffs)

# Embed the www directory as a SPIFFS partition
spiffs_create_partition_image(www www FLASH_IN_PROJECT)
