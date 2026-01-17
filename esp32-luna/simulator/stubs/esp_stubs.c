/**
 * ESP-IDF Stub Implementations for Simulator
 */

#include "esp_err.h"
#include "bsp/esp-bsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* External display pointer set by SDL driver */
extern lv_display_t* g_sim_display;

/**
 * Get the LVGL display (set by SDL driver)
 */
lv_display_t* bsp_display_start(void)
{
    return g_sim_display;
}

/**
 * Get display lock (not used in simulator)
 */
void* bsp_display_get_lock(void)
{
    return NULL;
}

/**
 * Initialize random seed
 */
__attribute__((constructor))
static void init_random(void)
{
    srand((unsigned int)time(NULL));
}
