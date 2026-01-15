/*
 * ESP32-Luna Main Application
 * Voice assistant frontend for Pipecat backend
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_manager.h"
#include "ws_client.h"
#include "luna_protocol.h"
#include "audio_manager.h"
#include "audio_capture.h"
#include "audio_playback.h"
#include "face_renderer.h"
#include "pmu_manager.h"

static const char *TAG = "luna_main";

// Configuration - TODO: Move to menuconfig or NVS
#define WIFI_SSID           CONFIG_LUNA_WIFI_SSID
#define WIFI_PASSWORD       CONFIG_LUNA_WIFI_PASSWORD
#define SERVER_IP           CONFIG_LUNA_SERVER_IP
#define SERVER_PORT         CONFIG_LUNA_SERVER_PORT

// Event group for synchronization
static EventGroupHandle_t s_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WS_CONNECTED_BIT    BIT1

// Forward declarations
static void wifi_event_handler(wifi_manager_event_t event, void *ctx);
static void ws_event_handler(ws_client_event_data_t *event, void *ctx);
static void luna_cmd_handler(const luna_cmd_t *cmd, void *ctx);
static void audio_capture_handler(const int16_t *data, size_t len, void *ctx);

/**
 * @brief Main application entry point
 */
void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== ESP32-Luna Starting ===");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create event group
    s_event_group = xEventGroupCreate();

    // Initialize Power Management (AXP2101)
    ESP_LOGI(TAG, "Initializing power management...");
    ret = pmu_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PMU init failed, continuing without power management");
    } else {
        ESP_LOGI(TAG, "Battery: %d%%", pmu_manager_get_battery_percent());
    }

    // Initialize Face Renderer (canvas-based, landscape mode)
    ESP_LOGI(TAG, "Initializing face renderer...");
    face_renderer_config_t face_config = {
        .width = 502,   // Landscape after 90° rotation
        .height = 410,
        .cat_mode = false,
    };
    ESP_ERROR_CHECK(face_renderer_init(&face_config));
    ESP_ERROR_CHECK(face_renderer_start());
    ESP_LOGI(TAG, "Face renderer started");

    // Initialize Audio Manager
    ESP_LOGI(TAG, "Initializing audio...");
    ESP_ERROR_CHECK(audio_manager_init());

    // Initialize Audio Capture
    audio_capture_config_t capture_config = {
        .buffer_size = 0,  // Default
        .callback = audio_capture_handler,
        .callback_ctx = NULL,
        .stereo_to_mono = true,
    };
    ESP_ERROR_CHECK(audio_capture_init(&capture_config));

    // Initialize Audio Playback
    ESP_ERROR_CHECK(audio_playback_init(NULL));

    // Initialize WiFi
    ESP_LOGI(TAG, "Initializing WiFi...");
    ESP_ERROR_CHECK(wifi_manager_init());
    wifi_manager_set_event_callback(wifi_event_handler, NULL);

    // Initialize WebSocket Client
    ws_client_config_t ws_config = {
        .server_port = SERVER_PORT,
        .reconnect_ms = 5000,
    };
    strncpy(ws_config.server_ip, SERVER_IP, sizeof(ws_config.server_ip) - 1);
    strncpy(ws_config.endpoint, "/luna-esp32", sizeof(ws_config.endpoint) - 1);
    ESP_ERROR_CHECK(ws_client_init(&ws_config));
    ws_client_set_event_callback(ws_event_handler, NULL);

    // Initialize Protocol Parser
    ESP_ERROR_CHECK(luna_protocol_init());
    luna_protocol_set_handler(luna_cmd_handler, NULL);

    // Connect to WiFi
    ESP_LOGI(TAG, "Connecting to WiFi: %s", WIFI_SSID);
    wifi_manager_config_t wifi_config = {
        .store_in_nvs = true,
    };
    strncpy(wifi_config.ssid, WIFI_SSID, sizeof(wifi_config.ssid) - 1);
    strncpy(wifi_config.password, WIFI_PASSWORD, sizeof(wifi_config.password) - 1);
    ESP_ERROR_CHECK(wifi_manager_connect(&wifi_config));

    // Wait for WiFi connection
    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi connection timeout!");
        face_renderer_set_emotion(EMOTION_SAD);
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    // Connect WebSocket
    ESP_LOGI(TAG, "Connecting to server...");
    ESP_ERROR_CHECK(ws_client_connect());

    // Wait for WebSocket connection
    bits = xEventGroupWaitBits(s_event_group,
                               WS_CONNECTED_BIT,
                               pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(10000));
    if (!(bits & WS_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WebSocket connection timeout!");
        face_renderer_set_emotion(EMOTION_CONFUSED);
        // DON'T start audio capture when not connected - saves CPU and prevents ring buffer floods
        ESP_LOGI(TAG, "Audio capture NOT started (no server connection)");
    } else {
        // Start audio streaming only when connected
        ESP_LOGI(TAG, "Starting audio capture...");
        ESP_ERROR_CHECK(audio_capture_start());
        ESP_ERROR_CHECK(audio_playback_start());
        face_renderer_set_emotion(EMOTION_HAPPY);
    }

    ESP_LOGI(TAG, "=== ESP32-Luna Ready ===");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    // Emotion names for cycling demo - all emotions
    static const char *emotion_names[] = {
        "neutral", "happy", "sad", "angry", "surprised", "thinking", "confused", "excited", "cat"
    };
    static const int emotion_count = sizeof(emotion_names) / sizeof(emotion_names[0]);
    int emotion_index = 0;
    int emotion_cycle_counter = 0;

    // Main loop - just keep running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Emotion cycling demo when not connected to server (every 3 seconds)
        if (!ws_client_is_connected()) {
            if (++emotion_cycle_counter >= 3) {
                emotion_cycle_counter = 0;
                ESP_LOGI(TAG, "Demo: Setting emotion to '%s'", emotion_names[emotion_index]);
                face_renderer_set_emotion_str(emotion_names[emotion_index]);
                emotion_index = (emotion_index + 1) % emotion_count;
            }
        } else {
            emotion_cycle_counter = 0;  // Reset counter when connected
        }

        // Periodic status logging (every 30 seconds)
        static int counter = 0;
        if (++counter >= 30) {
            counter = 0;
            ESP_LOGI(TAG, "Status: WiFi=%d, WS=%d, FPS=%.1f, Heap=%lu",
                     wifi_manager_is_connected(),
                     ws_client_is_connected(),
                     face_renderer_get_fps(),
                     esp_get_free_heap_size());
            if (pmu_manager_get_battery_percent() >= 0) {
                ESP_LOGI(TAG, "Battery: %d%%, Charging: %d",
                         pmu_manager_get_battery_percent(),
                         pmu_manager_is_charging());
            }
        }
    }
}

