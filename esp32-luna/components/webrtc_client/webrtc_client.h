#ifndef WEBRTC_CLIENT_H
#define WEBRTC_CLIENT_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// WebRTC client configuration
typedef struct {
    char server_url[256];      // e.g., "ws://192.168.1.100:7860"
    char server_ip[64];        // IP address of Pipecat server
    uint16_t server_port;      // Port (default: 7860)
} webrtc_client_config_t;

// Initialize WebRTC client
esp_err_t webrtc_client_init(const webrtc_client_config_t *config);

// Start WebRTC connection
esp_err_t webrtc_client_start(void);

// Stop WebRTC connection
esp_err_t webrtc_client_stop(void);

// Send audio data to server
esp_err_t webrtc_client_send_audio(const int16_t *audio_data, size_t samples);

// Receive audio data from server (callback)
typedef void (*webrtc_audio_callback_t)(const int16_t *audio_data, size_t samples);
void webrtc_client_set_audio_callback(webrtc_audio_callback_t callback);

// Receive display update (callback)
typedef void (*webrtc_display_callback_t)(const char *data, size_t len);
void webrtc_client_set_display_callback(webrtc_display_callback_t callback);

// Check connection status
bool webrtc_client_is_connected(void);

#endif // WEBRTC_CLIENT_H

