/**
 * SDL2 Display Driver for Luna Simulator
 * Provides LVGL display backend using SDL2
 */

#include "sdl_display.h"
#include "sdl_mouse.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>

/* Display dimensions */
#define LUNA_DISPLAY_WIDTH  502
#define LUNA_DISPLAY_HEIGHT 410

/* Global display pointer for BSP stub */
lv_display_t* g_sim_display = NULL;

/* SDL state */
static SDL_Window* s_window = NULL;
static SDL_Renderer* s_renderer = NULL;
static SDL_Texture* s_texture = NULL;
static bool s_initialized = false;
static bool s_quit_requested = false;
static int s_width = 0;
static int s_height = 0;

/* LVGL draw buffers (raw bytes for RGB565) */
static void* s_buf1 = NULL;
static void* s_buf2 = NULL;

/* Keyboard callback */
static sdl_keyboard_callback_t s_keyboard_callback = NULL;

/**
 * LVGL flush callback - renders to SDL texture
 * Using PARTIAL render mode for better performance
 * Only updates the dirty rectangle area
 */
static void sdl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    /* Calculate the dirty rectangle */
    SDL_Rect rect;
    rect.x = area->x1;
    rect.y = area->y1;
    rect.w = area->x2 - area->x1 + 1;
    rect.h = area->y2 - area->y1 + 1;

    /* Update only the dirty area of the texture
     * pitch = width of the area * 2 bytes per pixel (RGB565) */
    int pitch = rect.w * 2;
    SDL_UpdateTexture(s_texture, &rect, px_map, pitch);

    /* Only present on the last flush of this frame (when flushing is complete) */
    if (lv_display_flush_is_last(disp)) {
        SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
        SDL_RenderPresent(s_renderer);
    }

    /* Tell LVGL flush is done */
    lv_display_flush_ready(disp);
}

lv_display_t* sdl_display_init(int width, int height)
{
    if (s_initialized) {
        return g_sim_display;
    }

    s_width = width;
    s_height = height;

    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return NULL;
    }

    /* Create window */
    s_window = SDL_CreateWindow(
        "Luna Simulator",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN
    );

    if (!s_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return NULL;
    }

    /* Create renderer - NO VSYNC for responsive updates */
    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED);
    if (!s_renderer) {
        /* Fallback to software renderer if accelerated fails */
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return NULL;
    }

    /* Create texture - ESP32 uses RGB565 format (16-bit, 2 bytes per pixel)
     * SDL_PIXELFORMAT_RGB565 matches LVGL's LV_COLOR_FORMAT_RGB565 */
    s_texture = SDL_CreateTexture(
        s_renderer,
        SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING,
        width, height
    );

    if (!s_texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(s_renderer);
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return NULL;
    }

    /* Clear texture to dark background color (0x1E1E28 in RGB565) */
    SDL_SetRenderDrawColor(s_renderer, 0x1E, 0x1E, 0x28, 0xFF);
    SDL_RenderClear(s_renderer);
    SDL_RenderPresent(s_renderer);

    /* Allocate LVGL draw buffers (double buffered, full screen)
     * RGB565 = 2 bytes per pixel
     * Full screen buffer is fine on desktop - RAM is plentiful */
    size_t buf_size = width * height;
    size_t buf_bytes = buf_size * 2;  /* 2 bytes per pixel for RGB565 */
    s_buf1 = malloc(buf_bytes);
    s_buf2 = malloc(buf_bytes);

    if (!s_buf1 || !s_buf2) {
        fprintf(stderr, "Failed to allocate LVGL buffers\n");
        free(s_buf1);
        free(s_buf2);
        SDL_DestroyTexture(s_texture);
        SDL_DestroyRenderer(s_renderer);
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return NULL;
    }

    /* Create LVGL display */
    g_sim_display = lv_display_create(width, height);
    if (!g_sim_display) {
        fprintf(stderr, "Failed to create LVGL display\n");
        free(s_buf1);
        free(s_buf2);
        SDL_DestroyTexture(s_texture);
        SDL_DestroyRenderer(s_renderer);
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return NULL;
    }

    /* Set display color format to RGB565 (matching ESP32) */
    lv_display_set_color_format(g_sim_display, LV_COLOR_FORMAT_RGB565);

    /* Set up LVGL display buffers - use PARTIAL mode for better performance
     * Only dirty areas will be redrawn */
    lv_display_set_buffers(g_sim_display, s_buf1, s_buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Set flush callback */
    lv_display_set_flush_cb(g_sim_display, sdl_flush_cb);

    s_initialized = true;
    s_quit_requested = false;

    printf("SDL display initialized: %dx%d\n", width, height);
    return g_sim_display;
}

void sdl_display_deinit(void)
{
    if (!s_initialized) return;

    if (g_sim_display) {
        lv_display_delete(g_sim_display);
        g_sim_display = NULL;
    }

    free(s_buf1);
    free(s_buf2);
    s_buf1 = NULL;
    s_buf2 = NULL;

    if (s_texture) {
        SDL_DestroyTexture(s_texture);
        s_texture = NULL;
    }

    if (s_renderer) {
        SDL_DestroyRenderer(s_renderer);
        s_renderer = NULL;
    }

    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }

    SDL_Quit();
    s_initialized = false;

    printf("SDL display deinitialized\n");
}

bool sdl_display_is_init(void)
{
    return s_initialized;
}

bool sdl_display_quit_requested(void)
{
    return s_quit_requested;
}

void sdl_display_poll_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                s_quit_requested = true;
                break;

            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEMOTION:
                /* Forward to mouse handler */
                sdl_mouse_handle_event(&event);
                break;

            case SDL_KEYDOWN:
                /* Handle keyboard shortcuts */
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    s_quit_requested = true;
                }
                /* Forward to keyboard callback */
                if (s_keyboard_callback) {
                    s_keyboard_callback(event.key.keysym.sym);
                }
                break;

            default:
                break;
        }
    }
}

void sdl_display_set_keyboard_callback(sdl_keyboard_callback_t callback)
{
    s_keyboard_callback = callback;
}
