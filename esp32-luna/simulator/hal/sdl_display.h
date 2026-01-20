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

/**
 * Keyboard callback type
 * @param key SDL keycode (SDLK_*)
 */
typedef void (*sdl_keyboard_callback_t)(int key);

/**
 * Set keyboard callback for handling key presses
 * @param callback Function to call on key press
 */
void sdl_display_set_keyboard_callback(sdl_keyboard_callback_t callback);

/**
 * Shake callback type
 * @param intensity Shake intensity (0.0 - 1.0)
 */
typedef void (*sdl_shake_callback_t)(float intensity);

/**
 * Set shake callback for handling window shake gestures
 * Moving the window rapidly back and forth triggers shake detection
 * @param callback Function to call when shake detected
 */
void sdl_display_set_shake_callback(sdl_shake_callback_t callback);

/**
 * Eye poke callback type
 * @param which_eye 0 = left eye, 1 = right eye
 */
typedef void (*sdl_eye_poke_callback_t)(int which_eye);

/**
 * Set eye poke callback for handling clicks on eyes
 * @param callback Function to call when eye is clicked
 */
void sdl_display_set_eye_poke_callback(sdl_eye_poke_callback_t callback);

/**
 * Enable shake detection
 * @param enabled True to enable shake detection
 */
void sdl_display_enable_shake_detection(bool enabled);

/**
 * Get eye poke callback (for mouse handler to use)
 * @return Current eye poke callback, or NULL if not set
 */
sdl_eye_poke_callback_t sdl_display_get_eye_poke_callback(void);

#endif /* SDL_DISPLAY_H */
