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
    PAGE_ANIMATION,
    PAGE_COUNT
} page_t;

// Animation cycling state
static int s_current_animation = 0;

static page_t s_current_page = PAGE_FACE;
static bool s_button_last_state = true;  // true = released (pull-up)
static int64_t s_button_last_press_time = 0;

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
 * @brief Show the current page
 */
static void show_page(page_t page)
{
    static const char* page_names[] = {
        "Face", "Weather", "Clock", "Calendar", "Subway", "Timer", "Animation"
    };
    ESP_LOGI(TAG, "Showing page: %s", page_names[page]);

    // Note: Each show_* function calls hide_all_screen_elements() internally,
    // so we only call face_renderer_clear_display() for PAGE_FACE

    switch (page) {
        case PAGE_FACE:
            // clear_display returns to face mode and shows eyes
            face_renderer_clear_display();
            face_renderer_set_emotion(EMOTION_EYES_ONLY);
            break;

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

        case PAGE_ANIMATION: {
            // Cycle through animation types
            static const animation_type_t animations[] = {
                ANIMATION_RAIN,
                ANIMATION_SNOW,
                ANIMATION_STARS,
                ANIMATION_MATRIX
            };
            static const char* anim_names[] = {"Rain", "Snow", "Stars", "Matrix"};
            int num_animations = sizeof(animations) / sizeof(animations[0]);

            ESP_LOGI(TAG, "Animation: %s", anim_names[s_current_animation]);
            face_renderer_show_animation(animations[s_current_animation]);
            s_current_animation = (s_current_animation + 1) % num_animations;
            break;
        }

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

    // Initialize boot button
    init_boot_button();

    // Show initial page
    show_page(s_current_page);

    ESP_LOGI(TAG, "=== ESP32-Luna Ready ===");
    ESP_LOGI(TAG, "Press boot button to cycle through pages:");
    ESP_LOGI(TAG, "  Face -> Weather -> Clock -> Calendar -> Subway -> Timer -> Animation");
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
