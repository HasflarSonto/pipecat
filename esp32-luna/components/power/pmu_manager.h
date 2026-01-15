/*
 * PMU Manager for ESP32-Luna
 * AXP2101 power management interface
 * Based on 01_AXP2101 example
 */

#ifndef PMU_MANAGER_H
#define PMU_MANAGER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Charging state
 */
typedef enum {
    PMU_CHARGE_NONE,       // Not charging
    PMU_CHARGE_TRICKLE,    // Trickle charging
    PMU_CHARGE_CC,         // Constant current
    PMU_CHARGE_CV,         // Constant voltage
    PMU_CHARGE_DONE,       // Charging complete
} pmu_charge_state_t;

/**
 * @brief PMU status structure
 */
typedef struct {
    bool vbus_connected;       // USB power connected
    bool battery_present;      // Battery detected
    bool is_charging;          // Currently charging
    pmu_charge_state_t charge_state;
    float battery_voltage;     // Battery voltage (V)
    float vbus_voltage;        // USB voltage (V)
    float system_voltage;      // System voltage (V)
    int battery_percent;       // Battery percentage (0-100)
    float chip_temp;           // PMU chip temperature (C)
} pmu_status_t;

/**
 * @brief PMU event callback
 */
typedef enum {
    PMU_EVENT_VBUS_INSERT,
    PMU_EVENT_VBUS_REMOVE,
    PMU_EVENT_CHARGE_START,
    PMU_EVENT_CHARGE_DONE,
    PMU_EVENT_BATTERY_LOW,
    PMU_EVENT_BUTTON_PRESS,    // PWR button press
    PMU_EVENT_BUTTON_LONG,     // PWR button long press
} pmu_event_t;

typedef void (*pmu_event_cb_t)(pmu_event_t event, void *ctx);

/**
 * @brief Initialize PMU manager
 * @return ESP_OK on success
 */
esp_err_t pmu_manager_init(void);

/**
 * @brief Deinitialize PMU manager
 * @return ESP_OK on success
 */
esp_err_t pmu_manager_deinit(void);

/**
 * @brief Get current PMU status
 * @param status Output status structure
 * @return ESP_OK on success
 */
esp_err_t pmu_manager_get_status(pmu_status_t *status);

/**
 * @brief Get battery percentage
 * @return Battery percentage (0-100), or -1 on error
 */
int pmu_manager_get_battery_percent(void);

/**
 * @brief Check if charging
 * @return true if charging
 */
bool pmu_manager_is_charging(void);

/**
 * @brief Check if USB connected
 * @return true if USB power connected
 */
bool pmu_manager_is_vbus_connected(void);

/**
 * @brief Get chip temperature
 * @return Temperature in Celsius
 */
float pmu_manager_get_temperature(void);

/**
 * @brief Set event callback
 * @param callback Callback function
 * @param ctx User context
 */
void pmu_manager_set_event_callback(pmu_event_cb_t callback, void *ctx);

/**
 * @brief Power off the device
 */
void pmu_manager_power_off(void);

/**
 * @brief Set charging current limit
 * @param ma Current in mA
 * @return ESP_OK on success
 */
esp_err_t pmu_manager_set_charge_current(uint16_t ma);

#ifdef __cplusplus
}
#endif

#endif // PMU_MANAGER_H
