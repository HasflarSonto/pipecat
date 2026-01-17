/**
 * WebSocket Client for Luna Simulator
 * Connects to pipecat backend
 */

#ifndef WS_CLIENT_SIM_H
#define WS_CLIENT_SIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * WebSocket event types
 */
typedef enum {
    WS_EVENT_CONNECTED,
    WS_EVENT_DISCONNECTED,
    WS_EVENT_TEXT,      /* JSON command received */
    WS_EVENT_BINARY,    /* Audio data received */
    WS_EVENT_ERROR,
} ws_event_t;

/**
 * WebSocket event callback
 * @param event Event type
 * @param data Data pointer (text or binary)
 * @param len Data length
 */
typedef void (*ws_event_callback_t)(ws_event_t event, const void* data, size_t len);

/**
 * Initialize WebSocket client
 * @param callback Event callback function
 * @return true on success
 */
bool ws_client_init(ws_event_callback_t callback);

/**
 * Deinitialize WebSocket client
 */
void ws_client_deinit(void);

/**
 * Connect to server
 * @param host Server hostname
 * @param port Server port
 * @param path WebSocket path (e.g., "/luna-esp32")
 * @return true if connection started
 */
bool ws_client_connect(const char* host, int port, const char* path);

/**
 * Disconnect from server
 */
void ws_client_disconnect(void);

/**
 * Check if connected
 */
bool ws_client_is_connected(void);

/**
 * Service WebSocket - must be called regularly
 * @param timeout_ms Maximum time to wait for events
 */
void ws_client_service(int timeout_ms);

/**
 * Send text message (JSON command)
 * @param text JSON string
 * @return true on success
 */
bool ws_client_send_text(const char* text);

/**
 * Send binary data (audio)
 * @param data Binary data
 * @param len Data length
 * @return true on success
 */
bool ws_client_send_binary(const void* data, size_t len);

#endif /* WS_CLIENT_SIM_H */
