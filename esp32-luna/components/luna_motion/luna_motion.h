/*
 * Luna Motion Detection
 * Detects shake gestures using QMI8658 IMU sensor
 */

#ifndef LUNA_MOTION_H
#define LUNA_MOTION_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback when shake is detected
 * @param intensity Shake intensity (0.0 - 1.0)
 */
typedef void (*luna_motion_shake_cb_t)(float intensity);

/**
 * @brief Motion detection configuration
 */
typedef struct {
    float shake_threshold;      // Acceleration threshold for shake detection (m/s^2), default 15.0
    int shake_count_trigger;    // Number of direction changes to trigger shake, default 3
    int shake_window_ms;        // Time window for shake detection (ms), default 500
    int cooldown_ms;            // Cooldown after shake detected (ms), default 2000
    luna_motion_shake_cb_t on_shake;  // Callback when shake detected
} luna_motion_config_t;

/**
 * @brief Initialize motion detection
 * @param config Configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t luna_motion_init(const luna_motion_config_t *config);

/**
 * @brief Deinitialize motion detection
 * @return ESP_OK on success
 */
esp_err_t luna_motion_deinit(void);

/**
 * @brief Start motion detection task
 * @return ESP_OK on success
 */
esp_err_t luna_motion_start(void);

/**
 * @brief Stop motion detection task
 * @return ESP_OK on success
 */
esp_err_t luna_motion_stop(void);

/**
 * @brief Check if device is currently being shaken
 * @return true if shaking detected
 */
bool luna_motion_is_shaking(void);

/**
 * @brief Get shake intensity (0.0 - 1.0)
 * @return Current shake intensity, 0 if not shaking
 */
float luna_motion_get_shake_intensity(void);

/**
 * @brief Manual tick for testing/simulator (call from main loop)
 * @param accel_x X acceleration (m/s^2)
 * @param accel_y Y acceleration (m/s^2)
 * @param accel_z Z acceleration (m/s^2)
 */
void luna_motion_tick(float accel_x, float accel_y, float accel_z);

#ifdef __cplusplus
}
#endif

#endif // LUNA_MOTION_H
