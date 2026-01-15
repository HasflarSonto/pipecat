/*
 * WebSocket Client for ESP32-Luna
 * Handles WebSocket connection to Pipecat server with JSON commands and binary audio
 */

#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WebSocket client configuration
 */
typedef struct {
    char server_ip[64];        // Server IP address
    uint16_t server_port;      // Server port (default: 7860)
    char endpoint[32];         // WebSocket endpoint (default: "/luna-esp32")
    uint32_t reconnect_ms;     // Reconnect interval in ms (0 = no auto-reconnect)
} ws_client_config_t;

/**
 * @brief WebSocket client events
 */
typedef enum {
    WS_EVENT_CONNECTED,
    WS_EVENT_DISCONNECTED,
    WS_EVENT_ERROR,
    WS_EVENT_TEXT_DATA,        // JSON command received
    WS_EVENT_BINARY_DATA,      // Audio data received
} ws_client_event_t;

/**
 * @brief WebSocket event data
 */
typedef struct {
    ws_client_event_t event;
    const uint8_t *data;
    size_t data_len;
} ws_client_event_data_t;

/**
 * @brief WebSocket event callback
 */
typedef void (*ws_client_event_cb_t)(ws_client_event_data_t *event, void *ctx);

/**
 * @brief Initialize WebSocket client
 * @param config Client configuration
 * @return ESP_OK on success
 */
esp_err_t ws_client_init(const ws_client_config_t *config);

/**
 * @brief Deinitialize WebSocket client
 * @return ESP_OK on success
 */
esp_err_t ws_client_deinit(void);

/**
 * @brief Connect to server
 * @return ESP_OK on success
 */
esp_err_t ws_client_connect(void);

/**
 * @brief Disconnect from server
 * @return ESP_OK on success
 */
esp_err_t ws_client_disconnect(void);

/**
 * @brief Check if connected
 * @return true if connected
 */
bool ws_client_is_connected(void);

/**
 * @brief Send text (JSON) message
 * @param text JSON string
 * @return ESP_OK on success
 */
esp_err_t ws_client_send_text(const char *text);

/**
 * @brief Send binary audio data
 * @param data PCM audio data
 * @param len Data length in bytes
 * @return ESP_OK on success
 */
esp_err_t ws_client_send_binary(const uint8_t *data, size_t len);

/**
 * @brief Set event callback
 * @param callback Event callback function
 * @param ctx User context
 */
void ws_client_set_event_callback(ws_client_event_cb_t callback, void *ctx);

#ifdef __cplusplus
}
#endif

#endif // WS_CLIENT_H
