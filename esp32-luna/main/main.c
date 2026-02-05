/*
 * ESP32-Luna Main Application
 *
 * Demo mode with WiFi and WebSocket connectivity.
 * Button cycles through pages, WebSocket receives commands (logging only for now).
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "face_renderer.h"
#include "pmu_manager.h"
#include "luna_motion.h"
#include "emotions.h"
#include "wifi_manager.h"
#include "ws_client.h"
#include "luna_protocol.h"

static const char *TAG = "luna_main";

// Boot button configuration
#define BOOT_BUTTON_GPIO    GPIO_NUM_0
#define BUTTON_DEBOUNCE_MS  200

// Display pages
typedef enum {
    PAGE_FACE,
    PAGE_WEATHER,
    PAGE_CLOCK,
    PAGE_CALENDAR,
    PAGE_SUBWAY,
    PAGE_TIMER,
    PAGE_COUNT
} page_t;

static page_t s_current_page = PAGE_FACE;
static bool s_button_last_state = true;  // true = released (pull-up)
static int64_t s_button_last_press_time = 0;
static emotion_id_t s_pre_distress_emotion = EMOTION_EYES_ONLY;  // Emotion before distress
static bool s_is_distressed = false;  // Currently showing distress
static bool s_ws_initialized = false;  // WebSocket initialized flag
static bool s_wifi_got_ip = false;     // Flag to defer WebSocket init to main loop

/**
 * @brief Initialize boot button GPIO (polling mode)
 */
static void init_boot_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Boot button initialized on GPIO%d", BOOT_BUTTON_GPIO);
}

/**
 * @brief Poll boot button with debounce
 * @return true if button was just pressed
 */
static bool poll_boot_button(void)
{
    bool current_state = gpio_get_level(BOOT_BUTTON_GPIO);
    int64_t now = esp_timer_get_time() / 1000;

    // Detect falling edge (button press)
    if (!current_state && s_button_last_state) {
        if ((now - s_button_last_press_time) > BUTTON_DEBOUNCE_MS) {
            s_button_last_press_time = now;
            s_button_last_state = current_state;
            return true;
        }
    }

    s_button_last_state = current_state;
    return false;
}

/**
 * @brief Convert weather icon string to enum
 */
static weather_icon_t parse_weather_icon(const char *icon_str)
{
    if (!icon_str) return WEATHER_ICON_SUNNY;
    if (strcmp(icon_str, "sunny") == 0) return WEATHER_ICON_SUNNY;
    if (strcmp(icon_str, "cloudy") == 0) return WEATHER_ICON_CLOUDY;
    if (strcmp(icon_str, "rainy") == 0) return WEATHER_ICON_RAINY;
    if (strcmp(icon_str, "snowy") == 0) return WEATHER_ICON_SNOWY;
    if (strcmp(icon_str, "stormy") == 0) return WEATHER_ICON_STORMY;
    if (strcmp(icon_str, "foggy") == 0) return WEATHER_ICON_FOGGY;
    if (strcmp(icon_str, "partly_cloudy") == 0) return WEATHER_ICON_PARTLY_CLOUDY;
    return WEATHER_ICON_SUNNY;
}

/**
 * @brief Handle WebSocket command (parse JSON and dispatch to renderer)
 */
