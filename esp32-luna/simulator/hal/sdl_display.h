/**
 * SDL2 Display Driver for Luna Simulator
 */

#ifndef SDL_DISPLAY_H
#define SDL_DISPLAY_H

#include "lvgl.h"
#include <stdbool.h>

/**
 * Initialize SDL2 display
 * Creates window and LVGL display driver
 * @param width Display width
 * @param height Display height
 * @return LVGL display pointer, or NULL on failure
 */
lv_display_t* sdl_display_init(int width, int height);

/**
 * Deinitialize SDL2 display
 */
void sdl_display_deinit(void);

/**
 * Check if display is initialized
 */
bool sdl_display_is_init(void);

/**
 * Check if window close was requested
 */
bool sdl_display_quit_requested(void);

/**
 * Process SDL events
 * Call this regularly in the main loop
 */
void sdl_display_poll_events(void);

#endif /* SDL_DISPLAY_H */
