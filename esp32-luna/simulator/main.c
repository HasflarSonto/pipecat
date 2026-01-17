/**
 * Luna Simulator Main Entry Point
 *
 * Simulates the ESP32-Luna hardware client on desktop using SDL2.
 * Connects to the pipecat backend via WebSocket.
 *
 * Usage: ./luna_simulator [host] [port]
 *   host: Server hostname (default: localhost)
 *   port: Server port (default: 7860)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* LVGL */
#include "lvgl.h"

/* HAL drivers */
#include "hal/sdl_display.h"
#include "hal/sdl_mouse.h"
#include "hal/sdl_audio.h"

/* Network */
#include "net/ws_client_sim.h"

/* Luna face renderer */
#include "face_renderer.h"
#include "luna_protocol.h"

/* Display dimensions */
#define DISPLAY_WIDTH  502
#define DISPLAY_HEIGHT 410

/* Default server */
#define DEFAULT_HOST "localhost"
#define DEFAULT_PORT 7860
#define WS_PATH      "/luna-esp32"

/* State */
static volatile bool g_running = true;
static char g_server_host[256] = DEFAULT_HOST;
static int g_server_port = DEFAULT_PORT;
static bool g_audio_enabled = false;

/* LVGL tick timer (called from main loop) */
static uint32_t g_last_tick = 0;

/**
 * Get current time in milliseconds
 */
static uint32_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/**
 * Update LVGL tick
 */
static void update_lvgl_tick(void)
{
    uint32_t now = get_time_ms();
    uint32_t elapsed = now - g_last_tick;
    g_last_tick = now;

    if (elapsed > 0) {
        lv_tick_inc(elapsed);
    }
}

/**
 * Handle touch for petting interaction
 */
static void touch_callback(bool pressed, int x, int y)
{
    /* Forward touch to face renderer for petting detection */
    /* The face renderer reads touch state via bsp_display_lock/mutex */
    /* For now, we'll handle petting directly here if needed */
    (void)pressed;
    (void)x;
    (void)y;
}

/**
 * Handle Luna protocol commands
 */
static void handle_luna_command(const luna_cmd_t* cmd)
{
    switch (cmd->type) {
        case LUNA_CMD_EMOTION:
            printf("Emotion: %s\n", cmd->data.emotion.emotion);
            face_renderer_set_emotion_str(cmd->data.emotion.emotion);
            break;

        case LUNA_CMD_GAZE:
            face_renderer_set_gaze(cmd->data.gaze.x, cmd->data.gaze.y);
            break;

        case LUNA_CMD_TEXT:
            printf("Text: %s\n", cmd->data.text.content);
            face_renderer_show_text(
                cmd->data.text.content,
                (font_size_t)cmd->data.text.size,
                cmd->data.text.color,
                cmd->data.text.bg_color
            );
            break;

        case LUNA_CMD_TEXT_CLEAR:
            face_renderer_clear_text();
            break;

        case LUNA_CMD_PIXEL_ART:
            /* Convert pixel array to format expected by face_renderer */
            if (cmd->data.pixel_art.pixel_count > 0 && cmd->data.pixel_art.pixels) {
                uint32_t* pixels = malloc(cmd->data.pixel_art.pixel_count * sizeof(uint32_t));
                if (pixels) {
                    for (size_t i = 0; i < cmd->data.pixel_art.pixel_count; i++) {
                        luna_pixel_t* p = &cmd->data.pixel_art.pixels[i];
                        /* Pack as: x | (y << 8) | (color << 16) - but that's only 8 bits for color */
                        /* Better: Store x, y, color separately */
                        /* For now, skip - face_renderer_show_pixel_art needs different format */
                    }
                    free(pixels);
                }
            }
            break;

        case LUNA_CMD_PIXEL_ART_CLEAR:
            face_renderer_clear_pixel_art();
            break;

        case LUNA_CMD_AUDIO_START:
            printf("Audio start\n");
            if (!g_audio_enabled) {
                sdl_audio_playback_start();
                g_audio_enabled = true;
            }
            break;

        case LUNA_CMD_AUDIO_STOP:
            printf("Audio stop\n");
            if (g_audio_enabled) {
                sdl_audio_playback_stop();
                g_audio_enabled = false;
            }
            break;

        default:
            printf("Unknown command type: %d\n", cmd->type);
            break;
    }
}

/**
 * Audio capture callback - sends audio to server
 */
static void audio_capture_callback(const int16_t* data, size_t samples)
{
    if (!ws_client_is_connected()) {
        return;
    }

    /* Send binary audio data to server */
    ws_client_send_binary(data, samples * sizeof(int16_t));
}

/**
 * WebSocket event handler
 */
