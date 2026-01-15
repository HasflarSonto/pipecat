/*
 * Luna Face Renderer for ESP32
 * Canvas-based rendering engine using LVGL
 * Ported from luna_face_renderer.py
 *
 * Uses LVGL canvas (not widgets) for clean rendering without artifacts.
 * Canvas is cleared and redrawn each frame - no dirty rectangle issues.
 * Rotated 90° clockwise for landscape mode.
 */

#include "face_renderer.h"
#include "emotions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

static const char *TAG = "face_renderer";

// Display configuration - LANDSCAPE after 90° rotation
// Original hardware: 410x502, After rotation: 502x410
#define DISPLAY_WIDTH       502
#define DISPLAY_HEIGHT      410

// Canvas size (full screen for face)
#define CANVAS_WIDTH        DISPLAY_WIDTH
#define CANVAS_HEIGHT       DISPLAY_HEIGHT

// Colors
#define BG_COLOR_R          30
#define BG_COLOR_G          30
#define BG_COLOR_B          40
#define FACE_COLOR_R        255
#define FACE_COLOR_G        255
#define FACE_COLOR_B        255

// Animation timing
#define ANIMATION_PERIOD_MS    50       // ~20 FPS
#define RENDER_TASK_STACK_SIZE (12 * 1024)
#define RENDER_TASK_PRIORITY   3
#define RENDER_TASK_CORE       1

#define EMOTION_TRANSITION_SPEED    4.0f
#define GAZE_FOLLOW_SPEED          10.0f
#define BLINK_SPEED                10.0f
#define FACE_SHIFT_SPEED           6.0f

// Blink timing
#define BLINK_MIN_INTERVAL_MS      2000
#define BLINK_MAX_INTERVAL_MS      5000

// Scale factors from 240x320 reference to 502x410 landscape
#define SCALE_X                    2.092f   // 502/240
#define SCALE_Y                    1.281f   // 410/320

// Pixel art grid
#define PIXEL_GRID_COLS            12
#define PIXEL_GRID_ROWS            16
#define PIXEL_CELL_SIZE            34

// Maximum text length
#define MAX_TEXT_LENGTH            512

// Canvas draw buffer - will be allocated in PSRAM
static uint8_t *s_canvas_buf = NULL;

// Renderer state
static struct {
    // Configuration
    uint16_t width;
    uint16_t height;

    // Display
    lv_display_t *display;

    // Canvas for face rendering
    lv_obj_t *canvas;

    // Text label (widget, shown only in text mode)
    lv_obj_t *text_label;

    // Face geometry
    int center_x;
    int center_y;
    int eye_spacing;
    int left_eye_x;
    int right_eye_x;
    int eye_y;
    int mouth_y;

    // Display mode
    display_mode_t mode;

    // Emotion state
    emotion_id_t current_emotion;
    emotion_id_t target_emotion;
    float emotion_transition;
    emotion_config_t current_params;

    // Gaze state (0-1, 0.5 = center)
    float gaze_x;
    float gaze_y;
    float target_gaze_x;
    float target_gaze_y;

    // Face offset for edge tracking
    float face_offset_x;
    float face_offset_y;

    // Blink state
    float blink_progress;
    bool is_blinking;
    int64_t last_blink_time;
    int32_t blink_interval_ms;

    // Cat mode
    bool cat_mode;

    // Text mode
    char text_content[MAX_TEXT_LENGTH];
    font_size_t text_size;
    uint32_t text_color;
    uint32_t text_bg_color;

    // Pixel art
    uint32_t *pixel_data;
    size_t pixel_count;
    uint32_t pixel_bg_color;

    // Render task
    TaskHandle_t render_task;
    int64_t last_anim_time;

    // State
    bool running;
    bool initialized;

    // Performance tracking
    float actual_fps;
    int64_t last_fps_time;
    int frame_count;

    // Mutex for thread safety
    SemaphoreHandle_t mutex;
} s_renderer = {0};

