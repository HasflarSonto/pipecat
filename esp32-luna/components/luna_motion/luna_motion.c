/*
 * Luna Motion Detection
 * Detects shake gestures using QMI8658 IMU sensor
 */

#include "luna_motion.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef SIMULATOR
#include "qmi8658.h"
#include "bsp/esp-bsp.h"
#endif

#include <string.h>
#include <math.h>

static const char *TAG = "luna_motion";

// Default configuration
#define DEFAULT_SHAKE_THRESHOLD     15.0f   // m/s^2 - typical value for strong shaking
#define DEFAULT_SHAKE_COUNT         3       // direction changes
#define DEFAULT_SHAKE_WINDOW_MS     500     // ms
#define DEFAULT_COOLDOWN_MS         2000    // ms

// Task configuration
#define MOTION_TASK_STACK_SIZE      (4 * 1024)
#define MOTION_TASK_PRIORITY        2
#define MOTION_SAMPLE_PERIOD_MS     20      // 50 Hz sampling

// Shake detection state
typedef struct {
    float last_accel_magnitude;
    int direction_changes;
    int64_t last_peak_time;
    int64_t shake_start_time;
    int64_t last_shake_time;
    bool is_shaking;
    float shake_intensity;
    bool last_direction_positive;  // true = increasing, false = decreasing
} shake_state_t;

// Module state
static struct {
    luna_motion_config_t config;
    shake_state_t shake;
    TaskHandle_t task;
    bool running;
    bool initialized;
#ifndef SIMULATOR
    qmi8658_dev_t *imu_dev;
#endif
} s_motion = {0};

// Forward declarations
static void motion_task_func(void *pvParameters);
static void process_accel_sample(float ax, float ay, float az);

