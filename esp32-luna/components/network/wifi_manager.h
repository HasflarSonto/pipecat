/*
 * WiFi Manager for ESP32-Luna
 * Handles WiFi connection with NVS-stored credentials and auto-reconnect
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi configuration structure
 */
typedef struct {
    char ssid[32];          // WiFi SSID
    char password[64];      // WiFi password
    bool store_in_nvs;      // Store credentials in NVS
} wifi_manager_config_t;

/**
 * @brief WiFi event callback type
 */
typedef enum {
    WIFI_EVENT_CONNECTED,
    WIFI_EVENT_DISCONNECTED,
    WIFI_EVENT_GOT_IP,
    WIFI_EVENT_LOST_IP,
} wifi_manager_event_t;

typedef void (*wifi_manager_event_cb_t)(wifi_manager_event_t event, void *ctx);

/**
 * @brief Initialize WiFi manager
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Deinitialize WiFi manager
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_deinit(void);

/**
 * @brief Connect to WiFi with provided credentials
 * @param config WiFi configuration
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_connect(const wifi_manager_config_t *config);

/**
 * @brief Connect using NVS-stored credentials
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no stored credentials
 */
esp_err_t wifi_manager_connect_stored(void);

/**
 * @brief Disconnect from WiFi
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Check if WiFi is connected
 * @return true if connected
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get current IP address
 * @param ip_str Buffer to store IP string (at least 16 bytes)
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_get_ip(char *ip_str);

/**
 * @brief Register event callback
 * @param callback Callback function
 * @param ctx User context
 */
void wifi_manager_set_event_callback(wifi_manager_event_cb_t callback, void *ctx);

/**
 * @brief Clear stored credentials from NVS
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_clear_stored(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