// Forward declarations
static void render_task_func(void *pvParameters);
static void update_animation(float delta_time);
static void draw_face(void);
static float get_blink_factor(void);

static float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

static lv_color_t make_color(uint8_t r, uint8_t g, uint8_t b)
{
    return lv_color_make(r, g, b);
}

static int random_range(int min_val, int max_val)
{
    return min_val + (esp_random() % (max_val - min_val + 1));
}

// Draw a filled rounded rectangle
static void draw_rounded_rect(lv_layer_t *layer, int x1, int y1, int x2, int y2, int radius, lv_color_t color)
{
    if (x2 <= x1 || y2 <= y1) return;

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = color;
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.radius = radius;

    lv_area_t area = {
        .x1 = x1,
        .y1 = y1,
        .x2 = x2,
        .y2 = y2
    };
    lv_draw_rect(layer, &rect_dsc, &area);
}

// Draw a filled circle
static void draw_circle(lv_layer_t *layer, int cx, int cy, int radius, lv_color_t color)
{
    draw_rounded_rect(layer, cx - radius, cy - radius, cx + radius, cy + radius, radius, color);
}

// Draw an arc (curved line)
static void draw_arc(lv_layer_t *layer, int cx, int cy, int radius, int start_angle, int end_angle, int width, lv_color_t color)
{
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.color = color;
    arc_dsc.width = width;
    arc_dsc.start_angle = start_angle;
    arc_dsc.end_angle = end_angle;
    arc_dsc.center.x = cx;
    arc_dsc.center.y = cy;
    arc_dsc.radius = radius;
    arc_dsc.opa = LV_OPA_COVER;
    arc_dsc.rounded = true;

    lv_draw_arc(layer, &arc_dsc);
}

// Draw a line
static void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int width, lv_color_t color)
{
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = color;
    line_dsc.width = width;
    line_dsc.opa = LV_OPA_COVER;
    line_dsc.round_start = true;
    line_dsc.round_end = true;

    line_dsc.p1.x = x1;
    line_dsc.p1.y = y1;
    line_dsc.p2.x = x2;
    line_dsc.p2.y = y2;

    lv_draw_line(layer, &line_dsc);
}

// Draw robot eye (rounded rectangle)
static void draw_robot_eye(lv_layer_t *layer, int x, int y, bool is_right)
{
    emotion_config_t *params = &s_renderer.current_params;
    float blink_factor = get_blink_factor();

    float eye_height = params->eye_height * SCALE_Y;
    float eye_width = params->eye_width * SCALE_X;

    // Apply openness
    eye_height *= params->eye_openness;

    // Apply blink
    eye_height *= (1.0f - blink_factor * 0.95f);

    // Gaze offset
    float gaze_range_x = 28.0f * SCALE_X;
    float gaze_range_y = 18.0f * SCALE_Y;
    int gaze_x_offset = (int)((s_renderer.gaze_x - 0.5f) * 2.0f * gaze_range_x);
    int gaze_y_offset = (int)((s_renderer.gaze_y - 0.5f) * 2.0f * gaze_range_y);

    // Tilt for confused
    int y_offset = gaze_y_offset;
    if (params->tilt_eyes) {
        y_offset += is_right ? (int)(8.0f * SCALE_Y) : (int)(-8.0f * SCALE_Y);
    }

    // Look to side for thinking
    int x_offset = gaze_x_offset;
    if (params->look_side) {
        x_offset = (int)(12.0f * SCALE_X);
    }

    // Minimum height
    if (eye_height < 5) eye_height = 5;

    int w = (int)eye_width;
    int h = (int)eye_height;
    int radius = (h < w) ? h / 2 : w / 2;
    if (radius > (int)(15.0f * SCALE_Y)) radius = (int)(15.0f * SCALE_Y);

    lv_color_t face_color = make_color(FACE_COLOR_R, FACE_COLOR_G, FACE_COLOR_B);
    draw_rounded_rect(layer,
                      x + x_offset - w/2,
                      y + y_offset - h/2,
                      x + x_offset + w/2,
                      y + y_offset + h/2,
                      radius, face_color);

    // Draw sparkle if excited
    if (params->sparkle) {
        lv_color_t bg_color = make_color(BG_COLOR_R, BG_COLOR_G, BG_COLOR_B);
        int sparkle_size = (int)(12 * SCALE_X);
        int sparkle_x = x + x_offset - w/4;
        int sparkle_y = y + y_offset - h/4;
        draw_circle(layer, sparkle_x, sparkle_y, sparkle_size/2, bg_color);
    }
}

