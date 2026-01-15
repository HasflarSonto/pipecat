/*
 * Luna Face Renderer for ESP32
 * Widget-based rendering engine using LVGL
 * Ported from luna_face_renderer.py
 *
 * Uses LVGL widgets (not canvas) for efficient dirty rectangle updates.
 * Only changed areas are sent to display, avoiding SPI overflow.
 * Rotated 90° clockwise for landscape mode.
 */

#include "face_renderer.h"
#include "emotions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
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
#define DEFAULT_WIDTH       502
#define DEFAULT_HEIGHT      410

// Colors
#define BG_COLOR            0x1E1E28    // Dark background
#define FACE_COLOR          0xFFFFFF    // White for features

// Animation timing - SLOWER to avoid SPI overflow
#define ANIMATION_PERIOD_MS    66       // ~15 FPS (was 50ms/20FPS)
#define RENDER_TASK_STACK_SIZE (8 * 1024)
#define RENDER_TASK_PRIORITY   3
#define RENDER_TASK_CORE       1

#define EMOTION_TRANSITION_SPEED    2.5f  // Slower transitions
#define GAZE_FOLLOW_SPEED          8.0f
#define BLINK_SPEED                10.0f
#define FACE_SHIFT_SPEED           5.0f

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

// Minimum change thresholds to reduce unnecessary updates
#define MIN_EYE_CHANGE             2.0f
#define MIN_GAZE_CHANGE            0.02f
#define MIN_MOUTH_CHANGE           2.0f

// Renderer state
static struct {
    // Configuration
    uint16_t width;
    uint16_t height;

    // Display
    lv_display_t *display;

    // LVGL objects for face
    lv_obj_t *left_eye;
    lv_obj_t *right_eye;
    lv_obj_t *mouth_arc;
    lv_obj_t *mouth_line;
    lv_obj_t *left_brow;
    lv_obj_t *right_brow;
    lv_obj_t *left_sparkle;
    lv_obj_t *right_sparkle;

    // Text label
    lv_obj_t *text_label;

    // Pixel art objects
    lv_obj_t **pixel_objs;
    size_t pixel_obj_count;

    // Face geometry
    int center_x;
    int center_y;
    int eye_spacing;
    int left_eye_base_x;
    int right_eye_base_x;
    int eye_base_y;
    int mouth_base_y;

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

    // Last rendered values (to detect changes)
    int last_eye_x;
    int last_eye_y;
    int last_eye_w;
    int last_eye_h;
    int last_mouth_curve;
    bool last_angry_brows;
    bool last_sparkle;

    // Full screen invalidation counter (to clear artifacts)
    int invalidate_counter;

    // Mutex for thread safety
    SemaphoreHandle_t mutex;
} s_renderer = {0};

// Forward declarations
static void render_task_func(void *pvParameters);
static void update_animation(float delta_time);
static void update_face_widgets(void);
static float get_blink_factor(void);

static float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

