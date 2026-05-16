#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

/**
 * Initialize and start the HTTP web server for the repeater dashboard.
 * The server will be accessible at the AP interface IP (typically 192.168.4.1)
 */
esp_err_t web_server_init(void);

/**
 * Stop the web server
 */
esp_err_t web_server_stop(void);

#endif // WEB_SERVER_H