// Draw robot mouth - simple curved line or O shape
static void draw_robot_mouth(lv_layer_t *layer, int offset_x, int offset_y)
{
    emotion_config_t *params = &s_renderer.current_params;
    float mouth_curve = params->mouth_curve;
    float mouth_open = params->mouth_open;
    int mouth_width = (int)(params->mouth_width * SCALE_X);

    int x = s_renderer.center_x + offset_x;
    int y = s_renderer.mouth_y + offset_y;

    lv_color_t face_color = make_color(FACE_COLOR_R, FACE_COLOR_G, FACE_COLOR_B);
    lv_color_t bg_color = make_color(BG_COLOR_R, BG_COLOR_G, BG_COLOR_B);
    int line_width = (int)(4 * SCALE_Y);

    if (mouth_open > 0.3f) {
        // Draw O-shaped mouth for surprised (ring shape)
        int o_width = (int)((20 + mouth_open * 15) * SCALE_X);
        int o_height = (int)((15 + mouth_open * 20) * SCALE_Y);

        // Outer ellipse (white)
        draw_rounded_rect(layer, x - o_width, y - o_height, x + o_width, y + o_height, o_height, face_color);
        // Inner ellipse (dark) - makes it a ring
        int inner_pad = (int)(4 * SCALE_Y);
        draw_rounded_rect(layer, x - o_width + inner_pad, y - o_height + inner_pad,
                          x + o_width - inner_pad, y + o_height - inner_pad,
                          o_height - inner_pad, bg_color);
    } else if (fabsf(mouth_curve) < 0.1f) {
        // Straight line for neutral
        draw_line(layer, x - mouth_width, y, x + mouth_width, y, line_width, face_color);
    } else {
        // Curved smile or frown using arc
        int curve_amount = (int)(fabsf(mouth_curve) * 15.0f * SCALE_Y);
        int arc_radius = mouth_width;

        if (mouth_curve > 0) {
            // Smile - arc curving down (0-180 degrees)
            draw_arc(layer, x, y - curve_amount, arc_radius, 0, 180, line_width, face_color);
        } else {
            // Frown - arc curving up (180-360 degrees)
            draw_arc(layer, x, y + curve_amount, arc_radius, 180, 360, line_width, face_color);
        }
    }
}

