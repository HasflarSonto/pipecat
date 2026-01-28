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
    PAGE_COUNT
} page_t;

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
    static const char* page_names[] = {"Face", "Weather", "Clock", "Calendar"};
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
            face_renderer_show_clock(12, 34, false, NULL);
            break;

        case PAGE_CALENDAR: {
            calendar_event_t event = {0};
            strncpy(event.time_str, "In 15 min", sizeof(event.time_str) - 1);
            strncpy(event.title, "Team Standup", sizeof(event.title) - 1);
            strncpy(event.location, "Conference Room A", sizeof(event.location) - 1);
            face_renderer_show_calendar(&event, 1);
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
    ESP_LOGI(TAG, "Press boot button to cycle: Face -> Weather -> Clock -> Calendar");
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
