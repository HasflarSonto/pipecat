/*
 * ESP32-Luna Main Application
 * Voice assistant frontend for Pipecat backend
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
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

    // Connect to WiFi (with retry, don't crash on failure)
    ESP_LOGI(TAG, "Connecting to WiFi: %s", WIFI_SSID);
    wifi_manager_config_t wifi_config = {
        .store_in_nvs = true,
    };
    strncpy(wifi_config.ssid, WIFI_SSID, sizeof(wifi_config.ssid) - 1);
    strncpy(wifi_config.password, WIFI_PASSWORD, sizeof(wifi_config.password) - 1);

    esp_err_t wifi_err = wifi_manager_connect(&wifi_config);
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect returned %d, waiting for event-based connection...", wifi_err);
        // Don't crash - the event handler will still try to connect
    }

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

    // Wait briefly for WebSocket connection (audio will start in event handler when connected)
    bits = xEventGroupWaitBits(s_event_group,
                               WS_CONNECTED_BIT,
                               pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(5000));
    if (!(bits & WS_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WebSocket not yet connected, will start audio when connection established");
        face_renderer_set_emotion(EMOTION_NEUTRAL);
    }
    // Note: Audio capture/playback now starts in ws_event_handler on WS_EVENT_CONNECTED

    ESP_LOGI(TAG, "=== ESP32-Luna Ready ===");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    // Emotion names for cycling demo - all emotions
    static const char *emotion_names[] = {
        "neutral", "happy", "sad", "angry", "surprised", "thinking", "confused", "excited", "cat"
    };
    static const int emotion_count = sizeof(emotion_names) / sizeof(emotion_names[0]);
    int emotion_index = 0;
    int demo_counter = 0;
    int disconnect_grace_counter = 0;  // Grace period before entering demo mode
    #define DISCONNECT_GRACE_PERIOD 10  // 10 seconds before demo mode activates

    // Demo display cycle state
    typedef enum {
        DEMO_FACE,
        DEMO_CLOCK,
        DEMO_WEATHER,
        DEMO_TIMER,
        DEMO_ANIMATION,
    } demo_state_t;
    demo_state_t demo_state = DEMO_FACE;
    int demo_display_counter = 0;  // Counter for each display mode
    int timer_seconds = 25 * 60;   // 25 minute pomodoro timer

    // Main loop - just keep running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Demo mode when not connected to server (with grace period)
        if (!ws_client_is_connected()) {
            disconnect_grace_counter++;
            // Only enter demo mode after sustained disconnect (10 seconds)
            if (disconnect_grace_counter >= DISCONNECT_GRACE_PERIOD) {
                demo_display_counter++;

                switch (demo_state) {
                    case DEMO_FACE:
                        // Cycle through emotions every 3 seconds
                        if (demo_display_counter % 3 == 0) {
                            ESP_LOGI(TAG, "Demo: Face emotion '%s'", emotion_names[emotion_index]);
                            face_renderer_set_emotion_str(emotion_names[emotion_index]);
                            emotion_index = (emotion_index + 1) % emotion_count;
                        }
                        // Switch to clock after showing all emotions
                        if (demo_display_counter >= emotion_count * 3) {
                            demo_state = DEMO_CLOCK;
                            demo_display_counter = 0;
                            ESP_LOGI(TAG, "Demo: Switching to CLOCK");
                        }
                        break;

                    case DEMO_CLOCK: {
                        // Show real time from system clock
                        time_t now;
                        struct tm timeinfo;
                        time(&now);
                        localtime_r(&now, &timeinfo);
                        face_renderer_show_clock(timeinfo.tm_hour, timeinfo.tm_min, false);
                        // Switch to weather after 10 seconds
                        if (demo_display_counter >= 10) {
                            demo_state = DEMO_WEATHER;
                            demo_display_counter = 0;
                            ESP_LOGI(TAG, "Demo: Switching to WEATHER");
                        }
                        break;
                    }

                    case DEMO_WEATHER: {
                        // Cycle through weather conditions
                        static const char* weather_temps[] = {"72°F", "45°F", "88°F", "32°F", "65°F"};
                        static const weather_icon_t weather_icons[] = {
                            WEATHER_ICON_SUNNY, WEATHER_ICON_RAINY, WEATHER_ICON_CLOUDY,
                            WEATHER_ICON_SNOWY, WEATHER_ICON_PARTLY_CLOUDY
                        };
                        static const char* weather_descs[] = {"Sunny", "Rainy", "Cloudy", "Snowy", "Partly Cloudy"};
                        int weather_idx = (demo_display_counter / 3) % 5;
                        face_renderer_show_weather(weather_temps[weather_idx], weather_icons[weather_idx], weather_descs[weather_idx]);
                        // Switch to timer after showing all weather
                        if (demo_display_counter >= 15) {
                            demo_state = DEMO_TIMER;
                            demo_display_counter = 0;
                            timer_seconds = 25 * 60;  // Reset to 25 minutes
                            ESP_LOGI(TAG, "Demo: Switching to TIMER");
                        }
                        break;
                    }

                    case DEMO_TIMER: {
                        // Countdown timer (fast for demo - every second counts down 10 seconds)
                        timer_seconds -= 10;
                        if (timer_seconds < 0) timer_seconds = 0;
                        int mins = timer_seconds / 60;
                        int secs = timer_seconds % 60;
                        face_renderer_show_timer(mins, secs, "Focus", timer_seconds > 0);
                        // Switch to animation after timer runs out or 15 seconds
                        if (demo_display_counter >= 15 || timer_seconds <= 0) {
                            demo_state = DEMO_ANIMATION;
                            demo_display_counter = 0;
                            ESP_LOGI(TAG, "Demo: Switching to ANIMATION");
                        }
                        break;
                    }

                    case DEMO_ANIMATION: {
                        // Cycle through animations
                        static const animation_type_t anims[] = {
                            ANIMATION_RAIN, ANIMATION_SNOW, ANIMATION_STARS, ANIMATION_MATRIX
                        };
                        int anim_idx = (demo_display_counter / 4) % 4;
                        face_renderer_show_animation(anims[anim_idx]);
                        // Switch back to face after showing all animations
                        if (demo_display_counter >= 16) {
                            demo_state = DEMO_FACE;
                            demo_display_counter = 0;
                            face_renderer_clear_display();
                            ESP_LOGI(TAG, "Demo: Switching to FACE");
                        }
                        break;
                    }
                }
            }
        } else {
            disconnect_grace_counter = 0;  // Reset grace period on connection
            demo_display_counter = 0;  // Reset counter when connected
            demo_state = DEMO_FACE;  // Reset to face mode
            // Make sure we're showing the face when connected
            if (face_renderer_get_mode() != DISPLAY_MODE_FACE) {
                face_renderer_clear_display();
            }
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
            // Start audio streaming when connected (handles late connections)
            ESP_LOGI(TAG, "Starting audio capture...");
            audio_capture_start();
            audio_playback_start();
            face_renderer_set_emotion(EMOTION_HAPPY);
            break;

        case WS_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected!");
            xEventGroupClearBits(s_event_group, WS_CONNECTED_BIT);
            // Stop audio when disconnected
            audio_capture_stop();
            audio_playback_stop();
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

        case LUNA_CMD_WEATHER: {
            ESP_LOGI(TAG, "Weather: %s %s", cmd->data.weather.temp, cmd->data.weather.icon);
            // Map icon string to enum
            weather_icon_t icon = WEATHER_ICON_SUNNY;
            if (strcmp(cmd->data.weather.icon, "cloudy") == 0) icon = WEATHER_ICON_CLOUDY;
            else if (strcmp(cmd->data.weather.icon, "rainy") == 0) icon = WEATHER_ICON_RAINY;
            else if (strcmp(cmd->data.weather.icon, "snowy") == 0) icon = WEATHER_ICON_SNOWY;
            else if (strcmp(cmd->data.weather.icon, "stormy") == 0) icon = WEATHER_ICON_STORMY;
            else if (strcmp(cmd->data.weather.icon, "foggy") == 0) icon = WEATHER_ICON_FOGGY;
            else if (strcmp(cmd->data.weather.icon, "partly_cloudy") == 0) icon = WEATHER_ICON_PARTLY_CLOUDY;
            face_renderer_show_weather(cmd->data.weather.temp, icon, cmd->data.weather.description);
            break;
        }

        case LUNA_CMD_TIMER:
            ESP_LOGI(TAG, "Timer: %d:%02d %s", cmd->data.timer.minutes, cmd->data.timer.seconds, cmd->data.timer.label);
            face_renderer_show_timer(cmd->data.timer.minutes, cmd->data.timer.seconds,
                                     cmd->data.timer.label, cmd->data.timer.is_running);
            break;

        case LUNA_CMD_CLOCK:
            ESP_LOGI(TAG, "Clock: %02d:%02d", cmd->data.clock.hours, cmd->data.clock.minutes);
            face_renderer_show_clock(cmd->data.clock.hours, cmd->data.clock.minutes, cmd->data.clock.is_24h);
            break;

        case LUNA_CMD_ANIMATION: {
            ESP_LOGI(TAG, "Animation: %s", cmd->data.animation.type);
            // Map type string to enum
            animation_type_t anim = ANIMATION_RAIN;
            if (strcmp(cmd->data.animation.type, "snow") == 0) anim = ANIMATION_SNOW;
            else if (strcmp(cmd->data.animation.type, "stars") == 0) anim = ANIMATION_STARS;
            else if (strcmp(cmd->data.animation.type, "matrix") == 0) anim = ANIMATION_MATRIX;
            face_renderer_show_animation(anim);
            break;
        }

        case LUNA_CMD_CLEAR_DISPLAY:
            ESP_LOGI(TAG, "Clear display");
            face_renderer_clear_display();
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
