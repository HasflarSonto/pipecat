/**
 * ESP32 Board Support Package Stub for Simulator
 * Provides display initialization interface
 */

#ifndef ESP_BSP_H
#define ESP_BSP_H

#include "esp_err.h"
#include "lvgl.h"

/**
 * Display configuration (not used in simulator)
 */
typedef struct {
    int dummy;
} bsp_display_cfg_t;

/**
 * In simulator, LVGL display is initialized elsewhere
 * This stub returns the display pointer set by the SDL driver
 */
extern lv_display_t* bsp_display_start(void);

/**
 * Get LVGL display lock mutex
 * In simulator, we use our own locking
 */
extern void* bsp_display_get_lock(void);

/**
 * Lock display for LVGL operations
 */
static inline bool bsp_display_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    /* Locking handled by simulator main loop */
    return true;
}

/**
 * Unlock display
 */
static inline void bsp_display_unlock(void)
{
    /* Unlocking handled by simulator main loop */
}

/**
 * Rotate display - no-op in simulator (already landscape)
 */
static inline void bsp_display_rotate(lv_display_t* disp, lv_display_rotation_t rotation)
{
    (void)disp;
    (void)rotation;
    /* No rotation needed - SDL display is already in correct orientation */
}

/**
 * Turn on backlight - no-op in simulator
 */
static inline void bsp_display_backlight_on(void)
{
    /* No backlight in simulator */
}

/**
 * Turn off backlight - no-op in simulator
 */
static inline void bsp_display_backlight_off(void)
{
    /* No backlight in simulator */
}

/**
 * Get input device - returns NULL in simulator (we handle input separately)
 */
static inline void* bsp_display_get_input_dev(void)
{
    return NULL;
}

#endif /* ESP_BSP_H */
