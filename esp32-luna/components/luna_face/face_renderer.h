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
} display_mode_t;

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
 * @brief Get current display mode
 * @return Current mode
 */
display_mode_t face_renderer_get_mode(void);

/**
 * @brief Get current FPS (for debugging)
 * @return Actual frames per second
 */
float face_renderer_get_fps(void);

#ifdef __cplusplus
}
#endif

#endif // LUNA_FACE_RENDERER_H
