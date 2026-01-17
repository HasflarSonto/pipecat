/**
 * SDL2 Mouse/Touch Driver for Luna Simulator
 */

#ifndef SDL_MOUSE_H
#define SDL_MOUSE_H

#include "lvgl.h"
#include <SDL2/SDL.h>
#include <stdbool.h>

/**
 * Initialize mouse input driver
 * Must be called after display initialization
 * @param disp LVGL display to attach to
 * @return true on success
 */
bool sdl_mouse_init(lv_display_t* disp);

/**
 * Deinitialize mouse driver
 */
void sdl_mouse_deinit(void);

/**
 * Handle SDL mouse event
 * Called by sdl_display_poll_events
 */
void sdl_mouse_handle_event(SDL_Event* event);

/**
 * Get current touch state (for petting detection)
 * @param is_pressed Output: true if mouse button pressed
 * @param x Output: current X position
 * @param y Output: current Y position
 */
void sdl_mouse_get_state(bool* is_pressed, int* x, int* y);

/**
 * Touch callback for face renderer petting
 * Set this to receive touch updates
 */
typedef void (*touch_callback_t)(bool pressed, int x, int y);
void sdl_mouse_set_callback(touch_callback_t cb);

#endif /* SDL_MOUSE_H */
