/*
 * Luna Face Renderer for ESP32
 * Main rendering engine using LVGL
 * Ported from luna_face_renderer.py
 */

#ifndef LUNA_FACE_RENDERER_H
#define LUNA_FACE_RENDERER_H

#include "esp_err.h"
#include "emotions.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Display mode
 */
typedef enum {
    DISPLAY_MODE_FACE,        // Animated face (default)
    DISPLAY_MODE_TEXT,        // Text display
    DISPLAY_MODE_PIXEL_ART,   // Pixel art grid
    DISPLAY_MODE_WEATHER,     // Weather display (temp + icon)
    DISPLAY_MODE_TIMER,       // Pomodoro/countdown timer
    DISPLAY_MODE_CLOCK,       // Time display
    DISPLAY_MODE_ANIMATION,   // Custom animations (rain, snow, etc.)
    DISPLAY_MODE_SUBWAY,      // MTA subway arrival times
    DISPLAY_MODE_CALENDAR,    // Calendar events (Apple Watch style cards)
} display_mode_t;

/**
 * @brief Calendar event structure (for Apple Watch style display)
 */
typedef struct {
    char time_str[32];        // Time range (e.g., "10:00-11:00 AM")
    char title[64];           // Event title
    char location[64];        // Location (optional, can be empty)
} calendar_event_t;

/**
 * @brief Weather icon type
 */
typedef enum {
    WEATHER_ICON_SUNNY,
    WEATHER_ICON_CLOUDY,
    WEATHER_ICON_RAINY,
    WEATHER_ICON_SNOWY,
    WEATHER_ICON_STORMY,
    WEATHER_ICON_FOGGY,
    WEATHER_ICON_PARTLY_CLOUDY,
} weather_icon_t;

/**
 * @brief Animation type
 */
typedef enum {
    ANIMATION_RAIN,
    ANIMATION_SNOW,
    ANIMATION_STARS,
    ANIMATION_MATRIX,
} animation_type_t;

/**
 * @brief Font size for text mode
 */
typedef enum {
    FONT_SIZE_SMALL = 0,   // ~34px (scaled from 20px)
    FONT_SIZE_MEDIUM,      // ~54px (scaled from 32px)
    FONT_SIZE_LARGE,       // ~75px (scaled from 44px)
    FONT_SIZE_XLARGE,      // ~109px (scaled from 64px)
} font_size_t;

/**
 * @brief Renderer configuration
 */
typedef struct {
    uint16_t width;           // Display width (default 410)
    uint16_t height;          // Display height (default 502)
    uint8_t fps;              // Target FPS (default 30)
    bool cat_mode;            // Start in cat mode
} face_renderer_config_t;

/**
 * @brief Initialize face renderer
 * @param config Configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t face_renderer_init(const face_renderer_config_t *config);

/**
 * @brief Deinitialize face renderer
 * @return ESP_OK on success
 */
esp_err_t face_renderer_deinit(void);

/**
 * @brief Start render task
 * @return ESP_OK on success
 */
esp_err_t face_renderer_start(void);

/**
 * @brief Stop render task
 * @return ESP_OK on success
 */
esp_err_t face_renderer_stop(void);

/**
 * @brief Set current emotion
 * @param emotion Emotion identifier
 */
void face_renderer_set_emotion(emotion_id_t emotion);

/**
 * @brief Set emotion by string name
 * @param name Emotion name (e.g., "happy")
 */
void face_renderer_set_emotion_str(const char *name);

/**
 * @brief Get current emotion
 * @return Current emotion ID
 */
emotion_id_t face_renderer_get_emotion(void);

/**
 * @brief Set gaze target (eye tracking)
 * @param x Horizontal gaze (0.0 = left, 0.5 = center, 1.0 = right)
 * @param y Vertical gaze (0.0 = up, 0.5 = center, 1.0 = down)
 */
void face_renderer_set_gaze(float x, float y);

/**
 * @brief Get current gaze position
 * @param x Output horizontal gaze
 * @param y Output vertical gaze
 */
void face_renderer_get_gaze(float *x, float *y);

/**
 * @brief Set cat mode
 * @param enabled true for cat mode
 */
void face_renderer_set_cat_mode(bool enabled);

/**
 * @brief Check if cat mode is enabled
 * @return true if cat mode
 */
bool face_renderer_is_cat_mode(void);

/**
 * @brief Force a blink animation
 */
void face_renderer_blink(void);

/**
 * @brief Switch to text display mode
 * @param text Text to display
 * @param size Font size
 * @param color Text color (RGB888)
 * @param bg_color Background color (RGB888)
 */
void face_renderer_show_text(const char *text, font_size_t size,
                              uint32_t color, uint32_t bg_color);

/**
 * @brief Clear text and return to face mode
 */
