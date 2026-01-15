/*
 * Audio Manager for ESP32-Luna
 * ES8311 codec initialization and configuration
 * Based on esp-brookesia and Spec_Analyzer patterns
 */

#include "audio_manager.h"
#include "esp_log.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "audio_manager";

static esp_codec_dev_handle_t s_play_dev = NULL;
static esp_codec_dev_handle_t s_record_dev = NULL;
static bool s_initialized = false;
static int s_volume = AUDIO_DEFAULT_VOLUME;
static bool s_muted = false;

esp_err_t audio_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing audio manager...");

    // Initialize speaker codec
    s_play_dev = bsp_audio_codec_speaker_init();
    if (s_play_dev == NULL) {
        ESP_LOGE(TAG, "Failed to init speaker codec");
        return ESP_FAIL;
    }

    // Initialize microphone codec
    s_record_dev = bsp_audio_codec_microphone_init();
    if (s_record_dev == NULL) {
        ESP_LOGE(TAG, "Failed to init microphone codec");
        return ESP_FAIL;
    }

    // Configure sample format
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BIT_WIDTH,
    };

    // Open playback device
    esp_err_t ret = esp_codec_dev_open(s_play_dev, &sample_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open playback device: %d", ret);
        return ret;
    }

    // Open recording device
    ret = esp_codec_dev_open(s_record_dev, &sample_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open recording device: %d", ret);
        return ret;
    }

    // Set default volume
    ret = esp_codec_dev_set_out_vol(s_play_dev, s_volume);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set volume: %d", ret);
    }

    // Set microphone gain
    ret = esp_codec_dev_set_in_gain(s_record_dev, 24.0f);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set mic gain: %d", ret);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Audio manager initialized (rate=%d, bits=%d, ch=%d)",
             AUDIO_SAMPLE_RATE, AUDIO_BIT_WIDTH, AUDIO_CHANNELS);

    return ESP_OK;
}

esp_err_t audio_manager_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_play_dev) {
        esp_codec_dev_close(s_play_dev);
        s_play_dev = NULL;
    }

    if (s_record_dev) {
        esp_codec_dev_close(s_record_dev);
        s_record_dev = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Audio manager deinitialized");

    return ESP_OK;
}

esp_err_t audio_manager_set_volume(int volume)
{
    if (!s_initialized || s_play_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Clamp volume
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    esp_err_t ret = esp_codec_dev_set_out_vol(s_play_dev, volume);
    if (ret == ESP_OK) {
        s_volume = volume;
        ESP_LOGI(TAG, "Volume set to %d", volume);
    }

    return ret;
}

int audio_manager_get_volume(void)
{
    return s_volume;
}

esp_err_t audio_manager_set_mute(bool mute)
{
    if (!s_initialized || s_play_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_codec_dev_set_out_mute(s_play_dev, mute);
    if (ret == ESP_OK) {
        s_muted = mute;
        ESP_LOGI(TAG, "Mute %s", mute ? "enabled" : "disabled");

        // Restore volume when unmuting
        if (!mute) {
            esp_codec_dev_set_out_vol(s_play_dev, s_volume);
        }
    }

    return ret;
}

esp_err_t audio_manager_read(void *buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!s_initialized || s_record_dev == NULL || buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_codec_dev_read(s_record_dev, buffer, len);
    if (ret == ESP_OK && bytes_read != NULL) {
        *bytes_read = len;
    }

    return ret;
}

esp_err_t audio_manager_write(const void *buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (!s_initialized || s_play_dev == NULL || buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_codec_dev_write(s_play_dev, (void *)buffer, len);
    if (ret == ESP_OK && bytes_written != NULL) {
        *bytes_written = len;
    }

    return ret;
}

esp_err_t audio_manager_suspend(bool suspend)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (suspend) {
        if (s_play_dev) {
            esp_codec_dev_close(s_play_dev);
        }
        if (s_record_dev) {
            esp_codec_dev_close(s_record_dev);
        }
        ESP_LOGI(TAG, "Audio suspended");
    } else {
        esp_codec_dev_sample_info_t sample_info = {
            .sample_rate = AUDIO_SAMPLE_RATE,
            .channel = AUDIO_CHANNELS,
            .bits_per_sample = AUDIO_BIT_WIDTH,
        };

        if (s_play_dev) {
            esp_codec_dev_open(s_play_dev, &sample_info);
            esp_codec_dev_set_out_vol(s_play_dev, s_volume);
        }
        if (s_record_dev) {
            esp_codec_dev_open(s_record_dev, &sample_info);
        }
        ESP_LOGI(TAG, "Audio resumed");
    }

    return ESP_OK;
}