// Draw cat mouth - horizontal ω shape with whiskers
static void draw_cat_mouth(lv_layer_t *layer, int offset_x, int offset_y)
{
    int x = s_renderer.center_x + offset_x;
    int y = s_renderer.eye_y + (int)(50 * SCALE_Y) + offset_y;

    lv_color_t face_color = make_color(FACE_COLOR_R, FACE_COLOR_G, FACE_COLOR_B);
    int line_width = (int)(4 * SCALE_Y);

    int bump_size = (int)(10 * SCALE_Y);
    int bump_width = (int)(18 * SCALE_X);

    // Left bump (curves down) - arc from 0 to 180
    draw_arc(layer, x - bump_width, y, bump_size, 0, 180, line_width, face_color);

    // Right bump (curves down) - arc from 0 to 180
    draw_arc(layer, x + bump_width, y, bump_size, 0, 180, line_width, face_color);

    // Whiskers
    int whisker_length = (int)(40 * SCALE_X);
    int whisker_start_x = (int)(55 * SCALE_X);
    int whisker_y = s_renderer.eye_y + (int)(45 * SCALE_Y) + offset_y;

    int angles[] = {-15, 0, 15};
    for (int i = 0; i < 3; i++) {
        float angle_rad = angles[i] * 3.14159f / 180.0f;
        int end_y_offset = (int)(sinf(angle_rad) * whisker_length);
        int y_spacing = (i - 1) * (int)(8 * SCALE_Y);

        // Left whiskers
        draw_line(layer,
                  x - whisker_start_x, whisker_y + y_spacing,
                  x - whisker_start_x - whisker_length, whisker_y + y_spacing + end_y_offset,
                  (int)(3 * SCALE_Y), face_color);

        // Right whiskers
        draw_line(layer,
                  x + whisker_start_x, whisker_y + y_spacing,
                  x + whisker_start_x + whisker_length, whisker_y + y_spacing + end_y_offset,
                  (int)(3 * SCALE_Y), face_color);
    }
}

// Draw angry eyebrows
static void draw_angry_brows(lv_layer_t *layer, int offset_x, int offset_y)
{
    int brow_y = s_renderer.eye_y - (int)(35 * SCALE_Y) + offset_y;
    int brow_length = (int)(30 * SCALE_X);
    int left_eye_x = s_renderer.left_eye_x + offset_x;
    int right_eye_x = s_renderer.right_eye_x + offset_x;

    lv_color_t face_color = make_color(FACE_COLOR_R, FACE_COLOR_G, FACE_COLOR_B);
    int line_width = (int)(5 * SCALE_Y);

    // Left brow - angled down toward center
    draw_line(layer,
              left_eye_x - brow_length, brow_y - (int)(10 * SCALE_Y),
              left_eye_x + (int)(10 * SCALE_X), brow_y + (int)(5 * SCALE_Y),
              line_width, face_color);

    // Right brow - angled down toward center
    draw_line(layer,
              right_eye_x - (int)(10 * SCALE_X), brow_y + (int)(5 * SCALE_Y),
              right_eye_x + brow_length, brow_y - (int)(10 * SCALE_Y),
              line_width, face_color);
}

// Draw the complete face
static void draw_face(void)
{
    if (!s_renderer.canvas) return;

    lv_layer_t layer;
    lv_canvas_init_layer(s_renderer.canvas, &layer);

    // Clear canvas with background color
    lv_canvas_fill_bg(s_renderer.canvas, make_color(BG_COLOR_R, BG_COLOR_G, BG_COLOR_B), LV_OPA_COVER);

    emotion_config_t *params = &s_renderer.current_params;

    // Apply face offset for edge tracking
    int offset_x = (int)s_renderer.face_offset_x;
    int offset_y = (int)s_renderer.face_offset_y;

    int left_eye_x = s_renderer.left_eye_x + offset_x;
    int right_eye_x = s_renderer.right_eye_x + offset_x;
    int eye_y = s_renderer.eye_y + offset_y;

    // Draw eyes
    draw_robot_eye(&layer, left_eye_x, eye_y, false);
    draw_robot_eye(&layer, right_eye_x, eye_y, true);

    // Draw angry brows if needed
    if (params->angry_brows) {
        draw_angry_brows(&layer, offset_x, offset_y);
    }

    // Draw mouth
    if (params->cat_face) {
        draw_cat_mouth(&layer, offset_x, offset_y);
    } else {
        draw_robot_mouth(&layer, offset_x, offset_y);
    }

    lv_canvas_finish_layer(s_renderer.canvas, &layer);
}