static void handle_ws_command(const char *json_str)
{
    luna_cmd_t cmd;
    if (luna_protocol_parse(json_str, &cmd) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to parse command");
        return;
    }

    switch (cmd.type) {
        case LUNA_CMD_TEXT:
            // Show text as caption overlay at bottom of screen
            face_renderer_show_caption(cmd.data.text.content, cmd.data.text.color);
            break;

        case LUNA_CMD_TEXT_CLEAR:
            face_renderer_hide_caption();
            break;

        case LUNA_CMD_EMOTION:
            face_renderer_set_emotion_str(cmd.data.emotion.emotion);
            ESP_LOGI(TAG, "Set emotion: %s", cmd.data.emotion.emotion);
            break;

        case LUNA_CMD_GAZE:
            face_renderer_set_gaze(cmd.data.gaze.x, cmd.data.gaze.y);
            break;

        case LUNA_CMD_WEATHER:
            face_renderer_show_weather(
                cmd.data.weather.temp,
                parse_weather_icon(cmd.data.weather.icon),
                cmd.data.weather.description);
            ESP_LOGI(TAG, "Show weather: %s %s", cmd.data.weather.temp, cmd.data.weather.icon);
            break;

        case LUNA_CMD_CLOCK:
            face_renderer_show_clock(
                cmd.data.clock.hours,
                cmd.data.clock.minutes,
                cmd.data.clock.is_24h,
                NULL);  // No date string from command yet
            ESP_LOGI(TAG, "Show clock: %02d:%02d", cmd.data.clock.hours, cmd.data.clock.minutes);
            break;

        case LUNA_CMD_TIMER:
            face_renderer_show_timer(
                cmd.data.timer.minutes,
                cmd.data.timer.seconds,
                cmd.data.timer.label,
                cmd.data.timer.is_running);
            ESP_LOGI(TAG, "Show timer: %d:%02d %s",
                     cmd.data.timer.minutes, cmd.data.timer.seconds, cmd.data.timer.label);
            break;

        case LUNA_CMD_SUBWAY:
            face_renderer_show_subway(
                cmd.data.subway.line,
                cmd.data.subway.line_color,
                cmd.data.subway.station,
                cmd.data.subway.direction,
                cmd.data.subway.times,
                cmd.data.subway.num_times);
            ESP_LOGI(TAG, "Show subway: %s to %s", cmd.data.subway.line, cmd.data.subway.station);
            break;

        case LUNA_CMD_CLEAR_DISPLAY:
            face_renderer_clear_display();
            ESP_LOGI(TAG, "Clear display -> face mode");
            break;

        case LUNA_CMD_CALENDAR: {
            // Convert luna_calendar_event_t to calendar_event_t for face_renderer
            calendar_event_t events[3];
            for (int i = 0; i < cmd.data.calendar.num_events && i < 3; i++) {
                strncpy(events[i].time_str, cmd.data.calendar.events[i].time_str, sizeof(events[i].time_str) - 1);
                strncpy(events[i].title, cmd.data.calendar.events[i].title, sizeof(events[i].title) - 1);
                strncpy(events[i].location, cmd.data.calendar.events[i].location, sizeof(events[i].location) - 1);
            }
            face_renderer_show_calendar(events, cmd.data.calendar.num_events);
            ESP_LOGI(TAG, "Show calendar: %d events", cmd.data.calendar.num_events);
            break;
        }

        default:
            ESP_LOGD(TAG, "Unhandled command type: %d", cmd.type);
            break;
    }

    // Free any allocated resources (e.g., pixel_art pixels)
    luna_protocol_free_cmd(&cmd);
}

/**
 * @brief Callback for WebSocket events
 */
static void on_ws_event(ws_client_event_data_t *event, void *ctx)
{
    switch (event->event) {
        case WS_EVENT_CONNECTED:
            ESP_LOGI(TAG, ">>> WebSocket CONNECTED <<<");
            break;
        case WS_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, ">>> WebSocket DISCONNECTED <<<");
            break;
        case WS_EVENT_ERROR:
            ESP_LOGE(TAG, ">>> WebSocket ERROR <<<");
            break;
        case WS_EVENT_TEXT_DATA:
            // Log received JSON (truncate if too long)
            if (event->data_len < 200) {
                ESP_LOGI(TAG, ">>> WS TEXT: %.*s <<<", (int)event->data_len, (const char*)event->data);
            } else {
                ESP_LOGI(TAG, ">>> WS TEXT (%d bytes): %.100s... <<<", (int)event->data_len, (const char*)event->data);
            }
            // Parse and handle command
            handle_ws_command((const char*)event->data);
            break;
        case WS_EVENT_BINARY_DATA:
            ESP_LOGI(TAG, ">>> WS BINARY: %d bytes <<<", (int)event->data_len);
            break;
    }
}

/**
 * @brief Initialize and connect WebSocket
 */
