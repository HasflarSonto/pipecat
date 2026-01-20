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
#include <time.h>

/* SDL for key codes */
#include <SDL2/SDL.h>

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

/* Dizzy duration is managed by face_renderer (3 seconds) */

/* Default server */
#define DEFAULT_HOST "localhost"
#define DEFAULT_PORT 7860
#define WS_PATH      "/luna-esp32"

/* State */
static volatile bool g_running = true;
static char g_server_host[256] = DEFAULT_HOST;
static int g_server_port = DEFAULT_PORT;
static bool g_audio_enabled = false;
static bool g_demo_mode = true;  /* Demo mode when not connected */
static bool g_connected = false;

/* Demo mode state */
typedef enum {
    DEMO_EMOTIONS,      /* Cycle through emotions */
    DEMO_CLOCK,         /* Show clock */
    DEMO_WEATHER,       /* Show weather */
    DEMO_TIMER,         /* Show timer countdown */
    DEMO_ANIMATION,     /* Show animation types */
    DEMO_MODE_COUNT
} demo_state_t;

static demo_state_t g_demo_state = DEMO_EMOTIONS;
static int g_demo_sub_state = 0;  /* Sub-state within each demo mode */
static uint32_t g_demo_last_update = 0;
static int g_demo_timer_seconds = 10;  /* Countdown for timer demo */

/* Demo timing */
#define DEMO_EMOTION_INTERVAL_MS  2000   /* Time per emotion */
#define DEMO_MODE_INTERVAL_MS     8000   /* Time per demo mode */
#define DEMO_TIMER_INTERVAL_MS    1000   /* Timer countdown interval */

/* LVGL tick timer (called from main loop) */
static uint32_t g_last_tick = 0;

/* Dizzy is managed by face_renderer - we just trigger it */

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
 * Handle shake detection callback
 * Called when window is moved rapidly back and forth
 * Only triggers dizzy effect when in face mode (like pet/poke)
 */
static void shake_callback(float intensity)
{
    /* Only trigger dizzy when in face mode */
    if (face_renderer_get_mode() != DISPLAY_MODE_FACE) {
        printf("Shake callback: ignored (not in face mode)\n");
        return;
    }

    printf("Shake callback: intensity=%.2f\n", intensity);

    /* Tell face renderer to go dizzy (it handles its own timeout) */
    face_renderer_set_dizzy(true);
}

/**
 * Handle eye poke detection
 * Called when an eye is clicked
 */
static void eye_poke_callback(int which_eye)
{
    printf("Eye poke: %s eye\n", which_eye == 0 ? "left" : "right");
    face_renderer_poke_eye(which_eye);
}

/**
 * Handle touch for petting interaction and eye poke
 */