void face_renderer_clear_text(void);

/**
 * @brief Switch to pixel art mode
 * @param pixels Array of pixel data (x, y, color)
 * @param count Number of pixels
 * @param bg_color Background color (RGB888)
 *
 * Pixel format: x (0-11), y (0-15), color (RGB888)
 * Grid is 12x16, auto-centered on display
 */
void face_renderer_show_pixel_art(const uint32_t *pixels, size_t count,
                                   uint32_t bg_color);

/**
 * @brief Clear pixel art and return to face mode
 */
void face_renderer_clear_pixel_art(void);

/**
 * @brief Show weather display
 * @param temp Temperature string (e.g., "72°F")
 * @param icon Weather icon type
 * @param description Weather description (e.g., "Sunny")
 */
void face_renderer_show_weather(const char *temp, weather_icon_t icon,
                                 const char *description);

/**
 * @brief Show timer/pomodoro display
 * @param minutes Total minutes
 * @param seconds Current seconds remaining
 * @param label Timer label (e.g., "Focus", "Break")
 * @param is_running True if timer is active
 */
void face_renderer_show_timer(int minutes, int seconds, const char *label,
                               bool is_running);

/**
 * @brief Start the timer countdown
 */
void face_renderer_timer_start(void);

/**
 * @brief Pause the timer countdown
 */
void face_renderer_timer_pause(void);

/**
 * @brief Reset the timer to specified minutes
 * @param minutes Number of minutes to set
 */
void face_renderer_timer_reset(int minutes);

/**
 * @brief Check if timer is running
 * @return true if timer is counting down
 */
bool face_renderer_timer_is_running(void);

/**
 * @brief Show clock display (Apple Watch style)
 * @param hours Hour (0-23)
 * @param minutes Minute (0-59)
 * @param is_24h True for 24-hour format
 * @param date_str Date string (e.g., "TUE JAN 21") or NULL for no date
 */
void face_renderer_show_clock(int hours, int minutes, bool is_24h, const char *date_str);

/**
 * @brief Show subway arrival display
 * @param line Train line (e.g., "1", "A", "N")
 * @param line_color Line color (RGB888, e.g., 0xEE352E for red)
 * @param station Station name (e.g., "110 St")
 * @param direction Direction arrow: "↑" for uptown/north, "↓" for downtown/south
 * @param times Array of arrival times in minutes (up to 3)
 * @param num_times Number of times in array (1-3)
 */
void face_renderer_show_subway(const char *line, uint32_t line_color,
                                const char *station, const char *direction,
                                const int *times, int num_times);

/**
 * @brief Show calendar display (Apple Watch style cards)
 * @param events Array of calendar events
 * @param num_events Number of events (1-3)
 */
void face_renderer_show_calendar(const calendar_event_t *events, int num_events);

/**
 * @brief Show animation
 * @param type Animation type
 */
void face_renderer_show_animation(animation_type_t type);

/**
 * @brief Clear any special display and return to face mode
 */
void face_renderer_clear_display(void);

/**
 * @brief Get current display mode
 * @return Current mode
 */
display_mode_t face_renderer_get_mode(void);

/**
 * @brief Get current FPS (for debugging)
 * @return Actual frames per second
 */
float face_renderer_get_fps(void);

/**
 * @brief Manual tick for simulator (call from main loop instead of using render task)
 * @param delta_time_ms Time since last tick in milliseconds
 *
 * In the simulator, LVGL is not thread-safe so we can't use a background render task.
 * Instead, call this function from the main loop after lv_timer_handler().
 */
void face_renderer_tick(uint32_t delta_time_ms);

/**
 * @brief Set eye wink state (for eye poke interaction)
 * @param left_wink Left eye wink amount (0.0 = open, 1.0 = fully closed)
 * @param right_wink Right eye wink amount (0.0 = open, 1.0 = fully closed)
 */
void face_renderer_set_wink(float left_wink, float right_wink);

/**
 * @brief Poke an eye (causes that eye to close briefly)
 * @param which_eye 0 = left eye, 1 = right eye
 */
void face_renderer_poke_eye(int which_eye);

/**
 * @brief Check if a point is within an eye region
 * @param x X coordinate (screen space)
 * @param y Y coordinate (screen space)
 * @return -1 if not on eye, 0 if on left eye, 1 if on right eye
 */
int face_renderer_hit_test_eye(int x, int y);

/**
 * @brief Set dizzy state (from shake)
 * @param dizzy true to enable dizzy mode
 */
void face_renderer_set_dizzy(bool dizzy);

/**
 * @brief Check if currently dizzy
 * @return true if dizzy
 */
bool face_renderer_is_dizzy(void);

#ifdef __cplusplus
}
#endif

#endif // LUNA_FACE_RENDERER_H
