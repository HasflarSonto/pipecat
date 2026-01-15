/*
 * Audio Manager for ESP32-Luna
 * ES8311 codec initialization and configuration
 * Based on esp-brookesia and Spec_Analyzer patterns
 */

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audio configuration
 */
#define AUDIO_SAMPLE_RATE       16000   // 16kHz for Pipecat/OpenAI compatibility
#define AUDIO_BIT_WIDTH         16      // 16-bit samples
#define AUDIO_CHANNELS          2       // Stereo input (for DOA)
#define AUDIO_OUTPUT_CHANNELS   1       // Mono output
#define AUDIO_DEFAULT_VOLUME    70      // Default volume (0-100)
#define AUDIO_CHUNK_MS          20      // 20ms audio chunks
#define AUDIO_CHUNK_SAMPLES     (AUDIO_SAMPLE_RATE * AUDIO_CHUNK_MS / 1000)

/**
 * @brief Initialize audio manager (ES8311 codec)
 * @return ESP_OK on success
 */
esp_err_t audio_manager_init(void);

/**
 * @brief Deinitialize audio manager
 * @return ESP_OK on success
 */
esp_err_t audio_manager_deinit(void);

/**
 * @brief Set playback volume
 * @param volume Volume level (0-100)
 * @return ESP_OK on success
 */
esp_err_t audio_manager_set_volume(int volume);

/**
 * @brief Get current playback volume
 * @return Current volume level
 */
int audio_manager_get_volume(void);

/**
 * @brief Set playback mute
 * @param mute true to mute, false to unmute
 * @return ESP_OK on success
 */
esp_err_t audio_manager_set_mute(bool mute);

/**
 * @brief Read audio from microphone (blocking)
 * @param buffer Output buffer for PCM samples
 * @param len Buffer length in bytes
 * @param bytes_read Actual bytes read
 * @param timeout_ms Read timeout
 * @return ESP_OK on success
 */
esp_err_t audio_manager_read(void *buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms);

/**
 * @brief Write audio to speaker (blocking)
 * @param buffer Input buffer with PCM samples
 * @param len Buffer length in bytes
 * @param bytes_written Actual bytes written
 * @param timeout_ms Write timeout
 * @return ESP_OK on success
 */
esp_err_t audio_manager_write(const void *buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms);

/**
 * @brief Suspend audio (for power saving)
 * @param suspend true to suspend, false to resume
 * @return ESP_OK on success
 */
esp_err_t audio_manager_suspend(bool suspend);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_MANAGER_H
