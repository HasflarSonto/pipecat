/**
 * SDL2 Mouse/Touch Driver for Luna Simulator
 * Provides LVGL input device using mouse as touch simulation
 */

#include "sdl_mouse.h"
#include <stdio.h>

/* Mouse state */
static bool s_pressed = false;
static int s_last_x = 0;
static int s_last_y = 0;
static lv_indev_t* s_indev = NULL;
static touch_callback_t s_touch_callback = NULL;

/**
 * LVGL input read callback
 */
static void mouse_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    (void)indev;

    data->point.x = s_last_x;
    data->point.y = s_last_y;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool sdl_mouse_init(lv_display_t* disp)
{
    if (!disp) {
        fprintf(stderr, "Cannot init mouse: display is NULL\n");
        return false;
    }

    /* Create LVGL input device */
    s_indev = lv_indev_create();
    if (!s_indev) {
        fprintf(stderr, "Failed to create LVGL indev\n");
        return false;
    }

    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, mouse_read_cb);
    lv_indev_set_display(s_indev, disp);

    printf("SDL mouse initialized\n");
    return true;
}

void sdl_mouse_deinit(void)
{
    if (s_indev) {
        lv_indev_delete(s_indev);
        s_indev = NULL;
    }
    s_touch_callback = NULL;
}

void sdl_mouse_handle_event(SDL_Event* event)
{
    if (!event) return;

    switch (event->type) {
        case SDL_MOUSEBUTTONDOWN:
            if (event->button.button == SDL_BUTTON_LEFT) {
                s_pressed = true;
                s_last_x = event->button.x;
                s_last_y = event->button.y;
                if (s_touch_callback) {
                    s_touch_callback(true, s_last_x, s_last_y);
                }
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (event->button.button == SDL_BUTTON_LEFT) {
                s_pressed = false;
                s_last_x = event->button.x;
                s_last_y = event->button.y;
                if (s_touch_callback) {
                    s_touch_callback(false, s_last_x, s_last_y);
                }
            }
            break;

        case SDL_MOUSEMOTION:
            s_last_x = event->motion.x;
            s_last_y = event->motion.y;
            if (s_pressed && s_touch_callback) {
                s_touch_callback(true, s_last_x, s_last_y);
            }
            break;

        default:
            break;
    }
}

void sdl_mouse_get_state(bool* is_pressed, int* x, int* y)
{
    if (is_pressed) *is_pressed = s_pressed;
    if (x) *x = s_last_x;
    if (y) *y = s_last_y;
}

void sdl_mouse_set_callback(touch_callback_t cb)
{
    s_touch_callback = cb;
}

lv_indev_t* sdl_mouse_get_indev(void)
{
    return s_indev;
}
