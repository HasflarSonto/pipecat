/*
 * Audio Capture for ESP32-Luna
 * Microphone input with ring buffer for WebSocket streaming
 */

#include "audio_capture.h"
#include "audio_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "audio_capture";

#define CAPTURE_TASK_STACK_SIZE  (4 * 1024)
#define CAPTURE_TASK_PRIORITY    6
#define DEFAULT_BUFFER_SIZE      (16 * 1024)  // 16KB ring buffer
#define CAPTURE_CHUNK_SIZE       (AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))

static RingbufHandle_t s_ringbuf = NULL;
static RingbufHandle_t s_stereo_ringbuf = NULL;  // For DOA processing
static TaskHandle_t s_capture_task = NULL;
static bool s_running = false;
static bool s_initialized = false;

static audio_capture_cb_t s_callback = NULL;
static void *s_callback_ctx = NULL;
static bool s_stereo_to_mono = true;

static int16_t s_capture_buffer[AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS];
static int16_t s_mono_buffer[AUDIO_CHUNK_SAMPLES];

static void capture_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Capture task started");

    while (s_running) {
        size_t bytes_read = 0;

        // Read stereo audio from microphone
        esp_err_t ret = audio_manager_read(s_capture_buffer, CAPTURE_CHUNK_SIZE,
                                           &bytes_read, portMAX_DELAY);

        if (ret != ESP_OK || bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        int samples = bytes_read / (AUDIO_CHANNELS * sizeof(int16_t));

        // Store stereo data for DOA processing
        if (s_stereo_ringbuf) {
            xRingbufferSend(s_stereo_ringbuf, s_capture_buffer, bytes_read, 0);
        }

        // Convert to mono if needed
        int16_t *output_buffer;
        size_t output_size;

        if (s_stereo_to_mono && AUDIO_CHANNELS == 2) {
            // Average left and right channels
            for (int i = 0; i < samples; i++) {
                int32_t left = s_capture_buffer[i * 2];
                int32_t right = s_capture_buffer[i * 2 + 1];
                s_mono_buffer[i] = (int16_t)((left + right) / 2);
            }
            output_buffer = s_mono_buffer;
            output_size = samples * sizeof(int16_t);
        } else {
            output_buffer = s_capture_buffer;
            output_size = bytes_read;
        }

        // Send to ring buffer
        if (s_ringbuf) {
            BaseType_t sent = xRingbufferSend(s_ringbuf, output_buffer, output_size, 0);
            if (sent != pdTRUE) {
                // Throttle warning to reduce log spam
                static int drop_count = 0;
                if (++drop_count >= 100) {
                    ESP_LOGW(TAG, "Ring buffer full, dropped %d chunks", drop_count);
                    drop_count = 0;
                }
            }
        }

        // Call callback if registered
        if (s_callback) {
            s_callback(output_buffer, output_size / sizeof(int16_t), s_callback_ctx);
        }
    }

    ESP_LOGI(TAG, "Capture task stopped");
    vTaskDelete(NULL);
}

esp_err_t audio_capture_init(const audio_capture_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }

    size_t buffer_size = DEFAULT_BUFFER_SIZE;

    if (config) {
        if (config->buffer_size > 0) {
            buffer_size = config->buffer_size;
        }
        s_callback = config->callback;
        s_callback_ctx = config->callback_ctx;
        s_stereo_to_mono = config->stereo_to_mono;
    }

    // Create main ring buffer (mono output)
    s_ringbuf = xRingbufferCreate(buffer_size, RINGBUF_TYPE_BYTEBUF);
    if (s_ringbuf == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return ESP_ERR_NO_MEM;
    }

    // Create stereo ring buffer for DOA (smaller, just a few chunks)
    s_stereo_ringbuf = xRingbufferCreate(CAPTURE_CHUNK_SIZE * 4, RINGBUF_TYPE_BYTEBUF);
    if (s_stereo_ringbuf == NULL) {
        ESP_LOGW(TAG, "Failed to create stereo ring buffer, DOA disabled");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Audio capture initialized (buffer=%d bytes)", (int)buffer_size);

    return ESP_OK;
}

esp_err_t audio_capture_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    audio_capture_stop();

    if (s_ringbuf) {
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
    }

    if (s_stereo_ringbuf) {
        vRingbufferDelete(s_stereo_ringbuf);
        s_stereo_ringbuf = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Audio capture deinitialized");

    return ESP_OK;
}

esp_err_t audio_capture_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running) {
        return ESP_OK;
    }

    s_running = true;

    BaseType_t ret = xTaskCreate(capture_task, "audio_capture",
                                 CAPTURE_TASK_STACK_SIZE, NULL,
                                 CAPTURE_TASK_PRIORITY, &s_capture_task);

    if (ret != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "Failed to create capture task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio capture started");
    return ESP_OK;
}

esp_err_t audio_capture_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }

    s_running = false;

    // Wait for task to finish
    if (s_capture_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_capture_task = NULL;
    }

    ESP_LOGI(TAG, "Audio capture stopped");
    return ESP_OK;
}

bool audio_capture_is_running(void)
{
    return s_running;
}

int audio_capture_read(uint8_t *buffer, size_t len, uint32_t timeout_ms)
{
    if (!s_initialized || s_ringbuf == NULL || buffer == NULL) {
        return -1;
    }

    size_t item_size = 0;
    void *item = xRingbufferReceiveUpTo(s_ringbuf, &item_size, pdMS_TO_TICKS(timeout_ms), len);

    if (item == NULL) {
        return 0;
    }

    memcpy(buffer, item, item_size);
    vRingbufferReturnItem(s_ringbuf, item);

    return (int)item_size;
}

size_t audio_capture_available(void)
{
    if (!s_initialized || s_ringbuf == NULL) {
        return 0;
    }

    UBaseType_t free_size = xRingbufferGetCurFreeSize(s_ringbuf);
    size_t total_size = DEFAULT_BUFFER_SIZE;  // Approximation

    return (total_size > free_size) ? (total_size - free_size) : 0;
}

esp_err_t audio_capture_clear(void)
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

int audio_capture_read_stereo(int16_t *left, int16_t *right, size_t samples, uint32_t timeout_ms)
{
    if (!s_initialized || s_stereo_ringbuf == NULL || left == NULL || right == NULL) {
        return -1;
    }

    size_t bytes_needed = samples * AUDIO_CHANNELS * sizeof(int16_t);
    size_t item_size = 0;
    void *item = xRingbufferReceiveUpTo(s_stereo_ringbuf, &item_size,
                                        pdMS_TO_TICKS(timeout_ms), bytes_needed);

    if (item == NULL) {
        return 0;
    }

    // Deinterleave stereo data
    int16_t *stereo = (int16_t *)item;
    int sample_count = item_size / (AUDIO_CHANNELS * sizeof(int16_t));

    for (int i = 0; i < sample_count; i++) {
        left[i] = stereo[i * 2];
        right[i] = stereo[i * 2 + 1];
    }

    vRingbufferReturnItem(s_stereo_ringbuf, item);

    return sample_count;
}