static lv_color_t rgb888_to_lv(uint32_t rgb)
{
    return lv_color_make((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static int random_range(int min_val, int max_val)
{
    return min_val + (esp_random() % (max_val - min_val + 1));
}

// Create face widget objects
static void create_face_widgets(lv_obj_t *parent)
{
    lv_color_t face_color = rgb888_to_lv(FACE_COLOR);
    lv_color_t bg_color = rgb888_to_lv(BG_COLOR);

    // Left eye
    s_renderer.left_eye = lv_obj_create(parent);
    lv_obj_remove_style_all(s_renderer.left_eye);
    lv_obj_set_style_bg_color(s_renderer.left_eye, face_color, 0);
    lv_obj_set_style_bg_opa(s_renderer.left_eye, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_renderer.left_eye, 15, 0);
    lv_obj_set_style_border_width(s_renderer.left_eye, 0, 0);

    // Right eye
    s_renderer.right_eye = lv_obj_create(parent);
    lv_obj_remove_style_all(s_renderer.right_eye);
    lv_obj_set_style_bg_color(s_renderer.right_eye, face_color, 0);
    lv_obj_set_style_bg_opa(s_renderer.right_eye, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_renderer.right_eye, 15, 0);
    lv_obj_set_style_border_width(s_renderer.right_eye, 0, 0);

    // Mouth arc (for smile/frown)
    s_renderer.mouth_arc = lv_arc_create(parent);
    lv_obj_remove_style_all(s_renderer.mouth_arc);
    // Hide background arc
    lv_obj_set_style_arc_width(s_renderer.mouth_arc, 0, LV_PART_MAIN);
    // Style the indicator arc (the visible part)
    lv_obj_set_style_arc_color(s_renderer.mouth_arc, face_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_renderer.mouth_arc, (int)(6 * SCALE_Y), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_renderer.mouth_arc, true, LV_PART_INDICATOR);
    // Hide knob
    lv_obj_set_style_pad_all(s_renderer.mouth_arc, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_renderer.mouth_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    // Set arc mode to normal (not rotated)
    lv_arc_set_mode(s_renderer.mouth_arc, LV_ARC_MODE_NORMAL);
    lv_obj_add_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);

    // Mouth line (for neutral/straight)
    s_renderer.mouth_line = lv_obj_create(parent);
    lv_obj_remove_style_all(s_renderer.mouth_line);
    lv_obj_set_style_bg_color(s_renderer.mouth_line, face_color, 0);
    lv_obj_set_style_bg_opa(s_renderer.mouth_line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_renderer.mouth_line, 3, 0);
    lv_obj_add_flag(s_renderer.mouth_line, LV_OBJ_FLAG_HIDDEN);

    // Left brow (for angry)
    s_renderer.left_brow = lv_obj_create(parent);
    lv_obj_remove_style_all(s_renderer.left_brow);
    lv_obj_set_style_bg_color(s_renderer.left_brow, face_color, 0);
    lv_obj_set_style_bg_opa(s_renderer.left_brow, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_renderer.left_brow, 2, 0);
    lv_obj_add_flag(s_renderer.left_brow, LV_OBJ_FLAG_HIDDEN);

    // Right brow (for angry)
    s_renderer.right_brow = lv_obj_create(parent);
    lv_obj_remove_style_all(s_renderer.right_brow);
    lv_obj_set_style_bg_color(s_renderer.right_brow, face_color, 0);
    lv_obj_set_style_bg_opa(s_renderer.right_brow, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_renderer.right_brow, 2, 0);
    lv_obj_add_flag(s_renderer.right_brow, LV_OBJ_FLAG_HIDDEN);

    // Left sparkle (for excited)
    s_renderer.left_sparkle = lv_obj_create(parent);
    lv_obj_remove_style_all(s_renderer.left_sparkle);
    lv_obj_set_style_bg_color(s_renderer.left_sparkle, bg_color, 0);
    lv_obj_set_style_bg_opa(s_renderer.left_sparkle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_renderer.left_sparkle, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(s_renderer.left_sparkle, LV_OBJ_FLAG_HIDDEN);

    // Right sparkle (for excited)
    s_renderer.right_sparkle = lv_obj_create(parent);
    lv_obj_remove_style_all(s_renderer.right_sparkle);
    lv_obj_set_style_bg_color(s_renderer.right_sparkle, bg_color, 0);
    lv_obj_set_style_bg_opa(s_renderer.right_sparkle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_renderer.right_sparkle, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(s_renderer.right_sparkle, LV_OBJ_FLAG_HIDDEN);
}

// Update face widget positions and sizes
static void update_face_widgets(void)
{
    emotion_config_t *params = &s_renderer.current_params;

    // Calculate blink factor
    float blink_factor = get_blink_factor();

    // Calculate eye dimensions
    float eye_height = params->eye_height * SCALE_Y;
    float eye_width = params->eye_width * SCALE_X;
    eye_height *= params->eye_openness;
    eye_height *= (1.0f - blink_factor * 0.95f);
    if (eye_height < 5) eye_height = 5;

    // Gaze offset
    float gaze_range_x = 28.0f * SCALE_X;
    float gaze_range_y = 18.0f * SCALE_Y;
    int gaze_x_offset = (int)((s_renderer.gaze_x - 0.5f) * 2.0f * gaze_range_x);
    int gaze_y_offset = (int)((s_renderer.gaze_y - 0.5f) * 2.0f * gaze_range_y);

    // Face offset
    int offset_x = (int)s_renderer.face_offset_x;
    int offset_y = (int)s_renderer.face_offset_y;

    // Think offset (look to side)
    int think_offset = params->look_side ? (int)(12.0f * SCALE_X) : 0;

    // Tilt for confused
    int left_tilt = params->tilt_eyes ? (int)(-8.0f * SCALE_Y) : 0;
    int right_tilt = params->tilt_eyes ? (int)(8.0f * SCALE_Y) : 0;

    int eye_w = (int)eye_width;
    int eye_h = (int)eye_height;

    // Calculate corner radius
    int radius = (eye_h < eye_w) ? eye_h / 2 : eye_w / 2;
    int max_radius = (int)(15.0f * SCALE_Y);
    if (radius > max_radius) radius = max_radius;

    // Calculate eye positions
    int left_eye_x = s_renderer.left_eye_base_x + offset_x + gaze_x_offset + think_offset - eye_w/2;
    int left_eye_y = s_renderer.eye_base_y + offset_y + gaze_y_offset + left_tilt - eye_h/2;
    int right_eye_x = s_renderer.right_eye_base_x + offset_x + gaze_x_offset + think_offset - eye_w/2;
    int right_eye_y = s_renderer.eye_base_y + offset_y + gaze_y_offset + right_tilt - eye_h/2;

    // Check if significant change
    bool eye_changed = (abs(left_eye_x - s_renderer.last_eye_x) > MIN_EYE_CHANGE ||
                        abs(left_eye_y - s_renderer.last_eye_y) > MIN_EYE_CHANGE ||
                        abs(eye_w - s_renderer.last_eye_w) > MIN_EYE_CHANGE ||
                        abs(eye_h - s_renderer.last_eye_h) > MIN_EYE_CHANGE);

    if (eye_changed) {
        // Update left eye
        lv_obj_set_pos(s_renderer.left_eye, left_eye_x, left_eye_y);
        lv_obj_set_size(s_renderer.left_eye, eye_w, eye_h);
        lv_obj_set_style_radius(s_renderer.left_eye, radius, 0);

        // Update right eye
        lv_obj_set_pos(s_renderer.right_eye, right_eye_x, right_eye_y);
        lv_obj_set_size(s_renderer.right_eye, eye_w, eye_h);
        lv_obj_set_style_radius(s_renderer.right_eye, radius, 0);

        s_renderer.last_eye_x = left_eye_x;
        s_renderer.last_eye_y = left_eye_y;
        s_renderer.last_eye_w = eye_w;
        s_renderer.last_eye_h = eye_h;
    }

    // Update sparkles
    if (params->sparkle != s_renderer.last_sparkle) {
        // Invalidate before hiding to prevent artifacts
        lv_obj_invalidate(s_renderer.left_sparkle);
        lv_obj_invalidate(s_renderer.right_sparkle);

        if (params->sparkle) {
            int sparkle_size = (int)(10 * SCALE_X);  // Slightly larger
            lv_obj_set_size(s_renderer.left_sparkle, sparkle_size, sparkle_size);
            lv_obj_set_pos(s_renderer.left_sparkle, left_eye_x + eye_w/4, left_eye_y + eye_h/4);
            lv_obj_remove_flag(s_renderer.left_sparkle, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_size(s_renderer.right_sparkle, sparkle_size, sparkle_size);
            lv_obj_set_pos(s_renderer.right_sparkle, right_eye_x + eye_w/4, right_eye_y + eye_h/4);
            lv_obj_remove_flag(s_renderer.right_sparkle, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_renderer.left_sparkle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.right_sparkle, LV_OBJ_FLAG_HIDDEN);
        }
        s_renderer.last_sparkle = params->sparkle;
    } else if (params->sparkle && eye_changed) {
        // Update sparkle positions if eyes moved
        lv_obj_set_pos(s_renderer.left_sparkle, left_eye_x + eye_w/4, left_eye_y + eye_h/4);
        lv_obj_set_pos(s_renderer.right_sparkle, right_eye_x + eye_w/4, right_eye_y + eye_h/4);
    }

    // Mouth position
    int mouth_x = s_renderer.center_x + offset_x;
    int mouth_y = s_renderer.mouth_base_y + offset_y;
    int mouth_width = (int)(params->mouth_width * SCALE_X);
    int line_width = (int)(6 * SCALE_Y);

    // Calculate mouth curve category
    int curve_category;
    if (params->cat_face) {
        curve_category = 100;  // Cat mode
    } else if (params->mouth_open > 0.3f) {
        curve_category = 50;   // O mouth
    } else if (fabsf(params->mouth_curve) < 0.1f) {
        curve_category = 0;    // Straight
    } else if (params->mouth_curve > 0) {
        curve_category = 1;    // Smile
    } else {
        curve_category = -1;   // Frown
    }

    if (curve_category != s_renderer.last_mouth_curve) {
        // Invalidate old mouth areas before hiding (prevents artifacts)
        lv_obj_invalidate(s_renderer.mouth_arc);
        lv_obj_invalidate(s_renderer.mouth_line);

        // Hide all mouth objects first
        lv_obj_add_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_renderer.mouth_line, LV_OBJ_FLAG_HIDDEN);

        // Mouth sizes increased for better visibility
        int arc_size = (int)(mouth_width * 2.5f);  // Increased from 2x

        if (curve_category == 100) {
            // Cat mouth - very wide, shallow smile (cat-like :3)
            int cat_width = (int)(mouth_width * 3.5f);  // Extra wide
            int cat_height = (int)(mouth_width * 0.8f); // Very shallow
            lv_arc_set_range(s_renderer.mouth_arc, 0, 100);
            lv_arc_set_value(s_renderer.mouth_arc, 100);
            lv_arc_set_bg_angles(s_renderer.mouth_arc, 0, 180);
            lv_arc_set_angles(s_renderer.mouth_arc, 0, 180);
            lv_obj_set_size(s_renderer.mouth_arc, cat_width, cat_height);
            lv_obj_set_pos(s_renderer.mouth_arc, mouth_x - cat_width/2, mouth_y - cat_height/4);
            lv_obj_remove_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
        } else if (curve_category == 50) {
            // O-shaped surprised mouth - full circle
            int o_size = (int)((40.0f + params->mouth_open * 40.0f) * SCALE_X);
            lv_arc_set_range(s_renderer.mouth_arc, 0, 100);
            lv_arc_set_value(s_renderer.mouth_arc, 100);
            lv_arc_set_bg_angles(s_renderer.mouth_arc, 0, 360);
            lv_arc_set_angles(s_renderer.mouth_arc, 0, 360);
            lv_obj_set_size(s_renderer.mouth_arc, o_size, o_size);
            lv_obj_set_pos(s_renderer.mouth_arc, mouth_x - o_size/2, mouth_y - o_size/2);
            lv_obj_remove_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
        } else if (curve_category == 0) {
            // Straight line mouth - wider and thicker for visibility
            int line_len = (int)(mouth_width * 3.0f);  // Wider
            int thick_line = line_width + 2;           // Slightly thicker
            lv_obj_set_size(s_renderer.mouth_line, line_len, thick_line);
            lv_obj_set_pos(s_renderer.mouth_line, mouth_x - line_len/2, mouth_y - thick_line/2);
            lv_obj_remove_flag(s_renderer.mouth_line, LV_OBJ_FLAG_HIDDEN);
        } else if (curve_category == 1) {
            // Smile - arc from 0 to 180 (bottom half), bigger
            int curve_amount = (int)(fabsf(params->mouth_curve) * 60.0f * SCALE_Y);
            if (curve_amount < 40) curve_amount = 40;
            lv_arc_set_range(s_renderer.mouth_arc, 0, 100);
            lv_arc_set_value(s_renderer.mouth_arc, 100);
            lv_arc_set_bg_angles(s_renderer.mouth_arc, 0, 180);
            lv_arc_set_angles(s_renderer.mouth_arc, 0, 180);
            lv_obj_set_size(s_renderer.mouth_arc, arc_size, curve_amount);
            lv_obj_set_pos(s_renderer.mouth_arc, mouth_x - arc_size/2, mouth_y - curve_amount/4);
            lv_obj_remove_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Frown - arc from 180 to 360 (top half), bigger
            int curve_amount = (int)(fabsf(params->mouth_curve) * 60.0f * SCALE_Y);
            if (curve_amount < 40) curve_amount = 40;
            lv_arc_set_range(s_renderer.mouth_arc, 0, 100);
            lv_arc_set_value(s_renderer.mouth_arc, 100);
            lv_arc_set_bg_angles(s_renderer.mouth_arc, 180, 360);
            lv_arc_set_angles(s_renderer.mouth_arc, 180, 360);
            lv_obj_set_size(s_renderer.mouth_arc, arc_size, curve_amount);
            lv_obj_set_pos(s_renderer.mouth_arc, mouth_x - arc_size/2, mouth_y - curve_amount/2);
            lv_obj_remove_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
        }

        s_renderer.last_mouth_curve = curve_category;
    }

    // Update angry brows
    if (params->angry_brows != s_renderer.last_angry_brows) {
        // Invalidate before hiding to prevent artifacts
        lv_obj_invalidate(s_renderer.left_brow);
        lv_obj_invalidate(s_renderer.right_brow);

        if (params->angry_brows) {
            int brow_y = s_renderer.eye_base_y - (int)(40.0f * SCALE_Y) + offset_y;
            int brow_length = (int)(35.0f * SCALE_X);
            int brow_width = (int)(5 * SCALE_Y);
            int left_x = s_renderer.left_eye_base_x + offset_x;
            int right_x = s_renderer.right_eye_base_x + offset_x;

            // Left brow (simple rectangle for now - angled brows would need line widget)
            lv_obj_set_size(s_renderer.left_brow, brow_length, brow_width);
            lv_obj_set_pos(s_renderer.left_brow, left_x - brow_length/2, brow_y);
            lv_obj_remove_flag(s_renderer.left_brow, LV_OBJ_FLAG_HIDDEN);

            // Right brow
            lv_obj_set_size(s_renderer.right_brow, brow_length, brow_width);
            lv_obj_set_pos(s_renderer.right_brow, right_x - brow_length/2, brow_y);
            lv_obj_remove_flag(s_renderer.right_brow, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_renderer.left_brow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.right_brow, LV_OBJ_FLAG_HIDDEN);
        }
        s_renderer.last_angry_brows = params->angry_brows;
    }
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

                // Update face widgets
                if (bsp_display_lock(pdMS_TO_TICKS(50))) {
                    update_face_widgets();

                    // Periodic full-screen invalidation to clear artifacts
                    // Every ~2 seconds (30 frames at 15fps)
                    s_renderer.invalidate_counter++;
                    if (s_renderer.invalidate_counter >= 30) {
                        s_renderer.invalidate_counter = 0;
                        lv_obj_t *scr = lv_scr_act();
                        lv_obj_invalidate(scr);
                    }

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

// Clear pixel art objects
static void clear_pixel_objects(void)
{
    if (s_renderer.pixel_objs) {
        for (size_t i = 0; i < s_renderer.pixel_obj_count; i++) {
            if (s_renderer.pixel_objs[i]) {
                lv_obj_delete(s_renderer.pixel_objs[i]);
            }
        }
        free(s_renderer.pixel_objs);
        s_renderer.pixel_objs = NULL;
        s_renderer.pixel_obj_count = 0;
    }
}

// Public API implementation

esp_err_t face_renderer_init(const face_renderer_config_t *config)
{
    if (s_renderer.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing face renderer (widget-based, landscape)...");

    // Set dimensions for landscape mode
    s_renderer.width = DEFAULT_WIDTH;   // 502
    s_renderer.height = DEFAULT_HEIGHT; // 410
    s_renderer.cat_mode = config ? config->cat_mode : false;

    // Calculate face geometry for landscape
    s_renderer.center_x = s_renderer.width / 2;
    s_renderer.center_y = s_renderer.height / 2 - (int)(20 * SCALE_Y);
    s_renderer.eye_spacing = (int)(55 * SCALE_X);
    s_renderer.left_eye_base_x = s_renderer.center_x - s_renderer.eye_spacing;
    s_renderer.right_eye_base_x = s_renderer.center_x + s_renderer.eye_spacing;
    s_renderer.eye_base_y = s_renderer.center_y - (int)(15 * SCALE_Y);
    s_renderer.mouth_base_y = s_renderer.center_y + (int)(55 * SCALE_Y);

    // Initialize LVGL display
    s_renderer.display = bsp_display_start();
    if (s_renderer.display == NULL) {
        ESP_LOGE(TAG, "Failed to start display");
        return ESP_FAIL;
    }

    // Rotate display 270 degrees (or 90 counter-clockwise) for landscape mode
    // This puts the USB port on the left side when viewing
    ESP_LOGI(TAG, "Rotating display 270 degrees...");
    bsp_display_rotate(s_renderer.display, LV_DISPLAY_ROTATION_270);

    bsp_display_backlight_on();

    // Create face widgets
    bsp_display_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, rgb888_to_lv(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    create_face_widgets(scr);

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

    // Initialize last values for change detection
    s_renderer.last_eye_x = -1000;
    s_renderer.last_eye_y = -1000;
    s_renderer.last_eye_w = 0;
    s_renderer.last_eye_h = 0;
    s_renderer.last_mouth_curve = -1000;
    s_renderer.last_angry_brows = false;
    s_renderer.last_sparkle = false;

    // Create mutex
    s_renderer.mutex = xSemaphoreCreateMutex();
    if (s_renderer.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_renderer.initialized = true;
    ESP_LOGI(TAG, "Face renderer initialized (%dx%d, widget-based, landscape)",
             s_renderer.width, s_renderer.height);

    return ESP_OK;
}

esp_err_t face_renderer_deinit(void)
{
    if (!s_renderer.initialized) {
        return ESP_OK;
    }

    face_renderer_stop();

    // Clean up pixel objects
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        clear_pixel_objects();
        bsp_display_unlock();
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

    ESP_LOGI(TAG, "Face renderer started (widget-based on core %d)", RENDER_TASK_CORE);
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
            // Hide face widgets
            lv_obj_add_flag(s_renderer.left_eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.right_eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.mouth_line, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.left_brow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.right_brow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.left_sparkle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.right_sparkle, LV_OBJ_FLAG_HIDDEN);

            // Update screen background
            lv_obj_t *scr = lv_scr_act();
            lv_obj_set_style_bg_color(scr, rgb888_to_lv(bg_color), 0);

            // Show and update text label
            lv_label_set_text(s_renderer.text_label, text);
            lv_obj_set_style_text_color(s_renderer.text_label, rgb888_to_lv(color), 0);
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
            lv_obj_set_style_bg_color(scr, rgb888_to_lv(BG_COLOR), 0);

            // Show face widgets
            lv_obj_remove_flag(s_renderer.left_eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_renderer.right_eye, LV_OBJ_FLAG_HIDDEN);
            // Mouth visibility handled by update_face_widgets based on emotion

            // Force update on next frame
            s_renderer.last_eye_x = -1000;
            s_renderer.last_mouth_curve = -1000;

            bsp_display_unlock();
        }

        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Text cleared");
    }
}

void face_renderer_show_pixel_art(const uint32_t *pixels, size_t count,
                                   uint32_t bg_color)
{
    if (!s_renderer.initialized || pixels == NULL || count == 0) {
        return;
    }

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_renderer.mode = DISPLAY_MODE_PIXEL_ART;

        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            // Hide face widgets
            lv_obj_add_flag(s_renderer.left_eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.right_eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.mouth_line, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.left_brow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.right_brow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.left_sparkle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.right_sparkle, LV_OBJ_FLAG_HIDDEN);

            // Update background
            lv_obj_t *scr = lv_scr_act();
            lv_obj_set_style_bg_color(scr, rgb888_to_lv(bg_color), 0);

            // Clear old pixel objects
            clear_pixel_objects();

            // Create pixel objects
            int grid_width = PIXEL_GRID_COLS * PIXEL_CELL_SIZE;
            int grid_height = PIXEL_GRID_ROWS * PIXEL_CELL_SIZE;
            int screen_offset_x = (s_renderer.width - grid_width) / 2;
            int screen_offset_y = (s_renderer.height - grid_height) / 2;

            s_renderer.pixel_objs = malloc(count * sizeof(lv_obj_t *));
            if (s_renderer.pixel_objs) {
                s_renderer.pixel_obj_count = count;

                for (size_t i = 0; i < count; i++) {
                    int x = (pixels[i] >> 24) & 0xFF;
                    int y = (pixels[i] >> 16) & 0xFF;
                    uint32_t color = pixels[i] & 0xFFFFFF;

                    if (x >= 0 && x < PIXEL_GRID_COLS && y >= 0 && y < PIXEL_GRID_ROWS) {
                        int screen_x = screen_offset_x + x * PIXEL_CELL_SIZE;
                        int screen_y = screen_offset_y + y * PIXEL_CELL_SIZE;

                        lv_obj_t *obj = lv_obj_create(scr);
                        lv_obj_remove_style_all(obj);
                        lv_obj_set_size(obj, PIXEL_CELL_SIZE, PIXEL_CELL_SIZE);
                        lv_obj_set_pos(obj, screen_x, screen_y);
                        lv_obj_set_style_bg_color(obj, rgb888_to_lv(color), 0);
                        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
                        lv_obj_set_style_border_width(obj, 0, 0);

                        s_renderer.pixel_objs[i] = obj;
                    } else {
                        s_renderer.pixel_objs[i] = NULL;
                    }
                }
            }

            bsp_display_unlock();
        }

        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Pixel art: %d pixels", (int)count);
    }
}

void face_renderer_clear_pixel_art(void)
{
    if (!s_renderer.initialized) {
        return;
    }

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_renderer.mode = DISPLAY_MODE_FACE;

        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            // Clear pixel objects
            clear_pixel_objects();

            // Restore background
            lv_obj_t *scr = lv_scr_act();
            lv_obj_set_style_bg_color(scr, rgb888_to_lv(BG_COLOR), 0);

            // Show face widgets
            lv_obj_remove_flag(s_renderer.left_eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_renderer.right_eye, LV_OBJ_FLAG_HIDDEN);

            // Force update on next frame
            s_renderer.last_eye_x = -1000;
            s_renderer.last_mouth_curve = -1000;

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
