/*
 * ESP32-Luna Main Application (Simplified Demo Mode)
 *
 * This is a stripped-down version that boots directly into demo mode.
 * No WiFi, no WebSocket, no audio - just face display with button cycling.
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
    const char* orient_names[] = {"UPRIGHT", "ON_BACK", "FACE_DOWN", "OTHER"};
    ESP_LOGI(TAG, "Orientation changed to: %s", orient_names[orientation]);

    // Only react to orientation when on face page
    if (s_current_page != PAGE_FACE) {
        return;
    }

    if (orientation == ORIENTATION_ON_BACK || orientation == ORIENTATION_FACE_DOWN) {
        // Device is not upright - trigger distressed effect (wavy mouth, normal eyes)
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
            if (orientation == ORIENTATION_ON_BACK || orientation == ORIENTATION_FACE_DOWN) {
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