// Render task - runs on core 1
static void render_task_func(void *pvParameters)
{
    ESP_LOGI(TAG, "Render task started on core %d", xPortGetCoreID());

    s_renderer.last_anim_time = esp_timer_get_time();
    s_renderer.last_fps_time = esp_timer_get_time();

    while (s_renderer.running) {
        // Calculate delta time
        int64_t current_time = esp_timer_get_time();
        float delta_time = (current_time - s_renderer.last_anim_time) / 1000000.0f;
        s_renderer.last_anim_time = current_time;

        // Clamp delta time
        if (delta_time > 0.1f) delta_time = 0.1f;
        if (delta_time < 0.001f) delta_time = 0.001f;

        // Update FPS tracking
        s_renderer.frame_count++;
        if (current_time - s_renderer.last_fps_time > 1000000) {
            s_renderer.actual_fps = s_renderer.frame_count * 1000000.0f /
                                     (current_time - s_renderer.last_fps_time);
            s_renderer.frame_count = 0;
            s_renderer.last_fps_time = current_time;
        }

        // Take mutex
        if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (s_renderer.mode == DISPLAY_MODE_FACE) {
                // Update animation state
                update_animation(delta_time);

                // Draw face to canvas
                if (bsp_display_lock(pdMS_TO_TICKS(50))) {
                    draw_face();
                    bsp_display_unlock();
                }
            }
            xSemaphoreGive(s_renderer.mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(ANIMATION_PERIOD_MS));
    }

    ESP_LOGI(TAG, "Render task stopped");
    vTaskDelete(NULL);
}

static void update_animation(float delta_time)
{
    // Update emotion transition
    if (s_renderer.emotion_transition < 1.0f) {
        s_renderer.emotion_transition += delta_time * EMOTION_TRANSITION_SPEED;
        if (s_renderer.emotion_transition >= 1.0f) {
            s_renderer.emotion_transition = 1.0f;
            s_renderer.current_emotion = s_renderer.target_emotion;
        }

        // Interpolate emotion parameters
        const emotion_config_t *from = emotion_get_config(s_renderer.current_emotion);
        const emotion_config_t *to = emotion_get_config(s_renderer.target_emotion);
        emotion_interpolate(from, to, s_renderer.emotion_transition, &s_renderer.current_params);
    }

    // Update gaze (smooth follow)
    s_renderer.gaze_x = lerp(s_renderer.gaze_x, s_renderer.target_gaze_x,
                              delta_time * GAZE_FOLLOW_SPEED);
    s_renderer.gaze_y = lerp(s_renderer.gaze_y, s_renderer.target_gaze_y,
                              delta_time * GAZE_FOLLOW_SPEED);

    // Calculate face offset for edge tracking
    float target_offset_x = 0.0f;
    float target_offset_y = 0.0f;
    float edge_threshold = 0.25f;
    float max_face_shift_x = 25.0f * SCALE_X;
    float max_face_shift_y = 15.0f * SCALE_Y;

    if (s_renderer.gaze_x < edge_threshold) {
        float edge_factor = (edge_threshold - s_renderer.gaze_x) / edge_threshold;
        target_offset_x = -max_face_shift_x * edge_factor;
    } else if (s_renderer.gaze_x > (1.0f - edge_threshold)) {
        float edge_factor = (s_renderer.gaze_x - (1.0f - edge_threshold)) / edge_threshold;
        target_offset_x = max_face_shift_x * edge_factor;
    }

    if (s_renderer.gaze_y < edge_threshold) {
        float edge_factor = (edge_threshold - s_renderer.gaze_y) / edge_threshold;
        target_offset_y = -max_face_shift_y * edge_factor;
    } else if (s_renderer.gaze_y > (1.0f - edge_threshold)) {
        float edge_factor = (s_renderer.gaze_y - (1.0f - edge_threshold)) / edge_threshold;
        target_offset_y = max_face_shift_y * edge_factor;
    }

    s_renderer.face_offset_x = lerp(s_renderer.face_offset_x, target_offset_x,
                                     delta_time * FACE_SHIFT_SPEED);
    s_renderer.face_offset_y = lerp(s_renderer.face_offset_y, target_offset_y,
                                     delta_time * FACE_SHIFT_SPEED);

    // Update blink
    int64_t current_time = esp_timer_get_time() / 1000;

    if (!s_renderer.is_blinking) {
        if (current_time - s_renderer.last_blink_time > s_renderer.blink_interval_ms) {
            s_renderer.is_blinking = true;
            s_renderer.blink_progress = 0.0f;
            s_renderer.blink_interval_ms = random_range(BLINK_MIN_INTERVAL_MS, BLINK_MAX_INTERVAL_MS);
            s_renderer.last_blink_time = current_time;
        }
    }

    if (s_renderer.is_blinking) {
        s_renderer.blink_progress += delta_time * BLINK_SPEED;
        if (s_renderer.blink_progress >= 1.0f) {
            s_renderer.is_blinking = false;
            s_renderer.blink_progress = 0.0f;
        }
    }
}

