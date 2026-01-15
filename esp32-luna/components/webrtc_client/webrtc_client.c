#include "webrtc_client.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string.h>

static const char *TAG = "webrtc_client";

static webrtc_client_config_t s_config;
static bool s_connected = false;
static webrtc_audio_callback_t s_audio_callback = NULL;
static webrtc_display_callback_t s_display_callback = NULL;

esp_err_t webrtc_client_init(const webrtc_client_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(webrtc_client_config_t));
    s_connected = false;

    ESP_LOGI(TAG, "WebRTC client initialized");
    ESP_LOGI(TAG, "Server: %s:%d", s_config.server_ip, s_config.server_port);

    // TODO: Initialize WebSocket client
    // TODO: Initialize WebRTC peer connection
    // TODO: Set up audio codec

    return ESP_OK;
}

esp_err_t webrtc_client_start(void)
{
    ESP_LOGI(TAG, "Starting WebRTC connection...");

    // TODO: Connect to WiFi (if not already connected)
    // TODO: Establish WebSocket connection
    // TODO: Exchange SDP offer/answer
    // TODO: Exchange ICE candidates
    // TODO: Start audio streams

    s_connected = true;
    ESP_LOGI(TAG, "WebRTC connected");

    return ESP_OK;
}

esp_err_t webrtc_client_stop(void)
{
    ESP_LOGI(TAG, "Stopping WebRTC connection...");

    // TODO: Stop audio streams
    // TODO: Close peer connection
    // TODO: Close WebSocket connection

    s_connected = false;
    ESP_LOGI(TAG, "WebRTC disconnected");

    return ESP_OK;
}

esp_err_t webrtc_client_send_audio(const int16_t *audio_data, size_t samples)
{
    if (!s_connected || audio_data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // TODO: Send audio data via WebRTC
    // Format: PCM 16-bit mono, 16kHz

    return ESP_OK;
}

void webrtc_client_set_audio_callback(webrtc_audio_callback_t callback)
{
    s_audio_callback = callback;
}

void webrtc_client_set_display_callback(webrtc_display_callback_t callback)
{
    s_display_callback = callback;
}

bool webrtc_client_is_connected(void)
{
    return s_connected;
}