static void init_websocket(void)
{
    if (s_ws_initialized) {
        // Already initialized, just reconnect
        ws_client_connect();
        return;
    }

    ESP_LOGI(TAG, "Initializing WebSocket...");
    ws_client_config_t ws_config = {
        .server_port = CONFIG_LUNA_SERVER_PORT,
        .reconnect_ms = 5000,  // Auto-reconnect every 5 seconds
    };
    strncpy(ws_config.server_ip, CONFIG_LUNA_SERVER_IP, sizeof(ws_config.server_ip) - 1);
    strncpy(ws_config.endpoint, "/luna-esp32", sizeof(ws_config.endpoint) - 1);

    esp_err_t ret = ws_client_init(&ws_config);
    if (ret == ESP_OK) {
        ws_client_set_event_callback(on_ws_event, NULL);
        s_ws_initialized = true;
        ESP_LOGI(TAG, "WebSocket initialized, connecting to ws://%s:%d%s",
                 ws_config.server_ip, ws_config.server_port, ws_config.endpoint);
        ws_client_connect();
    } else {
        ESP_LOGE(TAG, "WebSocket init failed: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief Callback for WiFi events (logging only)
 */
static void on_wifi_event(wifi_manager_event_t event, void *ctx)
{
    switch (event) {
        case WIFI_EVENT_CONNECTED:
            ESP_LOGI(TAG, ">>> WiFi CONNECTED <<<");
            break;
        case WIFI_EVENT_GOT_IP: {
            char ip[16];
            wifi_manager_get_ip(ip);
            ESP_LOGI(TAG, ">>> WiFi GOT IP: %s <<<", ip);
            // Set flag to init WebSocket from main loop (avoid stack overflow in event task)
            s_wifi_got_ip = true;
            break;
        }
        case WIFI_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, ">>> WiFi DISCONNECTED <<<");
            break;
        case WIFI_EVENT_LOST_IP:
            ESP_LOGW(TAG, ">>> WiFi LOST IP <<<");
            break;
    }
}

/**
 * @brief Callback when shake is detected
 */
static void on_shake_detected(float intensity)
{
    ESP_LOGI(TAG, "Shake detected! intensity=%.2f", intensity);

    // Only trigger dizzy if we're on the face page
    if (s_current_page == PAGE_FACE) {
        face_renderer_set_dizzy(true);
    }
}

/**
 * @brief Callback when device orientation changes
 */
static void on_orientation_change(luna_orientation_t orientation)
{
    const char* orient_names[] = {"UPRIGHT", "ON_BACK", "FACE_DOWN", "UPSIDE_DOWN", "OTHER"};
    ESP_LOGI(TAG, "Orientation changed to: %s", orient_names[orientation]);

    // Only react to orientation when on face page
    if (s_current_page != PAGE_FACE) {
        return;
    }

    if (orientation == ORIENTATION_UPSIDE_DOWN) {
        // Device is upside down - trigger distressed effect (wavy mouth, normal eyes)
        if (!s_is_distressed) {
            s_is_distressed = true;
            face_renderer_set_distressed(true);
            ESP_LOGI(TAG, "Luna is distressed! (lying down)");
        }
    } else if (orientation == ORIENTATION_UPRIGHT) {
        // Device is upright again - distressed will auto-clear after 10 seconds
        // But we can also clear it immediately when upright
        if (s_is_distressed) {
            s_is_distressed = false;
            face_renderer_set_distressed(false);
            ESP_LOGI(TAG, "Luna is upright again!");
        }
    }
}

/**
 * @brief Show the current page
 */
static void show_page(page_t page)
{
    static const char* page_names[] = {
        "Face", "Weather", "Clock", "Calendar", "Subway", "Timer"
    };
    ESP_LOGI(TAG, "Showing page: %s", page_names[page]);

    // Note: Each show_* function calls hide_all_screen_elements() internally,
    // so we only call face_renderer_clear_display() for PAGE_FACE

    switch (page) {
        case PAGE_FACE: {
            // clear_display returns to face mode and shows eyes
            face_renderer_clear_display();
            face_renderer_set_emotion(EMOTION_EYES_ONLY);
            // Check current orientation and apply distressed if already lying down
            luna_orientation_t orientation = luna_motion_get_orientation();
            if (orientation == ORIENTATION_UPSIDE_DOWN) {
                s_is_distressed = true;
                face_renderer_set_distressed(true);
                ESP_LOGI(TAG, "Luna distressed on page entry (orientation=%d)", orientation);
            } else {
                s_is_distressed = false;
                face_renderer_set_distressed(false);
            }
            break;
        }

        case PAGE_WEATHER:
            face_renderer_show_weather("72°F", WEATHER_ICON_SUNNY, "Clear skies");
            break;

        case PAGE_CLOCK:
            face_renderer_show_clock(12, 34, false, "Mon, Jan 27");
            break;

        case PAGE_CALENDAR: {
            calendar_event_t events[2] = {0};
            strncpy(events[0].time_str, "In 15 min", sizeof(events[0].time_str) - 1);
            strncpy(events[0].title, "Team Standup", sizeof(events[0].title) - 1);
            strncpy(events[0].location, "Conference Room A", sizeof(events[0].location) - 1);
            strncpy(events[1].time_str, "2:00 PM", sizeof(events[1].time_str) - 1);
            strncpy(events[1].title, "Design Review", sizeof(events[1].title) - 1);
            strncpy(events[1].location, "Zoom", sizeof(events[1].location) - 1);
            face_renderer_show_calendar(events, 2);
            break;
        }

        case PAGE_SUBWAY: {
            // Demo MTA subway times - 1 train to Downtown
            int times[3] = {2, 8, 15};  // Minutes until arrival
            face_renderer_show_subway("1", 0xEE352E, "110 St", "Downtown", times, 3);
            break;
        }

        case PAGE_TIMER:
            // Demo 25-minute Pomodoro timer (not running)
            face_renderer_show_timer(25, 0, "Focus", false);
            break;

        default:
            break;
    }
}

/**
 * @brief Main application entry point
 */
void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== ESP32-Luna Demo Mode ===");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Power Management (AXP2101)
    ESP_LOGI(TAG, "Initializing power management...");
    ret = pmu_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PMU init failed, continuing without power management");
    } else {
        ESP_LOGI(TAG, "Battery: %d%%", pmu_manager_get_battery_percent());
    }

    // Initialize Face Renderer
    ESP_LOGI(TAG, "Initializing face renderer...");
    face_renderer_config_t face_config = {
        .width = 502,   // Landscape
        .height = 410,
        .cat_mode = false,
    };
    ESP_ERROR_CHECK(face_renderer_init(&face_config));
    ESP_ERROR_CHECK(face_renderer_start());
    ESP_LOGI(TAG, "Face renderer started");

    // Initialize motion detection (shake -> dizzy effect)
    ESP_LOGI(TAG, "Initializing motion detection...");
    luna_motion_config_t motion_config = {
        .shake_threshold = 18.0f,       // High threshold (m/s^2) - requires vigorous shake
        .shake_count_trigger = 4,       // 4 direction changes needed
        .shake_window_ms = 500,         // Tighter window - must shake quickly
        .cooldown_ms = 3000,            // 3 second cooldown between shakes
        .on_shake = on_shake_detected,  // Shake callback
        .on_orientation_change = on_orientation_change,  // Orientation callback
    };
    ret = luna_motion_init(&motion_config);
    if (ret == ESP_OK) {
        ret = luna_motion_start();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Motion detection started");
        } else {
            ESP_LOGW(TAG, "Failed to start motion detection: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "Failed to init motion detection: %s", esp_err_to_name(ret));
    }

    // Initialize WiFi ONLY (no WebSocket, no audio) - testing for SPI issues
    ESP_LOGI(TAG, "Initializing WiFi...");
    ret = wifi_manager_init();
    if (ret == ESP_OK) {
        wifi_manager_set_event_callback(on_wifi_event, NULL);  // Log WiFi events
        wifi_manager_config_t wifi_config = {
            .store_in_nvs = false,
        };
        strncpy(wifi_config.ssid, CONFIG_LUNA_WIFI_SSID, sizeof(wifi_config.ssid) - 1);
        strncpy(wifi_config.password, CONFIG_LUNA_WIFI_PASSWORD, sizeof(wifi_config.password) - 1);
        ESP_LOGI(TAG, "Connecting to WiFi: %s", wifi_config.ssid);
        wifi_manager_connect(&wifi_config);
    } else {
        ESP_LOGW(TAG, "WiFi manager init failed: %s", esp_err_to_name(ret));
    }

    // Initialize boot button
    init_boot_button();

    // Show initial page
    show_page(s_current_page);

    ESP_LOGI(TAG, "=== ESP32-Luna Ready ===");
    ESP_LOGI(TAG, "Press boot button to cycle through pages:");
    ESP_LOGI(TAG, "  Face -> Weather -> Clock -> Calendar -> Subway -> Timer");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    // Main loop - simple button polling
    int status_counter = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));  // 20Hz polling

        // Initialize WebSocket when WiFi gets IP (deferred from callback to avoid stack overflow)
        if (s_wifi_got_ip && !s_ws_initialized) {
            init_websocket();
        }

        // Check button
        if (poll_boot_button()) {
            s_current_page = (s_current_page + 1) % PAGE_COUNT;
            show_page(s_current_page);
        }

        // Status logging every 30 seconds
        status_counter++;
        if (status_counter >= 600) {  // 600 * 50ms = 30 seconds
            status_counter = 0;
            ESP_LOGI(TAG, "Status: Page=%d, FPS=%.1f, Heap=%lu",
                     s_current_page,
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
