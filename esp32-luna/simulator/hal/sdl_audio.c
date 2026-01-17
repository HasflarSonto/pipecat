/**
 * SDL2 Audio Driver for Luna Simulator
 * Uses SDL2 audio for microphone capture and speaker playback
 */

#include "sdl_audio.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ring buffer size (in samples) - 2 seconds */
#define RING_BUFFER_SIZE (AUDIO_SAMPLE_RATE * 2)

/* State */
static bool s_initialized = false;
static SDL_AudioDeviceID s_capture_device = 0;
static SDL_AudioDeviceID s_playback_device = 0;
static audio_capture_callback_t s_capture_callback = NULL;

/* Capture ring buffer */
static int16_t* s_capture_buffer = NULL;
static size_t s_capture_read_idx = 0;
static size_t s_capture_write_idx = 0;
static SDL_mutex* s_capture_mutex = NULL;

/* Playback ring buffer */
static int16_t* s_playback_buffer = NULL;
static size_t s_playback_read_idx = 0;
static size_t s_playback_write_idx = 0;
static SDL_mutex* s_playback_mutex = NULL;

/* Temporary buffer for callback */
static int16_t s_temp_buffer[AUDIO_CHUNK_SAMPLES];

/**
 * Get available samples in ring buffer
 */
static size_t ring_available(size_t read_idx, size_t write_idx)
{
    if (write_idx >= read_idx) {
        return write_idx - read_idx;
    }
    return RING_BUFFER_SIZE - read_idx + write_idx;
}

/**
 * Get free space in ring buffer
 */
static size_t ring_free(size_t read_idx, size_t write_idx)
{
    return RING_BUFFER_SIZE - 1 - ring_available(read_idx, write_idx);
}

/**
 * SDL capture callback - called when mic data available
 */
static void capture_callback(void* userdata, Uint8* stream, int len)
{
    (void)userdata;

    int samples = len / sizeof(int16_t);
    int16_t* data = (int16_t*)stream;

    SDL_LockMutex(s_capture_mutex);

    /* Write to ring buffer */
    for (int i = 0; i < samples; i++) {
        size_t next = (s_capture_write_idx + 1) % RING_BUFFER_SIZE;
        if (next != s_capture_read_idx) {
            s_capture_buffer[s_capture_write_idx] = data[i];
            s_capture_write_idx = next;
        }
    }

    SDL_UnlockMutex(s_capture_mutex);

    /* Call user callback with captured data */
    if (s_capture_callback) {
        s_capture_callback(data, samples);
    }
}

/**
 * SDL playback callback - called when speaker needs data
 */
static void playback_callback(void* userdata, Uint8* stream, int len)
{
    (void)userdata;

    int samples = len / sizeof(int16_t);
    int16_t* data = (int16_t*)stream;

    SDL_LockMutex(s_playback_mutex);

    size_t available = ring_available(s_playback_read_idx, s_playback_write_idx);

    if (available >= (size_t)samples) {
        /* Read from ring buffer */
        for (int i = 0; i < samples; i++) {
            data[i] = s_playback_buffer[s_playback_read_idx];
            s_playback_read_idx = (s_playback_read_idx + 1) % RING_BUFFER_SIZE;
        }
    } else {
        /* Not enough data - output silence and what we have */
        memset(stream, 0, len);
        for (size_t i = 0; i < available; i++) {
            data[i] = s_playback_buffer[s_playback_read_idx];
            s_playback_read_idx = (s_playback_read_idx + 1) % RING_BUFFER_SIZE;
        }
    }

    SDL_UnlockMutex(s_playback_mutex);
}

bool sdl_audio_init(void)
{
    if (s_initialized) {
        return true;
    }

    /* Allocate ring buffers */
    s_capture_buffer = calloc(RING_BUFFER_SIZE, sizeof(int16_t));
    s_playback_buffer = calloc(RING_BUFFER_SIZE, sizeof(int16_t));

    if (!s_capture_buffer || !s_playback_buffer) {
        fprintf(stderr, "Failed to allocate audio buffers\n");
        free(s_capture_buffer);
        free(s_playback_buffer);
        return false;
    }

    /* Create mutexes */
    s_capture_mutex = SDL_CreateMutex();
    s_playback_mutex = SDL_CreateMutex();

    if (!s_capture_mutex || !s_playback_mutex) {
        fprintf(stderr, "Failed to create audio mutexes\n");
        free(s_capture_buffer);
        free(s_playback_buffer);
        SDL_DestroyMutex(s_capture_mutex);
        SDL_DestroyMutex(s_playback_mutex);
        return false;
    }

    s_initialized = true;
    printf("SDL audio initialized\n");
    return true;
}

