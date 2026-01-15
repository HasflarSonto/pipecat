/*
 * Audio Capture for ESP32-Luna
 * Microphone input with ring buffer for WebSocket streaming
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audio capture callback (called when audio chunk is ready)
 * @param data PCM audio data (mono, 16-bit, 16kHz)
 * @param len Data length in bytes
 * @param ctx User context
 */
typedef void (*audio_capture_cb_t)(const int16_t *data, size_t len, void *ctx);

/**
 * @brief Audio capture configuration
 */
typedef struct {
    size_t buffer_size;        // Ring buffer size in bytes (0 = default)
    audio_capture_cb_t callback;  // Optional callback for audio chunks
    void *callback_ctx;        // Callback context
    bool stereo_to_mono;       // Convert stereo to mono (average channels)
} audio_capture_config_t;

/**
 * @brief Initialize audio capture
 * @param config Capture configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t audio_capture_init(const audio_capture_config_t *config);

/**
 * @brief Deinitialize audio capture
 * @return ESP_OK on success
 */
esp_err_t audio_capture_deinit(void);

/**
 * @brief Start audio capture task
 * @return ESP_OK on success
 */
esp_err_t audio_capture_start(void);

/**
 * @brief Stop audio capture task
 * @return ESP_OK on success
 */
esp_err_t audio_capture_stop(void);

/**
 * @brief Check if capture is running
 * @return true if running
 */
bool audio_capture_is_running(void);

/**
 * @brief Read audio data from capture buffer
 * @param buffer Output buffer
 * @param len Buffer length in bytes
 * @param timeout_ms Read timeout
 * @return Bytes read, or negative on error
 */
int audio_capture_read(uint8_t *buffer, size_t len, uint32_t timeout_ms);

/**
 * @brief Get available bytes in capture buffer
 * @return Available bytes
 */
size_t audio_capture_available(void);

/**
 * @brief Clear capture buffer
 * @return ESP_OK on success
 */
esp_err_t audio_capture_clear(void);

/**
 * @brief Get raw stereo samples (for DOA processing)
 * @param left Output buffer for left channel
 * @param right Output buffer for right channel
 * @param samples Number of samples to read
 * @param timeout_ms Read timeout
 * @return Samples read, or negative on error
 */
int audio_capture_read_stereo(int16_t *left, int16_t *right, size_t samples, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_CAPTURE_H
