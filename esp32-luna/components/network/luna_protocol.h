/*
 * Luna Protocol for ESP32-Luna
 * Parses JSON commands from server and formats responses
 */

#ifndef LUNA_PROTOCOL_H
#define LUNA_PROTOCOL_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Command types from server
 */
typedef enum {
    LUNA_CMD_UNKNOWN = 0,
    LUNA_CMD_EMOTION,          // {"cmd": "emotion", "value": "happy"}
    LUNA_CMD_GAZE,             // {"cmd": "gaze", "x": 0.5, "y": 0.5}
    LUNA_CMD_TEXT,             // {"cmd": "text", "content": "Hello", "size": "large"}
    LUNA_CMD_TEXT_CLEAR,       // {"cmd": "text_clear"}
    LUNA_CMD_PIXEL_ART,        // {"cmd": "pixel_art", "pixels": [...], "bg": "#1E1E28"}
    LUNA_CMD_PIXEL_ART_CLEAR,  // {"cmd": "pixel_art_clear"}
    LUNA_CMD_AUDIO_START,      // {"cmd": "audio_start"}
    LUNA_CMD_AUDIO_STOP,       // {"cmd": "audio_stop"}
} luna_cmd_type_t;

/**
 * @brief Font size for text display
 */
typedef enum {
    LUNA_FONT_SMALL = 0,
    LUNA_FONT_MEDIUM,
    LUNA_FONT_LARGE,
    LUNA_FONT_XLARGE,
} luna_font_size_t;

/**
 * @brief Emotion command data
 */
typedef struct {
    char emotion[16];  // neutral, happy, sad, angry, surprised, thinking, confused, excited, cat
} luna_cmd_emotion_t;

/**
 * @brief Gaze command data
 */
typedef struct {
    float x;  // 0.0 - 1.0, 0.5 = center
    float y;  // 0.0 - 1.0, 0.5 = center
} luna_cmd_gaze_t;

/**
 * @brief Text command data
 */
typedef struct {
    char content[256];
    luna_font_size_t size;
    uint32_t color;      // RGB888 packed
    uint32_t bg_color;   // RGB888 packed
} luna_cmd_text_t;

/**
 * @brief Single pixel for pixel art
 */
typedef struct {
    uint8_t x;
    uint8_t y;
    uint32_t color;  // RGB888 packed
} luna_pixel_t;

/**
 * @brief Pixel art command data
 */
typedef struct {
    luna_pixel_t *pixels;
    size_t pixel_count;
    uint32_t bg_color;
} luna_cmd_pixel_art_t;

/**
 * @brief Parsed command structure
 */
typedef struct {
    luna_cmd_type_t type;
    union {
        luna_cmd_emotion_t emotion;
        luna_cmd_gaze_t gaze;
        luna_cmd_text_t text;
        luna_cmd_pixel_art_t pixel_art;
    } data;
} luna_cmd_t;

/**
 * @brief Command handler callback
 */
typedef void (*luna_cmd_handler_t)(const luna_cmd_t *cmd, void *ctx);

/**
 * @brief Initialize protocol parser
 * @return ESP_OK on success
 */
esp_err_t luna_protocol_init(void);

/**
 * @brief Deinitialize protocol parser
 * @return ESP_OK on success
 */
esp_err_t luna_protocol_deinit(void);

/**
 * @brief Parse JSON command string
 * @param json JSON string
 * @param cmd Output command structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if invalid JSON
 */
esp_err_t luna_protocol_parse(const char *json, luna_cmd_t *cmd);

/**
 * @brief Free command resources (e.g., pixel_art pixels array)
 * @param cmd Command to free
 */
void luna_protocol_free_cmd(luna_cmd_t *cmd);

/**
 * @brief Set command handler
 * @param handler Handler function
 * @param ctx User context
 */
void luna_protocol_set_handler(luna_cmd_handler_t handler, void *ctx);

/**
 * @brief Parse hex color string to RGB888
 * @param hex Color string (e.g., "#FF0000" or "FF0000")
 * @param color Output RGB888 value
 * @return ESP_OK on success
 */
esp_err_t luna_protocol_parse_color(const char *hex, uint32_t *color);

#ifdef __cplusplus
}
#endif

#endif // LUNA_PROTOCOL_H