/**
 * @brief WiFi event handler
 */
static void wifi_event_handler(wifi_manager_event_t event, void *ctx)
{
    switch (event) {
        case WIFI_EVENT_GOT_IP:
            ESP_LOGI(TAG, "WiFi connected!");
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
            break;
        case WIFI_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi disconnected!");
            xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
            face_renderer_set_emotion(EMOTION_SAD);
            break;
        default:
            break;
    }
}

/**
 * @brief WebSocket event handler
 */
static void ws_event_handler(ws_client_event_data_t *event, void *ctx)
{
    switch (event->event) {
        case WS_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected!");
            xEventGroupSetBits(s_event_group, WS_CONNECTED_BIT);
            break;

        case WS_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected!");
            xEventGroupClearBits(s_event_group, WS_CONNECTED_BIT);
            face_renderer_set_emotion(EMOTION_CONFUSED);
            break;

        case WS_EVENT_TEXT_DATA:
            // Parse JSON command
            {
                luna_cmd_t cmd;
                if (luna_protocol_parse((const char *)event->data, &cmd) == ESP_OK) {
                    luna_cmd_handler(&cmd, NULL);
                    luna_protocol_free_cmd(&cmd);
                }
            }
            break;

        case WS_EVENT_BINARY_DATA:
            // Audio data from server - feed to playback
            audio_playback_feed(event->data, event->data_len);
            break;

        case WS_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error!");
            break;
    }
}

/**
 * @brief Luna command handler
 */
static void luna_cmd_handler(const luna_cmd_t *cmd, void *ctx)
{
    switch (cmd->type) {
        case LUNA_CMD_EMOTION:
            ESP_LOGI(TAG, "Emotion: %s", cmd->data.emotion.emotion);
            face_renderer_set_emotion_str(cmd->data.emotion.emotion);
            break;

        case LUNA_CMD_GAZE:
            face_renderer_set_gaze(cmd->data.gaze.x, cmd->data.gaze.y);
            break;

        case LUNA_CMD_TEXT:
            ESP_LOGI(TAG, "Text: %s", cmd->data.text.content);
            face_renderer_show_text(cmd->data.text.content,
                                    (font_size_t)cmd->data.text.size,
                                    cmd->data.text.color,
                                    cmd->data.text.bg_color);
            break;

        case LUNA_CMD_TEXT_CLEAR:
            face_renderer_clear_text();
            break;

        case LUNA_CMD_PIXEL_ART:
            // TODO: Convert pixel data format
            break;

        case LUNA_CMD_PIXEL_ART_CLEAR:
            face_renderer_clear_pixel_art();
            break;

        case LUNA_CMD_AUDIO_START:
            ESP_LOGI(TAG, "Audio start");
            audio_capture_start();
            break;

        case LUNA_CMD_AUDIO_STOP:
            ESP_LOGI(TAG, "Audio stop");
            audio_capture_stop();
            break;

        default:
            ESP_LOGW(TAG, "Unknown command: %d", cmd->type);
            break;
    }
}

/**
 * @brief Audio capture callback - send to WebSocket
 */
static void audio_capture_handler(const int16_t *data, size_t len, void *ctx)
{
    if (ws_client_is_connected()) {
        ws_client_send_binary((const uint8_t *)data, len * sizeof(int16_t));
    }
}
