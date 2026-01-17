/**
 * SDL2 Audio Driver for Luna Simulator
 * Provides microphone capture and speaker playback
 */

#ifndef SDL_AUDIO_H
#define SDL_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Audio format: 16kHz, 16-bit, mono */
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_CHANNELS      1
#define AUDIO_BITS          16
#define AUDIO_CHUNK_SAMPLES 320     /* 20ms at 16kHz */
#define AUDIO_CHUNK_BYTES   (AUDIO_CHUNK_SAMPLES * 2)  /* 640 bytes */

/**
 * Audio capture callback
 * Called when microphone data is available
 * @param data PCM audio data (16-bit mono)
 * @param samples Number of samples
 */
typedef void (*audio_capture_callback_t)(const int16_t* data, size_t samples);

/**
 * Initialize audio system
 * @return true on success
 */
bool sdl_audio_init(void);

/**
 * Deinitialize audio system
 */
void sdl_audio_deinit(void);

/**
 * Start audio capture (microphone)
 * @param callback Function called with captured audio
 * @return true on success
 */
bool sdl_audio_capture_start(audio_capture_callback_t callback);

/**
 * Stop audio capture
 */
void sdl_audio_capture_stop(void);

/**
 * Start audio playback (speaker)
 * @return true on success
 */
bool sdl_audio_playback_start(void);

/**
 * Stop audio playback
 */
void sdl_audio_playback_stop(void);

/**
 * Feed audio data for playback
 * @param data PCM audio data (16-bit mono)
 * @param samples Number of samples
 * @return Number of samples actually queued
 */
size_t sdl_audio_playback_feed(const int16_t* data, size_t samples);

/**
 * Check if audio is initialized
 */
bool sdl_audio_is_init(void);

/**
 * Get capture buffer level (for debugging)
 */
size_t sdl_audio_capture_available(void);

/**
 * Get playback buffer level (for debugging)
 */
size_t sdl_audio_playback_available(void);

#endif /* SDL_AUDIO_H */
