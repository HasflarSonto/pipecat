/*
 * Audio Playback for ESP32-Luna
 * Speaker output with FIFO buffer for WebSocket streaming
 */

#include "audio_playback.h"
#include "audio_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "audio_playback";

#define PLAYBACK_TASK_STACK_SIZE  (4 * 1024)
#define PLAYBACK_TASK_PRIORITY    6
#define DEFAULT_BUFFER_SIZE       (32 * 1024)  // 32KB ring buffer (2 seconds at 16kHz mono)
#define PLAYBACK_CHUNK_SIZE       (AUDIO_CHUNK_SAMPLES * sizeof(int16_t))

static RingbufHandle_t s_ringbuf = NULL;
static TaskHandle_t s_playback_task = NULL;
static audio_playback_state_t s_state = AUDIO_PLAYBACK_IDLE;
static bool s_initialized = false;
static size_t s_buffer_size = DEFAULT_BUFFER_SIZE;

static audio_playback_cb_t s_callback = NULL;
static void *s_callback_ctx = NULL;

static int16_t s_playback_buffer[AUDIO_CHUNK_SAMPLES];
static uint8_t s_silence_buffer[PLAYBACK_CHUNK_SIZE];  // Zero-filled silence

static void set_state(audio_playback_state_t new_state)
{
    if (s_state != new_state) {
        s_state = new_state;
        if (s_callback) {
            s_callback(new_state, s_callback_ctx);
        }
    }
}

static void playback_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Playback task started");

    while (s_state != AUDIO_PLAYBACK_IDLE || s_initialized) {
        if (s_state == AUDIO_PLAYBACK_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Try to get audio data from buffer
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &item_size,
                                            pdMS_TO_TICKS(20), PLAYBACK_CHUNK_SIZE);

        if (item != NULL && item_size > 0) {
            // Play audio data
            size_t bytes_written = 0;
            audio_manager_write(item, item_size, &bytes_written, portMAX_DELAY);
            vRingbufferReturnItem(s_ringbuf, item);
        } else {
            // Buffer empty - play silence to keep codec running
            // This prevents pops/clicks on buffer underrun
            size_t bytes_written = 0;
            audio_manager_write(s_silence_buffer, PLAYBACK_CHUNK_SIZE,
                               &bytes_written, portMAX_DELAY);
        }
    }

    ESP_LOGI(TAG, "Playback task stopped");
    vTaskDelete(NULL);
}

esp_err_t audio_playback_init(const audio_playback_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_buffer_size = DEFAULT_BUFFER_SIZE;

    if (config) {
        if (config->buffer_size > 0) {
            s_buffer_size = config->buffer_size;
        }
        s_callback = config->callback;
        s_callback_ctx = config->callback_ctx;
    }

    // Create ring buffer
    s_ringbuf = xRingbufferCreate(s_buffer_size, RINGBUF_TYPE_BYTEBUF);
    if (s_ringbuf == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return ESP_ERR_NO_MEM;
    }

    // Initialize silence buffer
    memset(s_silence_buffer, 0, sizeof(s_silence_buffer));

    s_state = AUDIO_PLAYBACK_IDLE;
    s_initialized = true;
    ESP_LOGI(TAG, "Audio playback initialized (buffer=%d bytes)", (int)s_buffer_size);

    return ESP_OK;
}

esp_err_t audio_playback_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    audio_playback_stop();

    if (s_ringbuf) {
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Audio playback deinitialized");

    return ESP_OK;
}

esp_err_t audio_playback_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == AUDIO_PLAYBACK_PLAYING) {
        return ESP_OK;
    }

    set_state(AUDIO_PLAYBACK_PLAYING);

    if (s_playback_task == NULL) {
        BaseType_t ret = xTaskCreate(playback_task, "audio_playback",
                                     PLAYBACK_TASK_STACK_SIZE, NULL,
                                     PLAYBACK_TASK_PRIORITY, &s_playback_task);

        if (ret != pdPASS) {
            set_state(AUDIO_PLAYBACK_IDLE);
            ESP_LOGE(TAG, "Failed to create playback task");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Audio playback started");
    return ESP_OK;
}

esp_err_t audio_playback_stop(void)
{
    if (s_state == AUDIO_PLAYBACK_IDLE && s_playback_task == NULL) {
        return ESP_OK;
    }

    set_state(AUDIO_PLAYBACK_IDLE);

    // Wait for task to finish
    if (s_playback_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_playback_task = NULL;
    }

    // Clear buffer
    audio_playback_clear();

    ESP_LOGI(TAG, "Audio playback stopped");
    return ESP_OK;
}

esp_err_t audio_playback_pause(void)
{
    if (!s_initialized || s_state == AUDIO_PLAYBACK_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    set_state(AUDIO_PLAYBACK_PAUSED);
    ESP_LOGI(TAG, "Audio playback paused");
    return ESP_OK;
}

esp_err_t audio_playback_resume(void)
{
    if (!s_initialized || s_state != AUDIO_PLAYBACK_PAUSED) {
        return ESP_ERR_INVALID_STATE;
    }

    set_state(AUDIO_PLAYBACK_PLAYING);
    ESP_LOGI(TAG, "Audio playback resumed");
    return ESP_OK;
}

audio_playback_state_t audio_playback_get_state(void)
{
    return s_state;
}

esp_err_t audio_playback_feed(const uint8_t *data, size_t len)
{
    if (!s_initialized || s_ringbuf == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // Try to send to ring buffer
    BaseType_t sent = xRingbufferSend(s_ringbuf, data, len, pdMS_TO_TICKS(10));

    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "Playback buffer full, dropping %d bytes", (int)len);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

size_t audio_playback_available(void)
{
    if (!s_initialized || s_ringbuf == NULL) {
        return 0;
    }

    return xRingbufferGetCurFreeSize(s_ringbuf);
}

esp_err_t audio_playback_clear(void)
{
    if (!s_initialized || s_ringbuf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Read and discard all data
    size_t item_size;
    void *item;
    while ((item = xRingbufferReceive(s_ringbuf, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(s_ringbuf, item);
    }

    return ESP_OK;
}

float audio_playback_fill_level(void)
{
    if (!s_initialized || s_ringbuf == NULL) {
        return 0.0f;
    }

    size_t free_size = xRingbufferGetCurFreeSize(s_ringbuf);
    size_t used_size = s_buffer_size - free_size;

    return (float)used_size / (float)s_buffer_size;
}