static float get_blink_factor(void)
{
    if (!s_renderer.is_blinking) {
        return 0.0f;
    }
    if (s_renderer.blink_progress < 0.3f) {
        return s_renderer.blink_progress / 0.3f;
    } else {
        return 1.0f - (s_renderer.blink_progress - 0.3f) / 0.7f;
    }
}

// Public API implementation

esp_err_t face_renderer_init(const face_renderer_config_t *config)
{
    if (s_renderer.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing face renderer (canvas-based, landscape)...");

    // Set dimensions
    s_renderer.width = DISPLAY_WIDTH;
    s_renderer.height = DISPLAY_HEIGHT;
    s_renderer.cat_mode = config ? config->cat_mode : false;

    // Calculate face geometry for landscape
    s_renderer.center_x = s_renderer.width / 2;
    s_renderer.center_y = s_renderer.height / 2 - (int)(50 * SCALE_Y);
    s_renderer.eye_spacing = (int)(50 * SCALE_X);
    s_renderer.left_eye_x = s_renderer.center_x - s_renderer.eye_spacing;
    s_renderer.right_eye_x = s_renderer.center_x + s_renderer.eye_spacing;
    s_renderer.eye_y = s_renderer.center_y - (int)(10 * SCALE_Y);
    s_renderer.mouth_y = s_renderer.center_y + (int)(50 * SCALE_Y);

    // Initialize LVGL display
    s_renderer.display = bsp_display_start();
    if (s_renderer.display == NULL) {
        ESP_LOGE(TAG, "Failed to start display");
        return ESP_FAIL;
    }

    // Rotate display for landscape mode
    ESP_LOGI(TAG, "Rotating display 270 degrees...");
    bsp_display_rotate(s_renderer.display, LV_DISPLAY_ROTATION_270);

    bsp_display_backlight_on();

    // Allocate canvas buffer in PSRAM
    size_t buf_size = LV_DRAW_BUF_SIZE(CANVAS_WIDTH, CANVAS_HEIGHT, LV_COLOR_FORMAT_RGB565);
    s_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (s_canvas_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer in PSRAM (%d bytes)", buf_size);
        // Try DRAM as fallback
        s_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
        if (s_canvas_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate canvas buffer");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "Using DRAM for canvas buffer");
    } else {
        ESP_LOGI(TAG, "Canvas buffer allocated in PSRAM (%d bytes)", buf_size);
    }

    // Create canvas
    bsp_display_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, make_color(BG_COLOR_R, BG_COLOR_G, BG_COLOR_B), 0);

    s_renderer.canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_renderer.canvas, s_canvas_buf, CANVAS_WIDTH, CANVAS_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_renderer.canvas);
    lv_canvas_fill_bg(s_renderer.canvas, make_color(BG_COLOR_R, BG_COLOR_G, BG_COLOR_B), LV_OPA_COVER);

    // Create text label (for text mode)
    s_renderer.text_label = lv_label_create(scr);
    lv_obj_set_width(s_renderer.text_label, s_renderer.width - 24);
    lv_label_set_long_mode(s_renderer.text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_renderer.text_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_renderer.text_label);
    lv_obj_add_flag(s_renderer.text_label, LV_OBJ_FLAG_HIDDEN);

    bsp_display_unlock();

    // Initialize state
    s_renderer.mode = DISPLAY_MODE_FACE;
    s_renderer.current_emotion = EMOTION_NEUTRAL;
    s_renderer.target_emotion = EMOTION_NEUTRAL;
    s_renderer.emotion_transition = 1.0f;
    s_renderer.current_params = *emotion_get_config(EMOTION_NEUTRAL);

    s_renderer.gaze_x = 0.5f;
    s_renderer.gaze_y = 0.5f;
    s_renderer.target_gaze_x = 0.5f;
    s_renderer.target_gaze_y = 0.5f;

    s_renderer.blink_progress = 0.0f;
    s_renderer.is_blinking = false;
    s_renderer.last_blink_time = esp_timer_get_time() / 1000;
    s_renderer.blink_interval_ms = random_range(BLINK_MIN_INTERVAL_MS, BLINK_MAX_INTERVAL_MS);

    // Create mutex
    s_renderer.mutex = xSemaphoreCreateMutex();
    if (s_renderer.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_renderer.initialized = true;
    ESP_LOGI(TAG, "Face renderer initialized (%dx%d, canvas-based, landscape)",
             s_renderer.width, s_renderer.height);

    return ESP_OK;
}

esp_err_t face_renderer_deinit(void)
{
    if (!s_renderer.initialized) {
        return ESP_OK;
    }

    face_renderer_stop();

    // Free canvas buffer
    if (s_canvas_buf) {
        heap_caps_free(s_canvas_buf);
        s_canvas_buf = NULL;
    }

    if (s_renderer.mutex) {
        vSemaphoreDelete(s_renderer.mutex);
        s_renderer.mutex = NULL;
    }

    s_renderer.initialized = false;
    ESP_LOGI(TAG, "Face renderer deinitialized");

    return ESP_OK;
}

esp_err_t face_renderer_start(void)
{
    if (!s_renderer.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_renderer.running) {
        return ESP_OK;
    }

    s_renderer.running = true;
    s_renderer.frame_count = 0;

    // Create FreeRTOS task on core 1
    BaseType_t ret = xTaskCreatePinnedToCore(
        render_task_func,
        "face_render",
        RENDER_TASK_STACK_SIZE,
        NULL,
        RENDER_TASK_PRIORITY,
        &s_renderer.render_task,
        RENDER_TASK_CORE
    );

    if (ret != pdPASS) {
        s_renderer.running = false;
        ESP_LOGE(TAG, "Failed to create render task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Face renderer started (canvas-based on core %d)", RENDER_TASK_CORE);
    return ESP_OK;
}

esp_err_t face_renderer_stop(void)
{
    if (!s_renderer.running) {
        return ESP_OK;
    }

    s_renderer.running = false;

    // Wait for render task to finish
    if (s_renderer.render_task) {
        vTaskDelay(pdMS_TO_TICKS(ANIMATION_PERIOD_MS * 2));
        s_renderer.render_task = NULL;
    }

    ESP_LOGI(TAG, "Face renderer stopped");
    return ESP_OK;
}

void face_renderer_set_emotion(emotion_id_t emotion)
{
    if (!s_renderer.initialized || emotion >= EMOTION_COUNT) {
        return;
    }

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_renderer.target_emotion = emotion;
        s_renderer.emotion_transition = 0.0f;
        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Emotion set to: %s", emotion_to_string(emotion));
    }
}

void face_renderer_set_emotion_str(const char *name)
{
    face_renderer_set_emotion(emotion_from_string(name));
}

emotion_id_t face_renderer_get_emotion(void)
{
    return s_renderer.current_emotion;
}

void face_renderer_set_gaze(float x, float y)
{
    if (!s_renderer.initialized) {
        return;
    }

    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    if (y < 0.0f) y = 0.0f;
    if (y > 1.0f) y = 1.0f;

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_renderer.target_gaze_x = x;
        s_renderer.target_gaze_y = y;
        xSemaphoreGive(s_renderer.mutex);
    }
}

