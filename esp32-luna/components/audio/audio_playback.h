/*
 * Audio Playback for ESP32-Luna
 * Speaker output with FIFO buffer for WebSocket streaming
 */

#ifndef AUDIO_PLAYBACK_H
#define AUDIO_PLAYBACK_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Playback state
 */
typedef enum {
    AUDIO_PLAYBACK_IDLE,
    AUDIO_PLAYBACK_PLAYING,
    AUDIO_PLAYBACK_PAUSED,
} audio_playback_state_t;

/**
 * @brief Playback event callback
 * @param state New playback state
 * @param ctx User context
 */
typedef void (*audio_playback_cb_t)(audio_playback_state_t state, void *ctx);

/**
 * @brief Playback configuration
 */
typedef struct {
    size_t buffer_size;           // FIFO buffer size in bytes (0 = default)
    audio_playback_cb_t callback; // Optional state callback
    void *callback_ctx;           // Callback context
} audio_playback_config_t;

/**
 * @brief Initialize audio playback
 * @param config Playback configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t audio_playback_init(const audio_playback_config_t *config);

/**
 * @brief Deinitialize audio playback
 * @return ESP_OK on success
 */
esp_err_t audio_playback_deinit(void);

/**
 * @brief Start playback task
 * @return ESP_OK on success
 */
esp_err_t audio_playback_start(void);

/**
 * @brief Stop playback task
 * @return ESP_OK on success
 */
esp_err_t audio_playback_stop(void);

/**
 * @brief Pause playback (keeps task running, stops output)
 * @return ESP_OK on success
 */
esp_err_t audio_playback_pause(void);

/**
 * @brief Resume playback
 * @return ESP_OK on success
 */
esp_err_t audio_playback_resume(void);

/**
 * @brief Get current playback state
 * @return Current state
 */
audio_playback_state_t audio_playback_get_state(void);

/**
 * @brief Feed audio data to playback buffer
 * @param data PCM audio data (mono, 16-bit, 16kHz)
 * @param len Data length in bytes
 * @return ESP_OK on success, ESP_ERR_NO_MEM if buffer full
 */
esp_err_t audio_playback_feed(const uint8_t *data, size_t len);

/**
 * @brief Get available space in playback buffer
 * @return Available bytes
 */
size_t audio_playback_available(void);

/**
 * @brief Clear playback buffer
 * @return ESP_OK on success
 */
esp_err_t audio_playback_clear(void);

/**
 * @brief Get playback buffer fill level (0.0 - 1.0)
 * @return Fill level
 */
float audio_playback_fill_level(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_PLAYBACK_H