static void ws_event_handler(ws_event_t event, const void* data, size_t len)
{
    switch (event) {
        case WS_EVENT_CONNECTED:
            printf("Connected to server\n");
            face_renderer_set_emotion(EMOTION_HAPPY);

            /* Start audio capture */
            sdl_audio_capture_start(audio_capture_callback);
            sdl_audio_playback_start();
            g_audio_enabled = true;
            break;

        case WS_EVENT_DISCONNECTED:
            printf("Disconnected from server\n");
            face_renderer_set_emotion(EMOTION_CONFUSED);

            /* Stop audio */
            sdl_audio_capture_stop();
            sdl_audio_playback_stop();
            g_audio_enabled = false;
            break;

        case WS_EVENT_TEXT:
            /* Parse JSON command */
            {
                luna_cmd_t cmd;
                if (luna_protocol_parse((const char*)data, &cmd) == ESP_OK) {
                    handle_luna_command(&cmd);
                    luna_protocol_free_cmd(&cmd);
                }
            }
            break;

        case WS_EVENT_BINARY:
            /* Audio data from server - play it */
            if (len > 0) {
                size_t samples = len / sizeof(int16_t);
                sdl_audio_playback_feed((const int16_t*)data, samples);
            }
            break;

        case WS_EVENT_ERROR:
            fprintf(stderr, "WebSocket error: %s\n", data ? (const char*)data : "unknown");
            break;
    }
}

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int sig)
{
    (void)sig;
    printf("\nShutting down...\n");
    g_running = false;
}

/**
 * Print usage
 */
static void print_usage(const char* prog)
{
    printf("Luna ESP32 Simulator\n");
    printf("Usage: %s [host] [port]\n", prog);
    printf("  host: Server hostname (default: %s)\n", DEFAULT_HOST);
    printf("  port: Server port (default: %d)\n", DEFAULT_PORT);
    printf("\nControls:\n");
    printf("  ESC or close window: Exit\n");
    printf("  Mouse drag: Pet the face\n");
}

int main(int argc, char* argv[])
{
    printf("Luna Simulator starting...\n");

    /* Parse arguments */
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        strncpy(g_server_host, argv[1], sizeof(g_server_host) - 1);
    }
    if (argc > 2) {
        g_server_port = atoi(argv[2]);
        if (g_server_port <= 0 || g_server_port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            return 1;
        }
    }

    printf("Server: ws://%s:%d%s\n", g_server_host, g_server_port, WS_PATH);

    /* Install signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize LVGL */
    lv_init();
    g_last_tick = get_time_ms();

    /* Initialize SDL display */
    lv_display_t* display = sdl_display_init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!display) {
        fprintf(stderr, "Failed to initialize display\n");
        return 1;
    }

    /* Initialize mouse input */
    if (!sdl_mouse_init(display)) {
        fprintf(stderr, "Failed to initialize mouse input\n");
        sdl_display_deinit();
        return 1;
    }
    sdl_mouse_set_callback(touch_callback);

    /* Initialize audio */
    if (!sdl_audio_init()) {
        fprintf(stderr, "Warning: Failed to initialize audio\n");
        /* Continue without audio */
    }

    /* Initialize Luna protocol */
    luna_protocol_init();

    /* Initialize face renderer */
    face_renderer_config_t face_config = {0};
    if (face_renderer_init(&face_config) != ESP_OK) {
        fprintf(stderr, "Failed to initialize face renderer\n");
        sdl_audio_deinit();
        sdl_mouse_deinit();
        sdl_display_deinit();
        return 1;
    }

    /* Start face animation */
    face_renderer_start();
    face_renderer_set_emotion(EMOTION_NEUTRAL);

    /* Initialize WebSocket client */
    if (!ws_client_init(ws_event_handler)) {
        fprintf(stderr, "Failed to initialize WebSocket client\n");
        face_renderer_stop();
        face_renderer_deinit();
        sdl_audio_deinit();
        sdl_mouse_deinit();
        sdl_display_deinit();
        return 1;
    }

    /* Connect to server */
    ws_client_connect(g_server_host, g_server_port, WS_PATH);

    printf("Simulator running. Press ESC or close window to exit.\n");

    /* Main loop */
    while (g_running && !sdl_display_quit_requested()) {
        /* Update LVGL tick */
        update_lvgl_tick();

        /* Handle SDL events */
        sdl_display_poll_events();

        /* Service WebSocket */
        ws_client_service(0);  /* Non-blocking */

        /* Run LVGL tasks */
        lv_timer_handler();

        /* Small delay to prevent CPU hogging */
        usleep(5000);  /* 5ms = ~200Hz main loop */
    }

    printf("Cleaning up...\n");

    /* Cleanup */
    ws_client_disconnect();
    ws_client_deinit();

    face_renderer_stop();
    face_renderer_deinit();

    sdl_audio_deinit();
    sdl_mouse_deinit();
    sdl_display_deinit();

    luna_protocol_deinit();

    printf("Goodbye!\n");
    return 0;
}