void face_renderer_get_gaze(float *x, float *y)
{
    if (x) *x = s_renderer.gaze_x;
    if (y) *y = s_renderer.gaze_y;
}

void face_renderer_set_cat_mode(bool enabled)
{
    s_renderer.cat_mode = enabled;
    if (enabled) {
        face_renderer_set_emotion(EMOTION_CAT);
    }
}

bool face_renderer_is_cat_mode(void)
{
    return s_renderer.cat_mode;
}

void face_renderer_blink(void)
{
    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_renderer.is_blinking = true;
        s_renderer.blink_progress = 0.0f;
        xSemaphoreGive(s_renderer.mutex);
    }
}

void face_renderer_show_text(const char *text, font_size_t size,
                              uint32_t color, uint32_t bg_color)
{
    if (!s_renderer.initialized || text == NULL) {
        return;
    }

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(s_renderer.text_content, text, MAX_TEXT_LENGTH - 1);
        s_renderer.text_content[MAX_TEXT_LENGTH - 1] = '\0';
        s_renderer.text_size = size;
        s_renderer.text_color = color;
        s_renderer.text_bg_color = bg_color;
        s_renderer.mode = DISPLAY_MODE_TEXT;

        // Update UI
        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            // Hide canvas
            lv_obj_add_flag(s_renderer.canvas, LV_OBJ_FLAG_HIDDEN);

            // Update screen background
            lv_obj_t *scr = lv_scr_act();
            lv_obj_set_style_bg_color(scr, make_color((bg_color >> 16) & 0xFF,
                                                       (bg_color >> 8) & 0xFF,
                                                       bg_color & 0xFF), 0);

            // Show and update text label
            lv_label_set_text(s_renderer.text_label, text);
            lv_obj_set_style_text_color(s_renderer.text_label,
                                        make_color((color >> 16) & 0xFF,
                                                   (color >> 8) & 0xFF,
                                                   color & 0xFF), 0);
            lv_obj_remove_flag(s_renderer.text_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_center(s_renderer.text_label);

            bsp_display_unlock();
        }

        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Text display: '%s'", text);
    }
}