esp_err_t luna_motion_init(const luna_motion_config_t *config)
{
    if (s_motion.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Apply configuration
    if (config) {
        s_motion.config = *config;
    } else {
        // Defaults
        s_motion.config.shake_threshold = DEFAULT_SHAKE_THRESHOLD;
        s_motion.config.shake_count_trigger = DEFAULT_SHAKE_COUNT;
        s_motion.config.shake_window_ms = DEFAULT_SHAKE_WINDOW_MS;
        s_motion.config.cooldown_ms = DEFAULT_COOLDOWN_MS;
        s_motion.config.on_shake = NULL;
    }

    // Initialize shake state
    memset(&s_motion.shake, 0, sizeof(shake_state_t));

#ifndef SIMULATOR
    // Initialize QMI8658 IMU sensor
    ESP_LOGI(TAG, "Initializing QMI8658 IMU sensor...");

    // Get I2C bus handle from BSP
    i2c_master_bus_handle_t bus_handle = bsp_i2c_get_handle();
    if (bus_handle == NULL) {
        ESP_LOGE(TAG, "Failed to get I2C bus handle");
        return ESP_FAIL;
    }

    // Allocate IMU device
    s_motion.imu_dev = malloc(sizeof(qmi8658_dev_t));
    if (!s_motion.imu_dev) {
        ESP_LOGE(TAG, "Failed to allocate IMU device");
        return ESP_ERR_NO_MEM;
    }

    // Initialize sensor (address is typically 0x6A or 0x6B)
    esp_err_t ret = qmi8658_init(s_motion.imu_dev, bus_handle, QMI8658_ADDRESS_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize QMI8658: %s", esp_err_to_name(ret));
        free(s_motion.imu_dev);
        s_motion.imu_dev = NULL;
        return ret;
    }

    // Configure accelerometer
    qmi8658_set_accel_range(s_motion.imu_dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(s_motion.imu_dev, QMI8658_ACCEL_ODR_500HZ);
    qmi8658_set_accel_unit_mps2(s_motion.imu_dev, true);  // Use m/s^2 units

    ESP_LOGI(TAG, "IMU initialized successfully");
#endif

    s_motion.initialized = true;
    ESP_LOGI(TAG, "Motion detection initialized (threshold=%.1f m/s^2, count=%d)",
             s_motion.config.shake_threshold, s_motion.config.shake_count_trigger);

    return ESP_OK;
}

esp_err_t luna_motion_deinit(void)
{
    if (!s_motion.initialized) {
        return ESP_OK;
    }

    luna_motion_stop();

#ifndef SIMULATOR
    if (s_motion.imu_dev) {
        free(s_motion.imu_dev);
        s_motion.imu_dev = NULL;
    }
#endif

    s_motion.initialized = false;
    return ESP_OK;
}

esp_err_t luna_motion_start(void)
{
    if (!s_motion.initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_motion.running) {
        return ESP_OK;
    }

#ifndef SIMULATOR
    // Create motion detection task
    BaseType_t ret = xTaskCreate(
        motion_task_func,
        "luna_motion",
        MOTION_TASK_STACK_SIZE,
        NULL,
        MOTION_TASK_PRIORITY,
        &s_motion.task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create motion task");
        return ESP_FAIL;
    }
#endif

    s_motion.running = true;
    ESP_LOGI(TAG, "Motion detection started");
    return ESP_OK;
}

esp_err_t luna_motion_stop(void)
{
    if (!s_motion.running) {
        return ESP_OK;
    }

    s_motion.running = false;

#ifndef SIMULATOR
    if (s_motion.task) {
        // Give task time to exit
        vTaskDelay(pdMS_TO_TICKS(50));
        vTaskDelete(s_motion.task);
        s_motion.task = NULL;
    }
#endif

    ESP_LOGI(TAG, "Motion detection stopped");
    return ESP_OK;
}

bool luna_motion_is_shaking(void)
{
    return s_motion.shake.is_shaking;
}

float luna_motion_get_shake_intensity(void)
{
    return s_motion.shake.shake_intensity;
}

void luna_motion_tick(float accel_x, float accel_y, float accel_z)
{
    if (!s_motion.initialized) {
        return;
    }
    process_accel_sample(accel_x, accel_y, accel_z);
}

// Process a single accelerometer sample
static void process_accel_sample(float ax, float ay, float az)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    shake_state_t *shake = &s_motion.shake;

    // Calculate acceleration magnitude (subtract gravity ~9.8 m/s^2)
    float magnitude = sqrtf(ax*ax + ay*ay + az*az);
    float delta = magnitude - shake->last_accel_magnitude;

    // Check if we're in cooldown period
    if (now_ms - shake->last_shake_time < s_motion.config.cooldown_ms) {
        shake->last_accel_magnitude = magnitude;
        // Decay shake intensity during cooldown
        if (shake->is_shaking) {
            shake->shake_intensity *= 0.95f;
            if (shake->shake_intensity < 0.1f) {
                shake->is_shaking = false;
                shake->shake_intensity = 0.0f;
                ESP_LOGI(TAG, "Shake ended (cooldown)");
            }
        }
        return;
    }

    // Detect direction changes (peaks in acceleration)
    bool current_direction = delta > 0;

    // Only count direction changes when magnitude is above threshold
    if (fabsf(delta) > s_motion.config.shake_threshold * 0.3f) {
        if (current_direction != shake->last_direction_positive) {
            // Direction changed
            if (magnitude > s_motion.config.shake_threshold) {
                // Check if within shake window
                if (shake->direction_changes == 0) {
                    shake->shake_start_time = now_ms;
                }

                int64_t elapsed = now_ms - shake->shake_start_time;
                if (elapsed < s_motion.config.shake_window_ms) {
                    shake->direction_changes++;
                    shake->last_peak_time = now_ms;

                    ESP_LOGD(TAG, "Direction change %d (mag=%.1f, delta=%.1f)",
                             shake->direction_changes, magnitude, delta);

                    // Check if shake detected
                    if (shake->direction_changes >= s_motion.config.shake_count_trigger) {
                        if (!shake->is_shaking) {
                            shake->is_shaking = true;
                            shake->last_shake_time = now_ms;

                            // Calculate intensity based on magnitude
                            float max_magnitude = s_motion.config.shake_threshold * 3.0f;
                            shake->shake_intensity = magnitude / max_magnitude;
                            if (shake->shake_intensity > 1.0f) {
                                shake->shake_intensity = 1.0f;
                            }

                            ESP_LOGI(TAG, "SHAKE detected! intensity=%.2f", shake->shake_intensity);

                            // Call callback
                            if (s_motion.config.on_shake) {
                                s_motion.config.on_shake(shake->shake_intensity);
                            }
                        }
                        shake->direction_changes = 0;
                    }
                } else {
                    // Window expired, reset
                    shake->direction_changes = 1;
                    shake->shake_start_time = now_ms;
                }
            }
            shake->last_direction_positive = current_direction;
        }
    }

    // Reset if no activity for too long
    if (now_ms - shake->last_peak_time > s_motion.config.shake_window_ms * 2) {
        shake->direction_changes = 0;
    }

    // Decay shake intensity when not actively shaking
    if (shake->is_shaking && now_ms - shake->last_shake_time > 500) {
        shake->shake_intensity *= 0.9f;
        if (shake->shake_intensity < 0.1f) {
            shake->is_shaking = false;
            shake->shake_intensity = 0.0f;
            ESP_LOGI(TAG, "Shake ended");
        }
    }

    shake->last_accel_magnitude = magnitude;
}

#ifndef SIMULATOR
static void motion_task_func(void *pvParameters)
{
    (void)pvParameters;
    qmi8658_data_t data;

    ESP_LOGI(TAG, "Motion task started");

    while (s_motion.running) {
        // Check if data is ready
        bool ready = false;
        esp_err_t ret = qmi8658_is_data_ready(s_motion.imu_dev, &ready);

        if (ret == ESP_OK && ready) {
            ret = qmi8658_read_sensor_data(s_motion.imu_dev, &data);
            if (ret == ESP_OK) {
                // Process the sample
                process_accel_sample(data.accelX, data.accelY, data.accelZ);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MOTION_SAMPLE_PERIOD_MS));
    }

    ESP_LOGI(TAG, "Motion task ended");
    vTaskDelete(NULL);
}
#endif