void sdl_audio_deinit(void)
{
    if (!s_initialized) return;

    sdl_audio_capture_stop();
    sdl_audio_playback_stop();

    if (s_capture_mutex) {
        SDL_DestroyMutex(s_capture_mutex);
        s_capture_mutex = NULL;
    }

    if (s_playback_mutex) {
        SDL_DestroyMutex(s_playback_mutex);
        s_playback_mutex = NULL;
    }

    free(s_capture_buffer);
    free(s_playback_buffer);
    s_capture_buffer = NULL;
    s_playback_buffer = NULL;

    s_initialized = false;
    printf("SDL audio deinitialized\n");
}

bool sdl_audio_capture_start(audio_capture_callback_t callback)
{
    if (!s_initialized) {
        fprintf(stderr, "Audio not initialized\n");
        return false;
    }

    if (s_capture_device != 0) {
        /* Already running */
        s_capture_callback = callback;
        return true;
    }

    s_capture_callback = callback;

    /* Configure audio spec */
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = AUDIO_CHANNELS;
    want.samples = AUDIO_CHUNK_SAMPLES;
    want.callback = capture_callback;

    /* Open capture device (NULL = default mic) */
    s_capture_device = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);

    if (s_capture_device == 0) {
        fprintf(stderr, "Failed to open capture device: %s\n", SDL_GetError());
        return false;
    }

    if (have.freq != AUDIO_SAMPLE_RATE || have.channels != AUDIO_CHANNELS) {
        fprintf(stderr, "Warning: Audio format differs from requested (got %dHz %dch)\n",
                have.freq, have.channels);
    }

    /* Reset ring buffer */
    s_capture_read_idx = 0;
    s_capture_write_idx = 0;

    /* Start capture */
    SDL_PauseAudioDevice(s_capture_device, 0);

    printf("Audio capture started\n");
    return true;
}

void sdl_audio_capture_stop(void)
{
    if (s_capture_device != 0) {
        SDL_CloseAudioDevice(s_capture_device);
        s_capture_device = 0;
        printf("Audio capture stopped\n");
    }
    s_capture_callback = NULL;
}

bool sdl_audio_playback_start(void)
{
    if (!s_initialized) {
        fprintf(stderr, "Audio not initialized\n");
        return false;
    }

    if (s_playback_device != 0) {
        return true;
    }

    /* Configure audio spec */
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = AUDIO_CHANNELS;
    want.samples = AUDIO_CHUNK_SAMPLES;
    want.callback = playback_callback;

    /* Open playback device (NULL = default speaker) */
    s_playback_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

    if (s_playback_device == 0) {
        fprintf(stderr, "Failed to open playback device: %s\n", SDL_GetError());
        return false;
    }

    /* Reset ring buffer */
    s_playback_read_idx = 0;
    s_playback_write_idx = 0;

    /* Start playback */
    SDL_PauseAudioDevice(s_playback_device, 0);

    printf("Audio playback started\n");
    return true;
}

void sdl_audio_playback_stop(void)
{
    if (s_playback_device != 0) {
        SDL_CloseAudioDevice(s_playback_device);
        s_playback_device = 0;
        printf("Audio playback stopped\n");
    }
}

size_t sdl_audio_playback_feed(const int16_t* data, size_t samples)
{
    if (!s_initialized || s_playback_device == 0 || !data) {
        return 0;
    }

    SDL_LockMutex(s_playback_mutex);

    size_t free_space = ring_free(s_playback_read_idx, s_playback_write_idx);
    size_t to_write = (samples < free_space) ? samples : free_space;

    for (size_t i = 0; i < to_write; i++) {
        s_playback_buffer[s_playback_write_idx] = data[i];
        s_playback_write_idx = (s_playback_write_idx + 1) % RING_BUFFER_SIZE;
    }

    SDL_UnlockMutex(s_playback_mutex);

    return to_write;
}

bool sdl_audio_is_init(void)
{
    return s_initialized;
}

size_t sdl_audio_capture_available(void)
{
    if (!s_initialized) return 0;
    SDL_LockMutex(s_capture_mutex);
    size_t avail = ring_available(s_capture_read_idx, s_capture_write_idx);
    SDL_UnlockMutex(s_capture_mutex);
    return avail;
}

size_t sdl_audio_playback_available(void)
{
    if (!s_initialized) return 0;
    SDL_LockMutex(s_playback_mutex);
    size_t avail = ring_available(s_playback_read_idx, s_playback_write_idx);
    SDL_UnlockMutex(s_playback_mutex);
    return avail;
}
