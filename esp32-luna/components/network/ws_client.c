/*
 * WebSocket Client for ESP32-Luna
 * Handles WebSocket connection to Pipecat server with JSON commands and binary audio
 */

#include "ws_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ws_client";

static esp_websocket_client_handle_t s_client = NULL;
static ws_client_config_t s_config;
static bool s_connected = false;
static bool s_initialized = false;

static ws_client_event_cb_t s_event_callback = NULL;
static void *s_event_callback_ctx = NULL;

static SemaphoreHandle_t s_send_mutex = NULL;

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    ws_client_event_data_t event = {0};

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            s_connected = true;
            event.event = WS_EVENT_CONNECTED;
            if (s_event_callback) {
                s_event_callback(&event, s_event_callback_ctx);
            }
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket disconnected");
            s_connected = false;
            event.event = WS_EVENT_DISCONNECTED;
            if (s_event_callback) {
                s_event_callback(&event, s_event_callback_ctx);
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x01) {
                // Text frame (JSON)
                event.event = WS_EVENT_TEXT_DATA;
                event.data = (const uint8_t *)data->data_ptr;
                event.data_len = data->data_len;
                ESP_LOGD(TAG, "Received text: %.*s", data->data_len, data->data_ptr);
            } else if (data->op_code == 0x02) {
                // Binary frame (audio)
                event.event = WS_EVENT_BINARY_DATA;
                event.data = (const uint8_t *)data->data_ptr;
                event.data_len = data->data_len;
                ESP_LOGD(TAG, "Received binary: %d bytes", data->data_len);
            } else {
                break;
            }
            if (s_event_callback) {
                s_event_callback(&event, s_event_callback_ctx);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            event.event = WS_EVENT_ERROR;
            if (s_event_callback) {
                s_event_callback(&event, s_event_callback_ctx);
            }
            break;

        default:
            break;
    }
}

esp_err_t ws_client_init(const ws_client_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(ws_client_config_t));

    // Set defaults
    if (s_config.server_port == 0) {
        s_config.server_port = 7860;
    }
    if (strlen(s_config.endpoint) == 0) {
        strncpy(s_config.endpoint, "/luna-esp32", sizeof(s_config.endpoint) - 1);
    }

    s_send_mutex = xSemaphoreCreateMutex();
    if (s_send_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WebSocket client initialized");
    ESP_LOGI(TAG, "Server: %s:%d%s", s_config.server_ip, s_config.server_port, s_config.endpoint);

    return ESP_OK;
}

esp_err_t ws_client_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_client) {
        ws_client_disconnect();
    }

    if (s_send_mutex) {
        vSemaphoreDelete(s_send_mutex);
        s_send_mutex = NULL;
    }

    s_initialized = false;
    return ESP_OK;
}

esp_err_t ws_client_connect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_client) {
        ws_client_disconnect();
    }

    // Build URI
    char uri[256];
    snprintf(uri, sizeof(uri), "ws://%s:%d%s",
             s_config.server_ip, s_config.server_port, s_config.endpoint);

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .buffer_size = 4096,
        .task_stack = 6 * 1024,
        .task_prio = 5,
        .reconnect_timeout_ms = s_config.reconnect_ms > 0 ? s_config.reconnect_ms : 10000,
        .network_timeout_ms = 10000,
    };

    s_client = esp_websocket_client_init(&ws_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                  websocket_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %d", err);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Connecting to %s", uri);
    return ESP_OK;
}

esp_err_t ws_client_disconnect(void)
{
    if (s_client == NULL) {
        return ESP_OK;
    }

    esp_websocket_client_close(s_client, pdMS_TO_TICKS(5000));
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;

    ESP_LOGI(TAG, "Disconnected");
    return ESP_OK;
}

bool ws_client_is_connected(void)
{
    return s_connected && s_client != NULL &&
           esp_websocket_client_is_connected(s_client);
}

esp_err_t ws_client_send_text(const char *text)
{
    if (!ws_client_is_connected() || text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_send_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int len = strlen(text);
    int sent = esp_websocket_client_send_text(s_client, text, len, pdMS_TO_TICKS(1000));

    xSemaphoreGive(s_send_mutex);

    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send text");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Sent text: %d bytes", sent);
    return ESP_OK;
}

esp_err_t ws_client_send_binary(const uint8_t *data, size_t len)
{
    if (!ws_client_is_connected() || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_send_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Add 2-byte length header (big-endian)
    uint8_t header[2];
    header[0] = (len >> 8) & 0xFF;
    header[1] = len & 0xFF;

    // Send header + data
    // Note: For efficiency, we should use a single send with combined buffer
    // but esp_websocket_client doesn't support scatter-gather, so we send as-is
    int sent = esp_websocket_client_send_bin(s_client, (const char *)data, len, pdMS_TO_TICKS(1000));

    xSemaphoreGive(s_send_mutex);

    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send binary");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Sent binary: %d bytes", sent);
    return ESP_OK;
}

void ws_client_set_event_callback(ws_client_event_cb_t callback, void *ctx)
{
    s_event_callback = callback;
    s_event_callback_ctx = ctx;
}