void face_renderer_clear_text(void)
{
    if (!s_renderer.initialized) {
        return;
    }

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_renderer.text_content[0] = '\0';
        s_renderer.mode = DISPLAY_MODE_FACE;

        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            // Hide text label
            lv_obj_add_flag(s_renderer.text_label, LV_OBJ_FLAG_HIDDEN);

            // Restore background
            lv_obj_t *scr = lv_scr_act();
            lv_obj_set_style_bg_color(scr, make_color(BG_COLOR_R, BG_COLOR_G, BG_COLOR_B), 0);

            // Show canvas
            lv_obj_remove_flag(s_renderer.canvas, LV_OBJ_FLAG_HIDDEN);

            bsp_display_unlock();
        }

        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Text cleared");
    }
}

void face_renderer_show_pixel_art(const uint32_t *pixels, size_t count,
                                   uint32_t bg_color)
{
    // TODO: Implement pixel art with canvas
    ESP_LOGW(TAG, "Pixel art not yet implemented in canvas mode");
}

void face_renderer_clear_pixel_art(void)
{
    if (!s_renderer.initialized) {
        return;
    }

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_renderer.mode = DISPLAY_MODE_FACE;

        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            // Show canvas
            lv_obj_remove_flag(s_renderer.canvas, LV_OBJ_FLAG_HIDDEN);
            bsp_display_unlock();
        }

        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Pixel art cleared");
    }
}

display_mode_t face_renderer_get_mode(void)
{
    return s_renderer.mode;
}

float face_renderer_get_fps(void)
{
    return s_renderer.actual_fps;
}