static void touch_callback(bool pressed, int x, int y)
{
    /* Check for eye poke on click */
    if (pressed) {
        int eye = face_renderer_hit_test_eye(x, y);
        if (eye >= 0) {
            eye_poke_callback(eye);
            return;  /* Don't treat as petting if it's an eye poke */
        }
    }

    /* Forward touch to face renderer for petting detection */
    /* The face renderer reads touch state via bsp_display_lock/mutex */
    /* For now, we'll handle petting directly here if needed */
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

        case LUNA_CMD_WEATHER:
            printf("Weather: %s %s (%s)\n",
                   cmd->data.weather.temp,
                   cmd->data.weather.icon,
                   cmd->data.weather.description);
            {
                /* Map icon string to weather_icon_t enum */
                weather_icon_t icon = WEATHER_ICON_SUNNY;
                if (strcmp(cmd->data.weather.icon, "cloudy") == 0) {
                    icon = WEATHER_ICON_CLOUDY;
                } else if (strcmp(cmd->data.weather.icon, "rainy") == 0) {
                    icon = WEATHER_ICON_RAINY;
                } else if (strcmp(cmd->data.weather.icon, "snowy") == 0) {
                    icon = WEATHER_ICON_SNOWY;
                } else if (strcmp(cmd->data.weather.icon, "stormy") == 0) {
                    icon = WEATHER_ICON_STORMY;
                } else if (strcmp(cmd->data.weather.icon, "foggy") == 0) {
                    icon = WEATHER_ICON_FOGGY;
                } else if (strcmp(cmd->data.weather.icon, "partly_cloudy") == 0) {
                    icon = WEATHER_ICON_PARTLY_CLOUDY;
                }
                face_renderer_show_weather(
                    cmd->data.weather.temp,
                    icon,
                    cmd->data.weather.description
                );
            }
            break;

        case LUNA_CMD_TIMER:
            printf("Timer: %d:%02d %s %s\n",
                   cmd->data.timer.minutes,
                   cmd->data.timer.seconds,
                   cmd->data.timer.label,
                   cmd->data.timer.is_running ? "(running)" : "(paused)");
            face_renderer_show_timer(
                cmd->data.timer.minutes,
                cmd->data.timer.seconds,
                cmd->data.timer.label,
                cmd->data.timer.is_running
            );
            break;

        case LUNA_CMD_CLOCK:
            printf("Clock: %02d:%02d %s\n",
                   cmd->data.clock.hours,
                   cmd->data.clock.minutes,
                   cmd->data.clock.is_24h ? "(24h)" : "(12h)");
            face_renderer_show_clock(
                cmd->data.clock.hours,
                cmd->data.clock.minutes,
                cmd->data.clock.is_24h
            );
            break;

        case LUNA_CMD_ANIMATION:
            printf("Animation: %s\n", cmd->data.animation.type);
            {
                /* Map animation string to animation_type_t enum */
                animation_type_t anim = ANIMATION_RAIN;
                if (strcmp(cmd->data.animation.type, "snow") == 0) {
                    anim = ANIMATION_SNOW;
                } else if (strcmp(cmd->data.animation.type, "stars") == 0) {
                    anim = ANIMATION_STARS;
                } else if (strcmp(cmd->data.animation.type, "matrix") == 0) {
                    anim = ANIMATION_MATRIX;
                }
                face_renderer_show_animation(anim);
            }
            break;

        case LUNA_CMD_CLEAR_DISPLAY:
            printf("Clear display\n");
            face_renderer_clear_display();
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
            g_connected = true;
            g_demo_mode = false;  /* Disable demo mode when connected */
            face_renderer_clear_display();  /* Return to face mode */
            face_renderer_set_emotion(EMOTION_HAPPY);

            /* Start audio capture */
            sdl_audio_capture_start(audio_capture_callback);
            sdl_audio_playback_start();
            g_audio_enabled = true;
            break;

        case WS_EVENT_DISCONNECTED:
            printf("Disconnected from server\n");
            g_connected = false;
            g_demo_mode = true;  /* Re-enable demo mode when disconnected */
            g_demo_state = DEMO_EMOTIONS;
            g_demo_sub_state = 0;
            g_demo_last_update = get_time_ms();
            face_renderer_clear_display();  /* Return to face mode */
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
 * Update demo mode - cycles through emotions and display modes
 */
static void update_demo_mode(void)
{
    if (!g_demo_mode || g_connected) {
        return;
    }

    uint32_t now = get_time_ms();
    uint32_t elapsed = now - g_demo_last_update;

    /* Emotion names for demo */
    static const char* emotions[] = {
        "neutral", "happy", "sad", "angry", "surprised",
        "thinking", "confused", "excited", "cat"
    };
    static const int num_emotions = sizeof(emotions) / sizeof(emotions[0]);

    /* Weather conditions for demo */
    static const struct {
        const char* temp;
        weather_icon_t icon;
        const char* desc;
    } weather_conditions[] = {
        {"72°F", WEATHER_ICON_SUNNY, "Sunny"},
        {"65°F", WEATHER_ICON_PARTLY_CLOUDY, "Partly Cloudy"},
        {"58°F", WEATHER_ICON_CLOUDY, "Cloudy"},
        {"52°F", WEATHER_ICON_RAINY, "Rainy"},
    };
    static const int num_weather = sizeof(weather_conditions) / sizeof(weather_conditions[0]);

    /* Animation types for demo */
    static const animation_type_t animations[] = {
        ANIMATION_RAIN, ANIMATION_SNOW, ANIMATION_STARS, ANIMATION_MATRIX
    };
    static const int num_animations = sizeof(animations) / sizeof(animations[0]);

    switch (g_demo_state) {
        case DEMO_EMOTIONS:
            if (elapsed >= DEMO_EMOTION_INTERVAL_MS) {
                g_demo_last_update = now;

                /* Cycle to next emotion */
                face_renderer_set_emotion_str(emotions[g_demo_sub_state]);
                printf("Demo: Emotion -> %s\n", emotions[g_demo_sub_state]);

                g_demo_sub_state++;
                if (g_demo_sub_state >= num_emotions) {
                    /* Move to next demo mode */
                    g_demo_sub_state = 0;
                    g_demo_state = DEMO_CLOCK;
                    printf("Demo: Switching to CLOCK mode\n");
                }
            }
            break;

        case DEMO_CLOCK:
            if (elapsed >= 1000) {  /* Update clock every second */
                g_demo_last_update = now;

                /* Get real system time */
                time_t t = time(NULL);
                struct tm* tm_info = localtime(&t);

                face_renderer_show_clock(tm_info->tm_hour, tm_info->tm_min, false);
                printf("Demo: Clock -> %d:%02d\n", tm_info->tm_hour, tm_info->tm_min);

                g_demo_sub_state++;
                if (g_demo_sub_state >= 5) {  /* Show clock for 5 seconds */
                    g_demo_sub_state = 0;
                    g_demo_state = DEMO_WEATHER;
                    printf("Demo: Switching to WEATHER mode\n");
                }
            }
            break;

        case DEMO_WEATHER:
            if (elapsed >= 2000) {  /* Change weather every 2 seconds */
                g_demo_last_update = now;

                face_renderer_show_weather(
                    weather_conditions[g_demo_sub_state].temp,
                    weather_conditions[g_demo_sub_state].icon,
                    weather_conditions[g_demo_sub_state].desc
                );
                printf("Demo: Weather -> %s %s\n",
                       weather_conditions[g_demo_sub_state].temp,
                       weather_conditions[g_demo_sub_state].desc);

                g_demo_sub_state++;
                if (g_demo_sub_state >= num_weather) {
                    g_demo_sub_state = 0;
                    g_demo_state = DEMO_TIMER;
                    g_demo_timer_seconds = 10;
                    printf("Demo: Switching to TIMER mode\n");
                }
            }
            break;

        case DEMO_TIMER:
            if (elapsed >= DEMO_TIMER_INTERVAL_MS) {
                g_demo_last_update = now;

                int mins = g_demo_timer_seconds / 60;
                int secs = g_demo_timer_seconds % 60;
                face_renderer_show_timer(mins, secs, "Demo", true);
                printf("Demo: Timer -> %d:%02d\n", mins, secs);

                g_demo_timer_seconds--;
                if (g_demo_timer_seconds < 0) {
                    g_demo_sub_state = 0;
                    g_demo_state = DEMO_ANIMATION;
                    printf("Demo: Switching to ANIMATION mode\n");
                }
            }
            break;

        case DEMO_ANIMATION:
            if (elapsed >= 2000) {  /* Change animation every 2 seconds */
                g_demo_last_update = now;

                face_renderer_show_animation(animations[g_demo_sub_state]);
                printf("Demo: Animation -> %d\n", animations[g_demo_sub_state]);

                g_demo_sub_state++;
                if (g_demo_sub_state >= num_animations) {
                    g_demo_sub_state = 0;
                    g_demo_state = DEMO_EMOTIONS;
                    /* Return to face mode before cycling emotions */
                    face_renderer_clear_display();
                    printf("Demo: Switching back to EMOTIONS mode\n");
                }
            }
            break;

        default:
            g_demo_state = DEMO_EMOTIONS;
            break;
    }
}

/* Manual control state */
static int g_manual_emotion = 0;
static int g_manual_weather = 0;
static int g_manual_animation = 0;

/**
 * Keyboard handler for manual control
 */
static void keyboard_handler(int key)
{
    /* Emotion names */
    static const char* emotions[] = {
        "neutral", "happy", "sad", "angry", "surprised",
        "thinking", "confused", "excited", "cat"
    };
    static const int num_emotions = 9;

    switch (key) {
        /* Number keys 1-9 for emotions */
        case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4: case SDLK_5:
        case SDLK_6: case SDLK_7: case SDLK_8: case SDLK_9:
            g_demo_mode = false;
            g_manual_emotion = key - SDLK_1;
            if (g_manual_emotion < num_emotions) {
                face_renderer_clear_display();
                face_renderer_set_emotion_str(emotions[g_manual_emotion]);
                printf("Manual: Emotion -> %s\n", emotions[g_manual_emotion]);
            }
            break;

        /* F = Face mode (cycle emotions with arrows) */
        case SDLK_f:
            g_demo_mode = false;
            face_renderer_clear_display();
            face_renderer_set_emotion_str(emotions[g_manual_emotion]);
            printf("Manual: Face mode, emotion=%s\n", emotions[g_manual_emotion]);
            break;

        /* C = Clock mode */
        case SDLK_c:
            g_demo_mode = false;
            {
                time_t t = time(NULL);
                struct tm* tm_info = localtime(&t);
                face_renderer_show_clock(tm_info->tm_hour, tm_info->tm_min, false);
                printf("Manual: Clock mode -> %d:%02d\n", tm_info->tm_hour, tm_info->tm_min);
            }
            break;

        /* W = Weather mode (cycle with arrows) */
        case SDLK_w:
            g_demo_mode = false;
            {
                static const struct {
                    const char* temp;
                    weather_icon_t icon;
                    const char* desc;
                } weather[] = {
                    {"72°F", WEATHER_ICON_SUNNY, "Sunny"},
                    {"65°F", WEATHER_ICON_PARTLY_CLOUDY, "Partly Cloudy"},
                    {"58°F", WEATHER_ICON_CLOUDY, "Cloudy"},
                    {"52°F", WEATHER_ICON_RAINY, "Rainy"},
                    {"28°F", WEATHER_ICON_SNOWY, "Snowy"},
                };
                face_renderer_show_weather(
                    weather[g_manual_weather].temp,
                    weather[g_manual_weather].icon,
                    weather[g_manual_weather].desc
                );
                printf("Manual: Weather mode -> %s %s\n",
                       weather[g_manual_weather].temp,
                       weather[g_manual_weather].desc);
            }
            break;

        /* T = Timer mode */
        case SDLK_t:
            g_demo_mode = false;
            face_renderer_show_timer(25, 0, "Focus", false);
            printf("Manual: Timer mode -> 25:00 (press S to start)\n");
            break;

        /* S = Start timer */
        case SDLK_s:
            if (face_renderer_get_mode() == DISPLAY_MODE_TIMER) {
                face_renderer_timer_start();
                printf("Manual: Timer started\n");
            }
            break;

        /* P = Pause timer */
        case SDLK_p:
            if (face_renderer_get_mode() == DISPLAY_MODE_TIMER) {
                face_renderer_timer_pause();
                printf("Manual: Timer paused\n");
            }
            break;

        /* R = Reset timer */
        case SDLK_r:
            if (face_renderer_get_mode() == DISPLAY_MODE_TIMER) {
                face_renderer_timer_reset(25);
                printf("Manual: Timer reset to 25:00\n");
            }
            break;

        /* A = Animation mode (cycle with arrows) */
        case SDLK_a:
            g_demo_mode = false;
            {
                static const char* anim_names[] = {"Rain", "Snow", "Stars", "Matrix"};
                face_renderer_show_animation((animation_type_t)g_manual_animation);
                printf("Manual: Animation mode -> %s\n", anim_names[g_manual_animation]);
            }
            break;

        /* M = Subway/MTA mode (demo data for 1 train at 110 St downtown) */
        case SDLK_m:
            g_demo_mode = false;
            {
                int demo_times[] = {3, 8, 12};
                face_renderer_show_subway("1", 0xEE352E, "110 St", "Downtown", demo_times, 3);
                printf("Manual: Subway mode -> 1 train at 110 St downtown (3, 8, 12 min)\n");
            }
            break;

        /* Space = Toggle demo mode */
        case SDLK_SPACE:
            g_demo_mode = !g_demo_mode;
            if (g_demo_mode) {
                g_demo_state = DEMO_EMOTIONS;
                g_demo_sub_state = 0;
                g_demo_last_update = get_time_ms();
                face_renderer_clear_display();
                printf("Demo mode ENABLED\n");
            } else {
                printf("Demo mode DISABLED (use keys to control)\n");
            }
            break;

        /* Left/Right arrows = cycle sub-state */
        case SDLK_LEFT:
        case SDLK_RIGHT:
            {
                int delta = (key == SDLK_RIGHT) ? 1 : -1;
                display_mode_t mode = face_renderer_get_mode();

                if (mode == DISPLAY_MODE_FACE) {
                    g_manual_emotion = (g_manual_emotion + delta + num_emotions) % num_emotions;
                    face_renderer_set_emotion_str(emotions[g_manual_emotion]);
                    printf("Cycle: Emotion -> %s\n", emotions[g_manual_emotion]);
                } else if (mode == DISPLAY_MODE_WEATHER) {
                    g_manual_weather = (g_manual_weather + delta + 5) % 5;
                    /* Re-trigger W key handler */
                    keyboard_handler(SDLK_w);
                } else if (mode == DISPLAY_MODE_ANIMATION) {
                    g_manual_animation = (g_manual_animation + delta + 4) % 4;
                    keyboard_handler(SDLK_a);
                }
            }
            break;

        /* B = Force blink */
        case SDLK_b:
            face_renderer_blink();
            printf("Manual: Blink!\n");
            break;

        /* D = Trigger dizzy effect (switch to face mode first if needed) */
        case SDLK_d:
            g_demo_mode = false;
            /* Switch to face mode first if not already there */
            if (face_renderer_get_mode() != DISPLAY_MODE_FACE) {
                face_renderer_clear_display();
            }
            /* Trigger dizzy (face_renderer handles its own 3-second timeout) */
            face_renderer_set_dizzy(true);
            printf("Manual: Dizzy! (move window rapidly back and forth to trigger naturally)\n");
            break;

        /* H = Help */
        case SDLK_h:
            printf("\n=== Keyboard Controls ===\n");
            printf("1-9    : Set emotion (1=neutral, 2=happy, ...9=cat)\n");
            printf("F      : Face mode\n");
            printf("C      : Clock mode\n");
            printf("W      : Weather mode\n");
            printf("T      : Timer mode (25 min pomodoro)\n");
            printf("  S    : Start timer\n");
            printf("  P    : Pause timer\n");
            printf("  R    : Reset timer to 25:00\n");
            printf("A      : Animation mode\n");
            printf("M      : Subway/MTA mode (demo: 1 train at 110 St)\n");
            printf("B      : Force blink\n");
            printf("D      : Trigger dizzy effect (or move window rapidly)\n");
            printf("SPACE  : Toggle demo mode\n");
            printf("LEFT/RIGHT : Cycle through current mode\n");
            printf("ESC    : Quit\n");
            printf("\n=== Mouse Controls ===\n");
            printf("Click on eye : Poke that eye (makes it wink)\n");
            printf("Drag up/down : Pet the face\n");
            printf("=========================\n\n");
            break;

        default:
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
    printf("\nKeyboard Controls:\n");
    printf("  1-9        : Set emotion (1=neutral, 2=happy, ...9=cat)\n");
    printf("  F          : Face mode\n");
    printf("  C          : Clock mode (real time)\n");
    printf("  W          : Weather mode\n");
    printf("  T          : Timer mode (25 min pomodoro)\n");
    printf("    S        : Start timer\n");
    printf("    P        : Pause timer\n");
    printf("    R        : Reset timer\n");
    printf("  A          : Animation mode\n");
    printf("  B          : Force blink\n");
    printf("  SPACE      : Toggle demo mode on/off\n");
    printf("  LEFT/RIGHT : Cycle through current mode\n");
    printf("  H          : Show help\n");
    printf("  ESC        : Quit\n");
    printf("\nMouse:\n");
    printf("  Drag up/down : Pet the face\n");
}

int main(int argc, char* argv[])
{
    printf("Luna Simulator starting...\n");
    fflush(stdout);

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
    sdl_display_set_keyboard_callback(keyboard_handler);

    /* Set up shake detection callback */
    sdl_display_set_shake_callback(shake_callback);
    sdl_display_enable_shake_detection(true);

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

    /* DON'T start face render task - we'll tick manually from main loop
     * (LVGL is not thread-safe, so background render task causes issues) */
    /* face_renderer_start(); */
    face_renderer_set_emotion(EMOTION_NEUTRAL);

    /* Initialize demo mode timer */
    g_demo_last_update = get_time_ms();
    printf("Demo mode enabled - will cycle through emotions and display modes\n");

    /* Initialize WebSocket client */
    if (!ws_client_init(ws_event_handler)) {
        fprintf(stderr, "Failed to initialize WebSocket client\n");
        /* face_renderer_stop(); - not started in simulator */
        face_renderer_deinit();
        sdl_audio_deinit();
        sdl_mouse_deinit();
        sdl_display_deinit();
        return 1;
    }

    /* Connect to server */
    ws_client_connect(g_server_host, g_server_port, WS_PATH);

    printf("Simulator running. Press ESC or close window to exit.\n");

    /* Track time for face renderer tick */
    uint32_t last_render_time = get_time_ms();

    /* Main loop */
    while (g_running && !sdl_display_quit_requested()) {
        /* Calculate delta time for face renderer */
        uint32_t now = get_time_ms();
        uint32_t delta_ms = now - last_render_time;
        last_render_time = now;

        /* Update LVGL tick */
        update_lvgl_tick();

        /* Handle SDL events */
        sdl_display_poll_events();

        /* Service WebSocket (skipped when disconnected to avoid blocking) */
        ws_client_service(0);

        /* Update demo mode (when not connected) */
        update_demo_mode();

        /* Tick face renderer (updates animation, widgets, dizzy timeout) */
        face_renderer_tick(delta_ms);

        /* Run LVGL tasks */
        lv_timer_handler();

        /* Small delay to prevent CPU hogging */
        usleep(5000);  /* 5ms = ~200Hz main loop */
    }

    printf("Cleaning up...\n");

    /* Cleanup */
    ws_client_disconnect();
    ws_client_deinit();

    /* face_renderer_stop(); - not started in simulator */
    face_renderer_deinit();

    sdl_audio_deinit();
    sdl_mouse_deinit();
    sdl_display_deinit();

    luna_protocol_deinit();

    printf("Goodbye!\n");
    return 0;
}
