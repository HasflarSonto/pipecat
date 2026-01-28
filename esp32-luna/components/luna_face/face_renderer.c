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

// Custom 64pt font for large text displays (clock, weather)
#ifdef SIMULATOR
extern const lv_font_t lv_font_montserrat_64;
#endif

static const char *TAG = "face_renderer";

// Display configuration - LANDSCAPE after 90° rotation
// Original hardware: 410x502, After rotation: 502x410
#define DEFAULT_WIDTH       502
#define DEFAULT_HEIGHT      410

// Colors - Apple Watch inspired palette
#define BG_COLOR            0x000000    // Pure black background (AMOLED efficient)
#define FACE_COLOR          0xFFFFFF    // Pure white for eyes/face features

// Apple-style color palette
#define COLOR_CARD_BG       0x1C1C1E    // Dark gray card background
#define COLOR_CARD_BG_ALT   0x2C2C2E    // Slightly lighter card variant
#define COLOR_ACCENT_BLUE   0x0A84FF    // Apple system blue (primary accent)
#define COLOR_ACCENT_GREEN  0x30D158    // Apple system green
#define COLOR_ACCENT_ORANGE 0xFF9F0A    // Apple system orange
#define COLOR_ACCENT_RED    0xFF453A    // Apple system red
#define COLOR_ACCENT_YELLOW 0xFFD60A    // Apple system yellow
#define COLOR_TEXT_PRIMARY  0xFFFFFF    // White text
#define COLOR_TEXT_SECONDARY 0x8E8E93   // Gray secondary text
#define COLOR_TEXT_TERTIARY 0x636366    // Darker gray tertiary text

// Legacy colors (keep for compatibility)
#define COLOR_SAND          0xE4CBA9    // Sand - warm neutral
#define COLOR_SKYBLUE       0x7FC7CC    // Sky blue - calm, cool
#define COLOR_DEEPSEA       0x1C1C1E    // Now maps to card background
#define COLOR_MOSS          0x30D158    // Now maps to Apple green
#define COLOR_TERRACOTTA    0xFF9F0A    // Now maps to Apple orange
#define COLOR_CHERRY        0xFDABA5    // Cherry blossom - soft pink
#define COLOR_REDWINE       0xFF453A    // Now maps to Apple red
#define COLOR_SUNSHINE      0xFFD60A    // Now maps to Apple yellow

// Card styling
#define CARD_RADIUS         24          // Rounded corner radius for cards
#define CARD_PADDING        16          // Internal padding
#define CARD_MARGIN         12          // Space between cards
#define CARD_WIDTH          (DEFAULT_WIDTH - 40)  // Full width minus margins

// Standardized UI Style
#define STYLE_TAG_COLOR     COLOR_TEXT_SECONDARY  // Gray for top-left screen tags
#define STYLE_TAG_POS_X     20              // Top-left tag X position
#define STYLE_TAG_POS_Y     15              // Top-left tag Y position
#define STYLE_PRIMARY_TEXT  COLOR_TEXT_PRIMARY    // Pure white for primary content
#define STYLE_SECONDARY_TEXT COLOR_TEXT_SECONDARY // Gray for secondary info
#define STYLE_ACCENT_COLOR  COLOR_ACCENT_BLUE     // Blue for accents/highlights
#define STYLE_BUTTON_ACTIVE COLOR_ACCENT_GREEN    // Green for timer/active buttons
#define STYLE_BUTTON_WARN   COLOR_ACCENT_ORANGE   // Orange for warning/pause
#define STYLE_BUTTON_INACTIVE COLOR_CARD_BG       // Card bg for inactive buttons

// Animation timing - VERY SLOW to avoid SPI overflow
#define ANIMATION_PERIOD_MS    200      // ~5 FPS to reduce SPI load
#define RENDER_TASK_STACK_SIZE (8 * 1024)
#define RENDER_TASK_PRIORITY   3
#define RENDER_TASK_CORE       1

#define EMOTION_TRANSITION_SPEED    2.5f  // Slower transitions
#define GAZE_FOLLOW_SPEED          8.0f
#define BLINK_SPEED                10.0f
#define FACE_SHIFT_SPEED           5.0f
#define PET_RESPONSE_SPEED         12.0f  // How fast face responds to petting
#define PET_DECAY_SPEED            6.0f   // How fast face returns to neutral

// Petting parameters
#define PET_SENSITIVITY            0.5f   // How much face moves per pixel of touch movement
#define PET_MAX_OFFSET             20.0f  // Maximum face displacement from petting (in pixels)

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
    lv_obj_t *mouth_bg;         // Background rectangle to clear mouth area (prevents artifacts)
    lv_obj_t *mouth_arc;
    lv_obj_t *mouth_line;       // Rectangle for neutral/surprised
    lv_obj_t *mouth_dots[5];    // 5 dots arranged in curve for smile/frown
    lv_obj_t *cat_arc_top;      // Top arc for cat :3 mouth
    lv_obj_t *cat_arc_bottom;   // Bottom arc for cat :3 mouth
    lv_obj_t *whisker_lines[6]; // Line widgets for angled whiskers
    lv_point_precise_t whisker_points[6][2];  // Points for each whisker line
    lv_obj_t *wavy_mouth;       // Line widget for dizzy wavy mouth
    lv_point_precise_t wavy_mouth_points[24];  // Points for wavy sine line (more = smoother)
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

    // Petting state (touch-based "giggle" effect)
    bool touch_active;
    int last_touch_y;
    float pet_offset_y;
    float target_pet_offset;
    int64_t last_pet_time;

    // Blink state
    float blink_progress;
    bool is_blinking;
    int64_t last_blink_time;
    int32_t blink_interval_ms;

    // Cat mode
    bool cat_mode;

    // Wink state (eye poke)
    float left_wink;          // 0.0 = open, 1.0 = closed
    float right_wink;
    float target_left_wink;
    float target_right_wink;
    int64_t left_poke_time;   // Time when poked
    int64_t right_poke_time;

    // Dizzy state (shake detection)
    bool is_dizzy;
    int64_t dizzy_start_time;
    float dizzy_wobble;       // Current wobble phase
    emotion_id_t pre_dizzy_emotion;  // Emotion to restore after dizzy

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
static void update_petting(float delta_time);
static void update_face_widgets(void);
static float get_blink_factor(void);
static void timer_btn_start_click_cb(lv_event_t *e);
static void timer_btn_pause_click_cb(lv_event_t *e);

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

    // Mouth background - always-visible rectangle in BG_COLOR to clear artifacts
    // This ensures the mouth area is always "clean" before drawing mouth widgets
    s_renderer.mouth_bg = lv_obj_create(parent);
    lv_obj_remove_style_all(s_renderer.mouth_bg);
    lv_obj_set_style_bg_color(s_renderer.mouth_bg, rgb888_to_lv(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_renderer.mouth_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_renderer.mouth_bg, 0, 0);
    lv_obj_set_style_border_width(s_renderer.mouth_bg, 0, 0);
    // Size and position will be set in update_face_widgets based on mouth_base_y

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
    lv_obj_set_pos(s_renderer.mouth_arc, -100, -100);  // Off-screen initially to prevent ghost artifacts
    lv_obj_add_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);

    // Mouth line (for neutral/straight)
    s_renderer.mouth_line = lv_obj_create(parent);
    lv_obj_remove_style_all(s_renderer.mouth_line);
    lv_obj_set_style_bg_color(s_renderer.mouth_line, face_color, 0);
    lv_obj_set_style_bg_opa(s_renderer.mouth_line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_renderer.mouth_line, 3, 0);
    lv_obj_add_flag(s_renderer.mouth_line, LV_OBJ_FLAG_HIDDEN);

    // Left brow (for angry) - initialize at (0,0) with size (0,0) to prevent artifacts
    s_renderer.left_brow = lv_obj_create(parent);
    lv_obj_remove_style_all(s_renderer.left_brow);
    lv_obj_set_style_bg_color(s_renderer.left_brow, face_color, 0);
    lv_obj_set_style_bg_opa(s_renderer.left_brow, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_renderer.left_brow, 2, 0);
    lv_obj_set_size(s_renderer.left_brow, 0, 0);
    lv_obj_set_pos(s_renderer.left_brow, 0, 0);
    lv_obj_add_flag(s_renderer.left_brow, LV_OBJ_FLAG_HIDDEN);

    // Right brow (for angry) - initialize at (0,0) with size (0,0) to prevent artifacts
    s_renderer.right_brow = lv_obj_create(parent);
    lv_obj_remove_style_all(s_renderer.right_brow);
    lv_obj_set_style_bg_color(s_renderer.right_brow, face_color, 0);
    lv_obj_set_style_bg_opa(s_renderer.right_brow, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_renderer.right_brow, 2, 0);
    lv_obj_set_size(s_renderer.right_brow, 0, 0);
    lv_obj_set_pos(s_renderer.right_brow, 0, 0);
    lv_obj_add_flag(s_renderer.right_brow, LV_OBJ_FLAG_HIDDEN);

    // Sparkles removed - they looked scary (like pupils)
    s_renderer.left_sparkle = NULL;
    s_renderer.right_sparkle = NULL;

    // 5 dots for curved smile/frown (arranged in parabolic curve)
    for (int i = 0; i < 5; i++) {
        s_renderer.mouth_dots[i] = lv_obj_create(parent);
        lv_obj_remove_style_all(s_renderer.mouth_dots[i]);
        lv_obj_set_style_bg_color(s_renderer.mouth_dots[i], face_color, 0);
        lv_obj_set_style_bg_opa(s_renderer.mouth_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_renderer.mouth_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_size(s_renderer.mouth_dots[i], 0, 0);
        lv_obj_set_pos(s_renderer.mouth_dots[i], 0, 0);
        lv_obj_add_flag(s_renderer.mouth_dots[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Cat mouth arcs - two small arcs forming sideways "3" for :3 face
    s_renderer.cat_arc_top = lv_arc_create(parent);
    lv_obj_remove_style_all(s_renderer.cat_arc_top);
    lv_obj_set_style_arc_width(s_renderer.cat_arc_top, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_renderer.cat_arc_top, face_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_renderer.cat_arc_top, (int)(5 * SCALE_Y), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_renderer.cat_arc_top, true, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_renderer.cat_arc_top, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_renderer.cat_arc_top, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_arc_set_mode(s_renderer.cat_arc_top, LV_ARC_MODE_NORMAL);
    lv_obj_set_pos(s_renderer.cat_arc_top, -100, -100);  // Off-screen initially
    lv_obj_add_flag(s_renderer.cat_arc_top, LV_OBJ_FLAG_HIDDEN);

    s_renderer.cat_arc_bottom = lv_arc_create(parent);
    lv_obj_remove_style_all(s_renderer.cat_arc_bottom);
    lv_obj_set_style_arc_width(s_renderer.cat_arc_bottom, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_renderer.cat_arc_bottom, face_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_renderer.cat_arc_bottom, (int)(5 * SCALE_Y), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_renderer.cat_arc_bottom, true, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_renderer.cat_arc_bottom, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_renderer.cat_arc_bottom, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_arc_set_mode(s_renderer.cat_arc_bottom, LV_ARC_MODE_NORMAL);
    lv_obj_set_pos(s_renderer.cat_arc_bottom, -100, -100);  // Off-screen initially
    lv_obj_add_flag(s_renderer.cat_arc_bottom, LV_OBJ_FLAG_HIDDEN);

    // Whisker lines - using lv_line for proper angled whiskers
    for (int i = 0; i < 6; i++) {
        // Initialize points to minimal line (avoid ghost at 0,0)
        s_renderer.whisker_points[i][0].x = 0;
        s_renderer.whisker_points[i][0].y = 0;
        s_renderer.whisker_points[i][1].x = 1;
        s_renderer.whisker_points[i][1].y = 0;

        s_renderer.whisker_lines[i] = lv_line_create(parent);
        lv_obj_remove_style_all(s_renderer.whisker_lines[i]);
        lv_obj_set_style_line_color(s_renderer.whisker_lines[i], face_color, 0);
        lv_obj_set_style_line_width(s_renderer.whisker_lines[i], (int)(3 * SCALE_Y), 0);
        lv_obj_set_style_line_rounded(s_renderer.whisker_lines[i], true, 0);
        lv_line_set_points(s_renderer.whisker_lines[i], s_renderer.whisker_points[i], 2);
        // Position off-screen initially to prevent ghost artifacts
        lv_obj_set_pos(s_renderer.whisker_lines[i], -100, -100);
        lv_obj_add_flag(s_renderer.whisker_lines[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Wavy mouth line for dizzy state (24 points for smooth curves)
    // Initialize points to minimal line
    for (int i = 0; i < 24; i++) {
        s_renderer.wavy_mouth_points[i].x = i * 5;
        s_renderer.wavy_mouth_points[i].y = 0;
    }
    s_renderer.wavy_mouth = lv_line_create(parent);
    lv_obj_remove_style_all(s_renderer.wavy_mouth);
    lv_obj_set_style_line_color(s_renderer.wavy_mouth, face_color, 0);
    lv_obj_set_style_line_width(s_renderer.wavy_mouth, (int)(5 * SCALE_Y), 0);
    lv_obj_set_style_line_rounded(s_renderer.wavy_mouth, true, 0);
    lv_line_set_points(s_renderer.wavy_mouth, s_renderer.wavy_mouth_points, 24);
    lv_obj_set_pos(s_renderer.wavy_mouth, -100, -100);
    lv_obj_add_flag(s_renderer.wavy_mouth, LV_OBJ_FLAG_HIDDEN);
}

// Update face widget positions and sizes
static void update_face_widgets(void)
{
    emotion_config_t *params = &s_renderer.current_params;

    // Calculate blink factor
    float blink_factor = get_blink_factor();

    // Calculate base eye dimensions (height increased 40% for better visibility)
    float base_eye_height = params->eye_height * SCALE_Y * 1.4f;
    float eye_width = params->eye_width * SCALE_X;
    base_eye_height *= params->eye_openness;
    base_eye_height *= (1.0f - blink_factor * 0.95f);

    // Apply independent wink factors to each eye
    float left_eye_height = base_eye_height * (1.0f - s_renderer.left_wink * 0.95f);
    float right_eye_height = base_eye_height * (1.0f - s_renderer.right_wink * 0.95f);

    // Apply dizzy wobble effect (eyes at different heights)
    if (s_renderer.is_dizzy) {
        float wobble = sinf(s_renderer.dizzy_wobble) * 14.0f * SCALE_Y;
        left_eye_height += wobble;
        right_eye_height -= wobble;  // Opposite wobble for dizzy effect
    }

    if (left_eye_height < 5) left_eye_height = 5;
    if (right_eye_height < 5) right_eye_height = 5;

    // Gaze offset
    float gaze_range_x = 28.0f * SCALE_X;
    float gaze_range_y = 18.0f * SCALE_Y;
    int gaze_x_offset = (int)((s_renderer.gaze_x - 0.5f) * 2.0f * gaze_range_x);
    int gaze_y_offset = (int)((s_renderer.gaze_y - 0.5f) * 2.0f * gaze_range_y);

    // Face offset (includes gaze tracking + petting)
    int offset_x = (int)s_renderer.face_offset_x;
    int offset_y = (int)(s_renderer.face_offset_y + s_renderer.pet_offset_y);

    // Think offset (look to side)
    int think_offset = params->look_side ? (int)(12.0f * SCALE_X) : 0;

    // Tilt for confused/dizzy
    int left_tilt = params->tilt_eyes ? (int)(-8.0f * SCALE_Y) : 0;
    int right_tilt = params->tilt_eyes ? (int)(8.0f * SCALE_Y) : 0;

    // Add extra tilt during dizzy wobble
    if (s_renderer.is_dizzy) {
        float tilt_wobble = cosf(s_renderer.dizzy_wobble * 1.5f) * 8.0f * SCALE_Y;
        left_tilt += (int)tilt_wobble;
        right_tilt -= (int)tilt_wobble;
    }

    int eye_w = (int)eye_width;
    int left_eye_h = (int)left_eye_height;
    int right_eye_h = (int)right_eye_height;

    // Calculate corner radius (use smaller height for radius)
    int left_radius = (left_eye_h < eye_w) ? left_eye_h / 2 : eye_w / 2;
    int right_radius = (right_eye_h < eye_w) ? right_eye_h / 2 : eye_w / 2;
    int max_radius = (int)(15.0f * SCALE_Y);
    if (left_radius > max_radius) left_radius = max_radius;
    if (right_radius > max_radius) right_radius = max_radius;

    // Calculate eye positions
    int left_eye_x = s_renderer.left_eye_base_x + offset_x + gaze_x_offset + think_offset - eye_w/2;
    int left_eye_y = s_renderer.eye_base_y + offset_y + gaze_y_offset + left_tilt - left_eye_h/2;
    int right_eye_x = s_renderer.right_eye_base_x + offset_x + gaze_x_offset + think_offset - eye_w/2;
    int right_eye_y = s_renderer.eye_base_y + offset_y + gaze_y_offset + right_tilt - right_eye_h/2;

    // Check if significant change (now we need to track both eyes' heights)
    bool eye_changed = (abs(left_eye_x - s_renderer.last_eye_x) > MIN_EYE_CHANGE ||
                        abs(left_eye_y - s_renderer.last_eye_y) > MIN_EYE_CHANGE ||
                        abs(eye_w - s_renderer.last_eye_w) > MIN_EYE_CHANGE ||
                        abs(left_eye_h - s_renderer.last_eye_h) > MIN_EYE_CHANGE ||
                        s_renderer.left_wink > 0.01f || s_renderer.right_wink > 0.01f ||
                        s_renderer.is_dizzy);

    if (eye_changed) {
        // Update left eye (no invalidate - let LVGL handle dirty rects)
        lv_obj_set_pos(s_renderer.left_eye, left_eye_x, left_eye_y);
        lv_obj_set_size(s_renderer.left_eye, eye_w, left_eye_h);
        lv_obj_set_style_radius(s_renderer.left_eye, left_radius, 0);

        // Update right eye (may have different height due to wink)
        lv_obj_set_pos(s_renderer.right_eye, right_eye_x, right_eye_y);
        lv_obj_set_size(s_renderer.right_eye, eye_w, right_eye_h);
        lv_obj_set_style_radius(s_renderer.right_eye, right_radius, 0);

        s_renderer.last_eye_x = left_eye_x;
        s_renderer.last_eye_y = left_eye_y;
        s_renderer.last_eye_w = eye_w;
        s_renderer.last_eye_h = left_eye_h;
    }

    // Sparkles disabled - they looked scary (like pupils)

    // Mouth position
    int mouth_x = s_renderer.center_x + offset_x;
    int mouth_y = s_renderer.mouth_base_y + offset_y;
    int mouth_width = (int)(params->mouth_width * SCALE_X);
    int line_width = (int)(6 * SCALE_Y);

    // Position mouth_bg as a thin strip above the mouth to catch ghost artifacts
    // The artifacts appear just above the mouth arc area when it moves
    int mouth_bg_width = 80;    // Just wide enough for the mouth arc area
    int mouth_bg_height = 30;   // Thin strip to catch artifacts without clipping eyes
    int mouth_bg_x = s_renderer.center_x - mouth_bg_width / 2 + offset_x;
    int mouth_bg_y = s_renderer.mouth_base_y - 50 + offset_y;  // Above the mouth
    lv_obj_set_pos(s_renderer.mouth_bg, mouth_bg_x, mouth_bg_y);
    lv_obj_set_size(s_renderer.mouth_bg, mouth_bg_width, mouth_bg_height);
    lv_obj_clear_flag(s_renderer.mouth_bg, LV_OBJ_FLAG_HIDDEN);

    // Calculate mouth curve category
    int curve_category;
    if (params->no_mouth) {
        curve_category = -100;  // No mouth (eyes only)
    } else if (params->cat_face) {
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
        ESP_LOGI(TAG, "Mouth curve changed: %d -> %d (mouth_curve=%.2f)",
                 s_renderer.last_mouth_curve, curve_category, params->mouth_curve);

        // Hide all mouth widgets first (simple hide - no invalidation to avoid artifacts)
        lv_obj_add_flag(s_renderer.mouth_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_renderer.wavy_mouth, LV_OBJ_FLAG_HIDDEN);

        for (int i = 0; i < 5; i++) {
            lv_obj_add_flag(s_renderer.mouth_dots[i], LV_OBJ_FLAG_HIDDEN);
        }

        // Hide whiskers and cat arcs
        for (int i = 0; i < 6; i++) {
            lv_obj_add_flag(s_renderer.whisker_lines[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_renderer.cat_arc_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_renderer.cat_arc_bottom, LV_OBJ_FLAG_HIDDEN);

        if (curve_category == -100) {
            // Eyes only - no mouth, hide the background too
            lv_obj_add_flag(s_renderer.mouth_bg, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "Eyes only mode - mouth hidden");
        } else if (curve_category == 100) {
            // Cat face ":3" mouth - two small arcs forming sideways "3"
            // LVGL arc: 0° is right (3 o'clock), angles increase counter-clockwise

            int arc_size = 40;  // Small to avoid SPI issues
            int arc_thickness = (int)(6 * SCALE_Y);
            int center_x = s_renderer.width / 2 + offset_x;
            int cat_y = mouth_y;

            // For :3, we want two curves opening UPWARD (like ω flipped)
            // Use angles 0-180 for top semicircle (opens up)
            // Overlap the arcs so they connect into one smooth line

            int overlap = arc_size / 5;  // Small overlap to connect arcs

            // Left arc - opens upward
            lv_obj_set_size(s_renderer.cat_arc_top, arc_size, arc_size);
            int left_arc_x = center_x - arc_size + overlap/2;
            int left_arc_y = cat_y - arc_size/2;
            lv_obj_set_pos(s_renderer.cat_arc_top, left_arc_x, left_arc_y);
            // 0° to 180° = top semicircle (opens upward)
            lv_arc_set_bg_angles(s_renderer.cat_arc_top, 0, 180);
            lv_arc_set_angles(s_renderer.cat_arc_top, 0, 180);
            lv_obj_set_style_arc_width(s_renderer.cat_arc_top, arc_thickness, LV_PART_INDICATOR);
            lv_obj_remove_flag(s_renderer.cat_arc_top, LV_OBJ_FLAG_HIDDEN);

            // Right arc - opens upward, overlapping with left
            lv_obj_set_size(s_renderer.cat_arc_bottom, arc_size, arc_size);
            int right_arc_x = center_x - overlap/2;
            int right_arc_y = cat_y - arc_size/2;
            lv_obj_set_pos(s_renderer.cat_arc_bottom, right_arc_x, right_arc_y);
            lv_arc_set_bg_angles(s_renderer.cat_arc_bottom, 0, 180);
            lv_arc_set_angles(s_renderer.cat_arc_bottom, 0, 180);
            lv_obj_set_style_arc_width(s_renderer.cat_arc_bottom, arc_thickness, LV_PART_INDICATOR);
            lv_obj_remove_flag(s_renderer.cat_arc_bottom, LV_OBJ_FLAG_HIDDEN);

            ESP_LOGI(TAG, "Cat :3 arcs at y=%d", cat_y);

            // Show whiskers - position line widgets at (0,0) and use absolute coords
            int whisker_len = (int)(55 * SCALE_X);
            int whisker_x_offset = (int)(50 * SCALE_X);
            int whisker_y_spacing = (int)(14 * SCALE_Y);
            int whisker_y_fan = (int)(10 * SCALE_Y);

            // Left whiskers - start near mouth, extend left with fan angle
            // Whisker 0: top-left, angles up
            s_renderer.whisker_points[0][0].x = 0;
            s_renderer.whisker_points[0][0].y = 0;
            s_renderer.whisker_points[0][1].x = whisker_len;
            s_renderer.whisker_points[0][1].y = whisker_y_fan;  // Angles down-right (so going left it angles up)
            lv_line_set_points(s_renderer.whisker_lines[0], s_renderer.whisker_points[0], 2);
            lv_obj_set_pos(s_renderer.whisker_lines[0], center_x - whisker_x_offset - whisker_len, cat_y - whisker_y_spacing - whisker_y_fan);
            lv_obj_remove_flag(s_renderer.whisker_lines[0], LV_OBJ_FLAG_HIDDEN);

            // Whisker 1: middle-left, horizontal
            s_renderer.whisker_points[1][0].x = 0;
            s_renderer.whisker_points[1][0].y = 0;
            s_renderer.whisker_points[1][1].x = whisker_len;
            s_renderer.whisker_points[1][1].y = 0;
            lv_line_set_points(s_renderer.whisker_lines[1], s_renderer.whisker_points[1], 2);
            lv_obj_set_pos(s_renderer.whisker_lines[1], center_x - whisker_x_offset - whisker_len, cat_y);
            lv_obj_remove_flag(s_renderer.whisker_lines[1], LV_OBJ_FLAG_HIDDEN);

            // Whisker 2: bottom-left, angles down
            s_renderer.whisker_points[2][0].x = 0;
            s_renderer.whisker_points[2][0].y = whisker_y_fan;
            s_renderer.whisker_points[2][1].x = whisker_len;
            s_renderer.whisker_points[2][1].y = 0;
            lv_line_set_points(s_renderer.whisker_lines[2], s_renderer.whisker_points[2], 2);
            lv_obj_set_pos(s_renderer.whisker_lines[2], center_x - whisker_x_offset - whisker_len, cat_y + whisker_y_spacing);
            lv_obj_remove_flag(s_renderer.whisker_lines[2], LV_OBJ_FLAG_HIDDEN);

            // Right whiskers - mirror of left
            // Whisker 3: top-right, angles up
            s_renderer.whisker_points[3][0].x = 0;
            s_renderer.whisker_points[3][0].y = whisker_y_fan;
            s_renderer.whisker_points[3][1].x = whisker_len;
            s_renderer.whisker_points[3][1].y = 0;
            lv_line_set_points(s_renderer.whisker_lines[3], s_renderer.whisker_points[3], 2);
            lv_obj_set_pos(s_renderer.whisker_lines[3], center_x + whisker_x_offset, cat_y - whisker_y_spacing - whisker_y_fan);
            lv_obj_remove_flag(s_renderer.whisker_lines[3], LV_OBJ_FLAG_HIDDEN);

            // Whisker 4: middle-right, horizontal
            s_renderer.whisker_points[4][0].x = 0;
            s_renderer.whisker_points[4][0].y = 0;
            s_renderer.whisker_points[4][1].x = whisker_len;
            s_renderer.whisker_points[4][1].y = 0;
            lv_line_set_points(s_renderer.whisker_lines[4], s_renderer.whisker_points[4], 2);
            lv_obj_set_pos(s_renderer.whisker_lines[4], center_x + whisker_x_offset, cat_y);
            lv_obj_remove_flag(s_renderer.whisker_lines[4], LV_OBJ_FLAG_HIDDEN);

            // Whisker 5: bottom-right, angles down
            s_renderer.whisker_points[5][0].x = 0;
            s_renderer.whisker_points[5][0].y = 0;
            s_renderer.whisker_points[5][1].x = whisker_len;
            s_renderer.whisker_points[5][1].y = whisker_y_fan;
            lv_line_set_points(s_renderer.whisker_lines[5], s_renderer.whisker_points[5], 2);
            lv_obj_set_pos(s_renderer.whisker_lines[5], center_x + whisker_x_offset, cat_y + whisker_y_spacing);
            lv_obj_remove_flag(s_renderer.whisker_lines[5], LV_OBJ_FLAG_HIDDEN);

            ESP_LOGI(TAG, "Cat :3 with whiskers at center_x=%d, cat_y=%d", center_x, cat_y);

        } else if (curve_category == 50) {
            // Surprised O - circular mouth using mouth_line
            int o_size = (int)(35 * SCALE_X);
            lv_obj_set_size(s_renderer.mouth_line, o_size, o_size);
            lv_obj_set_pos(s_renderer.mouth_line, mouth_x - o_size/2, mouth_y - o_size/2);
            lv_obj_set_style_radius(s_renderer.mouth_line, o_size/2, 0);
            lv_obj_remove_flag(s_renderer.mouth_line, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "Surprised O mouth at y=%d, size=%d", mouth_y, o_size);

        } else if (curve_category == 0) {
            // Neutral - straight line rectangle
            int line_len = (int)(mouth_width * 1.5f);
            lv_obj_set_size(s_renderer.mouth_line, line_len, line_width);
            lv_obj_set_pos(s_renderer.mouth_line, mouth_x - line_len/2, mouth_y - line_width/2);
            lv_obj_set_style_radius(s_renderer.mouth_line, 3, 0);
            lv_obj_remove_flag(s_renderer.mouth_line, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "Neutral mouth at y=%d, len=%d", mouth_y, line_len);

        } else if (curve_category == 1) {
            // Smile - arc curving downward (like a U)
            // Use same pattern as cat arcs: square size, 0-180° angles
            int arc_size = 60;  // Same approach as cat arcs (which use 40)
            int arc_thickness = (int)(6 * SCALE_Y);
            lv_obj_set_size(s_renderer.mouth_arc, arc_size, arc_size);  // Square like cat arcs
            lv_obj_set_pos(s_renderer.mouth_arc, mouth_x - arc_size/2, mouth_y - arc_size/2);
            // Smile: 180° to 360° = bottom semicircle (curves down like a smile)
            lv_arc_set_bg_angles(s_renderer.mouth_arc, 180, 360);
            lv_arc_set_angles(s_renderer.mouth_arc, 180, 360);
            lv_obj_set_style_arc_width(s_renderer.mouth_arc, arc_thickness, LV_PART_INDICATOR);
            lv_obj_remove_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "Smile (arc) at y=%d, size=%d", mouth_y, arc_size);

        } else {
            // Frown - arc curving upward (inverted U)
            // Use same pattern as cat arcs: square size, 0-180° angles
            int arc_size = 60;  // Same approach as cat arcs (which use 40)
            int arc_thickness = (int)(6 * SCALE_Y);
            lv_obj_set_size(s_renderer.mouth_arc, arc_size, arc_size);  // Square like cat arcs
            lv_obj_set_pos(s_renderer.mouth_arc, mouth_x - arc_size/2, mouth_y - arc_size/2);
            // Frown: 0° to 180° = top semicircle (curves up like a frown)
            lv_arc_set_bg_angles(s_renderer.mouth_arc, 0, 180);
            lv_arc_set_angles(s_renderer.mouth_arc, 0, 180);
            lv_obj_set_style_arc_width(s_renderer.mouth_arc, arc_thickness, LV_PART_INDICATOR);
            lv_obj_remove_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "Frown (arc) at y=%d, size=%d", mouth_y, arc_size);
        }

        s_renderer.last_mouth_curve = curve_category;
    }

    // Wavy mouth for dizzy state (overrides normal mouth, animates each frame)
    if (s_renderer.is_dizzy) {
        // Hide all normal mouth widgets
        lv_obj_add_flag(s_renderer.mouth_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 5; i++) {
            lv_obj_add_flag(s_renderer.mouth_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_renderer.cat_arc_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_renderer.cat_arc_bottom, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 6; i++) {
            lv_obj_add_flag(s_renderer.whisker_lines[i], LV_OBJ_FLAG_HIDDEN);
        }

        // Generate wavy sine line points (24 points for smoother curves)
        int wavy_width = (int)(100 * SCALE_X);  // Total width of wavy line
        int wavy_amplitude = (int)(10 * SCALE_Y);  // Height of wave
        int num_points = 24;
        int segment_width = wavy_width / (num_points - 1);

        // Use dizzy_wobble as phase offset to animate the wave
        float phase = s_renderer.dizzy_wobble * 2.0f;

        for (int i = 0; i < num_points; i++) {
            s_renderer.wavy_mouth_points[i].x = i * segment_width;
            // Use smoother wave frequency (0.4 instead of 0.8) for rounder curves
            s_renderer.wavy_mouth_points[i].y = wavy_amplitude +
                (int)(sinf(phase + i * 0.4f) * wavy_amplitude);
        }

        // Position wavy mouth at center of face
        int wavy_x = mouth_x - wavy_width / 2;
        int wavy_y = mouth_y - wavy_amplitude;

        lv_line_set_points(s_renderer.wavy_mouth, s_renderer.wavy_mouth_points, num_points);
        lv_obj_set_pos(s_renderer.wavy_mouth, wavy_x, wavy_y);
        lv_obj_remove_flag(s_renderer.wavy_mouth, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Hide wavy mouth when not dizzy
        lv_obj_add_flag(s_renderer.wavy_mouth, LV_OBJ_FLAG_HIDDEN);
    }

    // Update angry brows
    if (params->angry_brows != s_renderer.last_angry_brows) {
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

                // Update petting (touch-based face movement)
                update_petting(delta_time);

                // Update face widgets
                if (bsp_display_lock(pdMS_TO_TICKS(50))) {
                    update_face_widgets();

                    // Periodic full-screen invalidation DISABLED - causes SPI overflow
                    // Artifacts will be handled by hiding widgets before showing new ones

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

    // Update wink state (eye poke)
    #define WINK_SPEED 10.0f      // How fast wink opens/closes
    #define WINK_DECAY_MS 400     // How long eye stays closed after poke

    // Left eye wink
    if (s_renderer.left_poke_time > 0) {
        int64_t elapsed = current_time - s_renderer.left_poke_time;
        if (elapsed > WINK_DECAY_MS) {
            // Start opening
            s_renderer.target_left_wink = 0.0f;
            if (s_renderer.left_wink < 0.05f) {
                s_renderer.left_poke_time = 0;
            }
        }
    }
    s_renderer.left_wink = lerp(s_renderer.left_wink, s_renderer.target_left_wink, delta_time * WINK_SPEED);

    // Right eye wink
    if (s_renderer.right_poke_time > 0) {
        int64_t elapsed = current_time - s_renderer.right_poke_time;
        if (elapsed > WINK_DECAY_MS) {
            // Start opening
            s_renderer.target_right_wink = 0.0f;
            if (s_renderer.right_wink < 0.05f) {
                s_renderer.right_poke_time = 0;
            }
        }
    }
    s_renderer.right_wink = lerp(s_renderer.right_wink, s_renderer.target_right_wink, delta_time * WINK_SPEED);

    // Update dizzy wobble effect
    #define DIZZY_DURATION_MS 1500   // How long dizzy lasts (1.5 seconds)
    #define DIZZY_WOBBLE_SPEED 8.0f  // Wobble frequency

    if (s_renderer.is_dizzy) {
        int64_t elapsed = current_time - s_renderer.dizzy_start_time;
        if (elapsed > DIZZY_DURATION_MS) {
            // Auto-recover from dizzy after duration
            face_renderer_set_dizzy(false);
        } else {
            // Update wobble phase
            s_renderer.dizzy_wobble += delta_time * DIZZY_WOBBLE_SPEED;
        }
    }
}

// Update petting state based on touch input
static void update_petting(float delta_time)
{
    // Get the LVGL input device for touch
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev == NULL) {
        return;
    }

    // Get touch state
    lv_indev_state_t state = lv_indev_get_state(indev);
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    bool was_petting = s_renderer.touch_active;

    if (state == LV_INDEV_STATE_PRESSED) {
        if (s_renderer.touch_active) {
            // Calculate vertical movement delta
            int delta_y = point.y - s_renderer.last_touch_y;

            // Only update if movement exceeds threshold (reduces artifacts)
            if (abs(delta_y) > 3) {
                // Apply sensitivity and clamp
                float offset = delta_y * PET_SENSITIVITY;
                s_renderer.target_pet_offset += offset;

                // Clamp target offset
                if (s_renderer.target_pet_offset > PET_MAX_OFFSET) {
                    s_renderer.target_pet_offset = PET_MAX_OFFSET;
                } else if (s_renderer.target_pet_offset < -PET_MAX_OFFSET) {
                    s_renderer.target_pet_offset = -PET_MAX_OFFSET;
                }

                // Update last position only when we actually moved
                s_renderer.last_touch_y = point.y;
            }
        } else {
            // First touch - store position
            s_renderer.last_touch_y = point.y;
        }

        // Store touch state
        s_renderer.touch_active = true;
        s_renderer.last_pet_time = esp_timer_get_time() / 1000;

        // Auto-switch to cat face when petting starts
        if (!was_petting && s_renderer.target_emotion != EMOTION_CAT) {
            // Save current emotion to restore later (stored in cat_mode flag area)
            s_renderer.cat_mode = true;  // Mark that we auto-switched
            s_renderer.target_emotion = EMOTION_CAT;
            s_renderer.emotion_transition = 0.0f;
            s_renderer.last_mouth_curve = -1000;  // Force mouth redraw
        }
    } else {
        // Touch released - decay the offset back to zero
        if (s_renderer.touch_active) {
            // Just released - start decay
            s_renderer.target_pet_offset = 0.0f;

            // Restore to happy face after petting (cat enjoyed it!)
            if (s_renderer.cat_mode) {
                s_renderer.cat_mode = false;
                s_renderer.target_emotion = EMOTION_HAPPY;
                s_renderer.emotion_transition = 0.0f;
                s_renderer.last_mouth_curve = -1000;
            }
        }
        s_renderer.touch_active = false;
    }

    // Smoothly interpolate towards target (slower to reduce artifacts)
    float speed = s_renderer.touch_active ? PET_RESPONSE_SPEED : PET_DECAY_SPEED;
    float new_offset = lerp(s_renderer.pet_offset_y, s_renderer.target_pet_offset,
                            delta_time * speed);

    // Only update if change is significant (reduces artifacts from tiny movements)
    if (fabsf(new_offset - s_renderer.pet_offset_y) > 1.0f ||
        (!s_renderer.touch_active && fabsf(new_offset) < 1.0f)) {
        s_renderer.pet_offset_y = new_offset;
    }

    // Snap to zero if very small and not touching
    if (!s_renderer.touch_active && fabsf(s_renderer.pet_offset_y) < 1.0f) {
        s_renderer.pet_offset_y = 0.0f;
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
    ESP_LOGI(TAG, "Backlight on, acquiring display lock...");

    // Create face widgets
    if (!bsp_display_lock(pdMS_TO_TICKS(5000))) {
        ESP_LOGE(TAG, "Failed to acquire display lock (timeout)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Display lock acquired, creating widgets...");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, rgb888_to_lv(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    create_face_widgets(scr);
    ESP_LOGI(TAG, "Face widgets created, refreshing display...");

    // Force full-screen refresh to clear any artifacts
    lv_obj_invalidate(scr);
    lv_refr_now(s_renderer.display);
    ESP_LOGI(TAG, "Display refreshed, creating text label...");

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
    s_renderer.current_emotion = EMOTION_EYES_ONLY;
    s_renderer.target_emotion = EMOTION_EYES_ONLY;
    s_renderer.emotion_transition = 1.0f;
    s_renderer.current_params = *emotion_get_config(EMOTION_EYES_ONLY);

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

        // Force widget state recheck on emotion change
        s_renderer.last_mouth_curve = -1000;  // Force mouth redraw

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

void face_renderer_set_wink(float left_wink, float right_wink)
{
    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_renderer.target_left_wink = left_wink < 0.0f ? 0.0f : (left_wink > 1.0f ? 1.0f : left_wink);
        s_renderer.target_right_wink = right_wink < 0.0f ? 0.0f : (right_wink > 1.0f ? 1.0f : right_wink);
        xSemaphoreGive(s_renderer.mutex);
    }
}

void face_renderer_poke_eye(int which_eye)
{
    if (!s_renderer.initialized) {
        return;
    }

    int64_t now = esp_timer_get_time() / 1000;

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (which_eye == 0) {
            // Left eye poke
            s_renderer.target_left_wink = 1.0f;
            s_renderer.left_poke_time = now;
            ESP_LOGI(TAG, "Left eye poked!");
        } else if (which_eye == 1) {
            // Right eye poke
            s_renderer.target_right_wink = 1.0f;
            s_renderer.right_poke_time = now;
            ESP_LOGI(TAG, "Right eye poked!");
        }
        xSemaphoreGive(s_renderer.mutex);
    }
}

int face_renderer_hit_test_eye(int x, int y)
{
    if (!s_renderer.initialized || s_renderer.mode != DISPLAY_MODE_FACE) {
        return -1;
    }

    // Get current eye positions (base + offsets)
    int offset_x = (int)s_renderer.face_offset_x;
    int offset_y = (int)(s_renderer.face_offset_y + s_renderer.pet_offset_y);

    // Calculate eye dimensions
    emotion_config_t *params = &s_renderer.current_params;
    float eye_height = params->eye_height * SCALE_Y * 1.4f * params->eye_openness;
    float eye_width = params->eye_width * SCALE_X;

    int left_eye_cx = s_renderer.left_eye_base_x + offset_x;
    int left_eye_cy = s_renderer.eye_base_y + offset_y;
    int right_eye_cx = s_renderer.right_eye_base_x + offset_x;
    int right_eye_cy = s_renderer.eye_base_y + offset_y;

    // Hit test radius (slightly larger than eye for easier interaction)
    int hit_radius_x = (int)(eye_width * 0.7f);
    int hit_radius_y = (int)(eye_height * 0.7f);

    // Check left eye
    int dx = x - left_eye_cx;
    int dy = y - left_eye_cy;
    if (dx*dx / (float)(hit_radius_x * hit_radius_x) + dy*dy / (float)(hit_radius_y * hit_radius_y) <= 1.0f) {
        return 0;  // Left eye
    }

    // Check right eye
    dx = x - right_eye_cx;
    dy = y - right_eye_cy;
    if (dx*dx / (float)(hit_radius_x * hit_radius_x) + dy*dy / (float)(hit_radius_y * hit_radius_y) <= 1.0f) {
        return 1;  // Right eye
    }

    return -1;  // Not on eye
}

void face_renderer_set_dizzy(bool dizzy)
{
    if (!s_renderer.initialized) {
        return;
    }

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (dizzy && !s_renderer.is_dizzy) {
            // Start dizzy state
            s_renderer.is_dizzy = true;
            s_renderer.dizzy_start_time = esp_timer_get_time() / 1000;
            s_renderer.dizzy_wobble = 0.0f;
            s_renderer.pre_dizzy_emotion = s_renderer.target_emotion;
            s_renderer.target_emotion = EMOTION_DIZZY;
            s_renderer.emotion_transition = 0.0f;
            s_renderer.last_mouth_curve = -1000;  // Force redraw
            ESP_LOGI(TAG, "Dizzy mode ON!");
        } else if (!dizzy && s_renderer.is_dizzy) {
            // End dizzy state
            s_renderer.is_dizzy = false;
            s_renderer.target_emotion = s_renderer.pre_dizzy_emotion;
            s_renderer.emotion_transition = 0.0f;
            s_renderer.last_mouth_curve = -1000;
            ESP_LOGI(TAG, "Dizzy mode OFF, restoring %s", emotion_to_string(s_renderer.target_emotion));
        }
        xSemaphoreGive(s_renderer.mutex);
    }
}

bool face_renderer_is_dizzy(void)
{
    return s_renderer.is_dizzy;
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
            for (int i = 0; i < 5; i++) {
                lv_obj_add_flag(s_renderer.mouth_dots[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_add_flag(s_renderer.left_brow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.right_brow, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 6; i++) {
                lv_obj_add_flag(s_renderer.whisker_lines[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_add_flag(s_renderer.cat_arc_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.cat_arc_bottom, LV_OBJ_FLAG_HIDDEN);

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
            for (int i = 0; i < 5; i++) {
                lv_obj_add_flag(s_renderer.mouth_dots[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_add_flag(s_renderer.left_brow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.right_brow, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 6; i++) {
                lv_obj_add_flag(s_renderer.whisker_lines[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_add_flag(s_renderer.cat_arc_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_renderer.cat_arc_bottom, LV_OBJ_FLAG_HIDDEN);

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

// ============================================================================
// New Display Screens (Weather, Timer, Clock, Animation)
// ============================================================================

// Weather icon drawing helpers - static widgets created once
static lv_obj_t *s_weather_icon_objs[10] = {0};  // For sun rays, cloud shapes, etc.
static int s_weather_icon_count = 0;
static lv_obj_t *s_weather_card = NULL;
static lv_obj_t *s_weather_desc_label = NULL;

// Animation particles
#define MAX_PARTICLES 30
static lv_obj_t *s_particles[MAX_PARTICLES] = {0};
static float s_particle_x[MAX_PARTICLES];
static float s_particle_y[MAX_PARTICLES];
static float s_particle_speed[MAX_PARTICLES];
static animation_type_t s_current_animation = ANIMATION_RAIN;
static bool s_animation_active = false;

// Timer widgets and state
static lv_obj_t *s_timer_arc = NULL;
static lv_obj_t *s_timer_label_small = NULL;  // "Focus" label in top-left
static lv_obj_t *s_timer_btn_start = NULL;
static lv_obj_t *s_timer_btn_pause = NULL;
static lv_obj_t *s_timer_btn_label_start = NULL;
static lv_obj_t *s_timer_btn_label_pause = NULL;

// Timer state (for functional countdown)
static int s_timer_minutes = 25;
static int s_timer_seconds = 0;
static int s_timer_total_seconds_start = 25 * 60;  // Total at start
static bool s_timer_running = false;
static int64_t s_timer_last_tick = 0;

// Clock AM/PM label
static lv_obj_t *s_clock_ampm_label = NULL;
static lv_obj_t *s_clock_date_label = NULL;
static lv_obj_t *s_clock_card = NULL;

// Subway display widgets
static lv_obj_t *s_subway_card = NULL;         // Card background
static lv_obj_t *s_subway_circle = NULL;       // Colored circle for line
static lv_obj_t *s_subway_line_label = NULL;   // Line name inside circle
static lv_obj_t *s_subway_station_label = NULL; // Station name
static lv_obj_t *s_subway_time_labels[3] = {NULL, NULL, NULL};  // Arrival times

// Shared screen tag label (used for "Weather", "Clock", etc. in top-left)
static lv_obj_t *s_screen_tag_label = NULL;

// Show screen tag in top-left corner (standardized position and style)
static void show_screen_tag(const char *tag_text)
{
    lv_obj_t *scr = lv_scr_act();
    if (!s_screen_tag_label) {
        s_screen_tag_label = lv_label_create(scr);
    }
    lv_obj_clear_flag(s_screen_tag_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_screen_tag_label, tag_text);
    lv_obj_set_style_text_color(s_screen_tag_label, lv_color_hex(STYLE_TAG_COLOR), 0);
    lv_obj_set_style_text_font(s_screen_tag_label, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_screen_tag_label, STYLE_TAG_POS_X, STYLE_TAG_POS_Y);
}

// Create an Apple-style card widget
// Returns the card object (caller can add children to it)
static lv_obj_t *create_card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, width, height);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, CARD_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, CARD_PADDING, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

// Calendar card storage (up to 3 events)
#define MAX_CALENDAR_CARDS 3
static lv_obj_t *s_calendar_cards[MAX_CALENDAR_CARDS] = {NULL, NULL, NULL};
static lv_obj_t *s_calendar_time_labels[MAX_CALENDAR_CARDS] = {NULL, NULL, NULL};
static lv_obj_t *s_calendar_title_labels[MAX_CALENDAR_CARDS] = {NULL, NULL, NULL};
static lv_obj_t *s_calendar_location_labels[MAX_CALENDAR_CARDS] = {NULL, NULL, NULL};

// Clear weather icon objects and card
static void clear_weather_icons(void)
{
    for (int i = 0; i < s_weather_icon_count; i++) {
        if (s_weather_icon_objs[i]) {
            lv_obj_delete(s_weather_icon_objs[i]);
            s_weather_icon_objs[i] = NULL;
        }
    }
    s_weather_icon_count = 0;
    if (s_weather_card) lv_obj_add_flag(s_weather_card, LV_OBJ_FLAG_HIDDEN);
    if (s_weather_desc_label) lv_obj_add_flag(s_weather_desc_label, LV_OBJ_FLAG_HIDDEN);
}

// Clear calendar cards (deletes the card objects and their children)
static void clear_calendar_cards(void)
{
    for (int i = 0; i < MAX_CALENDAR_CARDS; i++) {
        if (s_calendar_cards[i]) {
            lv_obj_delete(s_calendar_cards[i]);
            s_calendar_cards[i] = NULL;
        }
        // Labels are children of cards, so they get deleted automatically
        s_calendar_time_labels[i] = NULL;
        s_calendar_title_labels[i] = NULL;
        s_calendar_location_labels[i] = NULL;
    }
}

// Clear animation particles
static void clear_particles(void)
{
    s_animation_active = false;
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (s_particles[i]) {
            lv_obj_delete(s_particles[i]);
            s_particles[i] = NULL;
        }
    }
}

// Hide ALL screen-specific elements (call at start of each show_* function)
static void hide_all_screen_elements(void)
{
    // Hide face elements
    lv_obj_add_flag(s_renderer.left_eye, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_renderer.right_eye, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_renderer.mouth_arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_renderer.mouth_line, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 5; i++) {
        if (s_renderer.mouth_dots[i]) {
            lv_obj_add_flag(s_renderer.mouth_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_renderer.cat_arc_top) lv_obj_add_flag(s_renderer.cat_arc_top, LV_OBJ_FLAG_HIDDEN);
    if (s_renderer.cat_arc_bottom) lv_obj_add_flag(s_renderer.cat_arc_bottom, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 6; i++) {
        if (s_renderer.whisker_lines[i]) {
            lv_obj_add_flag(s_renderer.whisker_lines[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_renderer.left_brow) lv_obj_add_flag(s_renderer.left_brow, LV_OBJ_FLAG_HIDDEN);
    if (s_renderer.right_brow) lv_obj_add_flag(s_renderer.right_brow, LV_OBJ_FLAG_HIDDEN);
    if (s_renderer.left_sparkle) lv_obj_add_flag(s_renderer.left_sparkle, LV_OBJ_FLAG_HIDDEN);
    if (s_renderer.right_sparkle) lv_obj_add_flag(s_renderer.right_sparkle, LV_OBJ_FLAG_HIDDEN);
    if (s_renderer.mouth_bg) lv_obj_add_flag(s_renderer.mouth_bg, LV_OBJ_FLAG_HIDDEN);
    if (s_renderer.wavy_mouth) lv_obj_add_flag(s_renderer.wavy_mouth, LV_OBJ_FLAG_HIDDEN);

    // Hide text label
    lv_obj_add_flag(s_renderer.text_label, LV_OBJ_FLAG_HIDDEN);

    // Hide timer elements
    if (s_timer_arc) lv_obj_add_flag(s_timer_arc, LV_OBJ_FLAG_HIDDEN);
    if (s_timer_label_small) lv_obj_add_flag(s_timer_label_small, LV_OBJ_FLAG_HIDDEN);
    if (s_timer_btn_start) lv_obj_add_flag(s_timer_btn_start, LV_OBJ_FLAG_HIDDEN);
    if (s_timer_btn_pause) lv_obj_add_flag(s_timer_btn_pause, LV_OBJ_FLAG_HIDDEN);

    // Hide clock elements
    if (s_clock_ampm_label) lv_obj_add_flag(s_clock_ampm_label, LV_OBJ_FLAG_HIDDEN);
    if (s_clock_date_label) lv_obj_add_flag(s_clock_date_label, LV_OBJ_FLAG_HIDDEN);
    if (s_clock_card) lv_obj_add_flag(s_clock_card, LV_OBJ_FLAG_HIDDEN);

    // Hide subway elements
    if (s_subway_card) lv_obj_add_flag(s_subway_card, LV_OBJ_FLAG_HIDDEN);
    if (s_subway_circle) lv_obj_add_flag(s_subway_circle, LV_OBJ_FLAG_HIDDEN);
    if (s_subway_line_label) lv_obj_add_flag(s_subway_line_label, LV_OBJ_FLAG_HIDDEN);
    if (s_subway_station_label) lv_obj_add_flag(s_subway_station_label, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) {
        if (s_subway_time_labels[i]) lv_obj_add_flag(s_subway_time_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Hide shared screen tag
    if (s_screen_tag_label) lv_obj_add_flag(s_screen_tag_label, LV_OBJ_FLAG_HIDDEN);

    // Clear dynamic elements
    clear_weather_icons();
    clear_particles();
    clear_calendar_cards();
}

// Draw sun icon (circle + rays around it) - Apple Weather style
static void draw_sun_icon(int cx, int cy, int radius)
{
    lv_obj_t *scr = lv_scr_act();

    // Sun circle (main body) - bright yellow
    lv_obj_t *sun = lv_obj_create(scr);
    lv_obj_remove_style_all(sun);
    lv_obj_set_size(sun, radius * 2, radius * 2);
    lv_obj_set_pos(sun, cx - radius, cy - radius);
    lv_obj_set_style_bg_color(sun, lv_color_hex(COLOR_ACCENT_YELLOW), 0);  // Apple yellow
    lv_obj_set_style_bg_opa(sun, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
    s_weather_icon_objs[s_weather_icon_count++] = sun;

    // Sun rays as small dots (8 around the sun) - simple and efficient
    int ray_size = 12;
    int ray_dist = radius + 16;
    for (int i = 0; i < 8 && s_weather_icon_count < 10; i++) {
        float angle = i * 3.14159f / 4.0f;
        int rx = cx + (int)(cosf(angle) * ray_dist);
        int ry = cy + (int)(sinf(angle) * ray_dist);

        lv_obj_t *ray = lv_obj_create(scr);
        lv_obj_remove_style_all(ray);
        lv_obj_set_size(ray, ray_size, ray_size);
        lv_obj_set_pos(ray, rx - ray_size / 2, ry - ray_size / 2);
        lv_obj_set_style_bg_color(ray, lv_color_hex(COLOR_ACCENT_YELLOW), 0);
        lv_obj_set_style_bg_opa(ray, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ray, LV_RADIUS_CIRCLE, 0);  // Dots, not rectangles
        s_weather_icon_objs[s_weather_icon_count++] = ray;
    }
}

// Draw cloud icon (simple overlapping circles)
static void draw_cloud_icon(int cx, int cy, int size, uint32_t color)
{
    lv_obj_t *scr = lv_scr_act();

    // Cloud made of 3 overlapping circles - simple and clean
    int circles[][3] = {
        {-size/2, 0, size * 3/4},    // Left circle
        {0, -size/4, size},          // Center circle (larger, higher)
        {size/2, 0, size * 2/3},     // Right circle
    };

    for (int i = 0; i < 3 && s_weather_icon_count < 10; i++) {
        lv_obj_t *c = lv_obj_create(scr);
        lv_obj_remove_style_all(c);
        int r = circles[i][2] / 2;
        lv_obj_set_size(c, r * 2, r * 2);
        lv_obj_set_pos(c, cx + circles[i][0] - r, cy + circles[i][1] - r);
        lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        s_weather_icon_objs[s_weather_icon_count++] = c;
    }
}

// Draw rain drops (simple rounded rectangles)
static void draw_rain_drops(int cx, int cy, int spread)
{
    lv_obj_t *scr = lv_scr_act();

    // Rain drop positions (offset_x, offset_y)
    int drops[][2] = {
        {-spread, 0},     // Left
        {0, 15},          // Center (lower)
        {spread, 5},      // Right (middle height)
    };

    // Simple rounded rectangle drops
    for (int i = 0; i < 3 && s_weather_icon_count < 10; i++) {
        int dx = drops[i][0];
        int dy = drops[i][1];

        lv_obj_t *drop = lv_obj_create(scr);
        lv_obj_remove_style_all(drop);
        lv_obj_set_size(drop, 8, 22);
        lv_obj_set_pos(drop, cx + dx - 4, cy + dy);
        lv_obj_set_style_bg_color(drop, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_bg_opa(drop, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(drop, 4, 0);  // Rounded ends
        s_weather_icon_objs[s_weather_icon_count++] = drop;
    }
}

// Draw snowflakes
static void draw_snowflakes(int cx, int cy, int spread)
{
    lv_obj_t *scr = lv_scr_act();

    // 4 snowflakes (small circles)
    int positions[][2] = {{-spread, -10}, {spread, 0}, {0, 15}, {-spread/2, 5}};
    for (int i = 0; i < 4 && s_weather_icon_count < 10; i++) {
        lv_obj_t *flake = lv_obj_create(scr);
        lv_obj_remove_style_all(flake);
        lv_obj_set_size(flake, 10, 10);
        lv_obj_set_pos(flake, cx + positions[i][0] - 5, cy + positions[i][1] - 5);
        lv_obj_set_style_bg_color(flake, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(flake, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(flake, LV_RADIUS_CIRCLE, 0);
        s_weather_icon_objs[s_weather_icon_count++] = flake;
    }
}

void face_renderer_show_weather(const char *temp, weather_icon_t icon,
                                 const char *description)
{
    if (!s_renderer.initialized) return;

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (bsp_display_lock(500)) {
            // Set mode AFTER acquiring display lock to ensure hide runs
            s_renderer.mode = DISPLAY_MODE_WEATHER;
            // Hide ALL other screen elements first
            hide_all_screen_elements();

            lv_obj_t *scr = lv_scr_act();
            int center_x = s_renderer.width / 2;
            int center_y = s_renderer.height / 2;

            // Hide card (not needed for simple layout)
            if (s_weather_card) {
                lv_obj_add_flag(s_weather_card, LV_OBJ_FLAG_HIDDEN);
            }

            // Draw weather icon centered, above middle
            int icon_cy = center_y - 80;

            switch (icon) {
                case WEATHER_ICON_SUNNY:
                    draw_sun_icon(center_x, icon_cy, 45);
                    break;
                case WEATHER_ICON_CLOUDY:
                case WEATHER_ICON_PARTLY_CLOUDY:
                    draw_cloud_icon(center_x, icon_cy, 50, 0xB0BEC5);  // Gray cloud
                    break;
                case WEATHER_ICON_RAINY:
                case WEATHER_ICON_STORMY:
                    draw_cloud_icon(center_x, icon_cy - 15, 45, 0x78909C);  // Dark gray
                    draw_rain_drops(center_x, icon_cy + 10, 20);
                    break;
                case WEATHER_ICON_SNOWY:
                    draw_cloud_icon(center_x, icon_cy - 15, 45, 0xCFD8DC);  // Light gray
                    draw_snowflakes(center_x, icon_cy + 10, 25);
                    break;
                case WEATHER_ICON_FOGGY:
                    draw_cloud_icon(center_x, icon_cy, 50, 0x9E9E9E);  // Gray fog
                    break;
                default:
                    break;
            }

            // Temperature - large white text, centered
            lv_obj_clear_flag(s_renderer.text_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_renderer.text_label, temp);
#ifdef SIMULATOR
            lv_obj_set_style_text_font(s_renderer.text_label, &lv_font_montserrat_64, 0);
#else
            lv_obj_set_style_text_font(s_renderer.text_label, &lv_font_montserrat_48, 0);
#endif
            lv_obj_set_style_text_color(s_renderer.text_label, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
            lv_obj_set_style_text_align(s_renderer.text_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_transform_scale_x(s_renderer.text_label, 256, 0);
            lv_obj_set_style_transform_scale_y(s_renderer.text_label, 256, 0);
            lv_obj_set_width(s_renderer.text_label, s_renderer.width);
            lv_obj_align(s_renderer.text_label, LV_ALIGN_CENTER, 0, 30);

            // Description label - gray secondary text below temperature
            if (!s_weather_desc_label) {
                s_weather_desc_label = lv_label_create(scr);
            }
            if (description && description[0]) {
                lv_obj_clear_flag(s_weather_desc_label, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(s_weather_desc_label, description);
                lv_obj_set_style_text_color(s_weather_desc_label, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
                lv_obj_set_style_text_font(s_weather_desc_label, &lv_font_montserrat_28, 0);
                lv_obj_set_style_text_align(s_weather_desc_label, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_width(s_weather_desc_label, s_renderer.width);
                lv_obj_align(s_weather_desc_label, LV_ALIGN_CENTER, 0, 110);
            } else {
                lv_obj_add_flag(s_weather_desc_label, LV_OBJ_FLAG_HIDDEN);
            }

            bsp_display_unlock();
        }

        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Weather display: %s, icon=%d, desc=%s", temp, icon, description ? description : "none");
    }
}

void face_renderer_show_timer(int minutes, int seconds, const char *label,
                               bool is_running)
{
    if (!s_renderer.initialized) return;

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        // Store timer state (safe even without display lock)
        s_timer_minutes = minutes;
        s_timer_seconds = seconds;
        s_timer_running = is_running;

        // Initialize total if this is a new/larger time
        int current_total = minutes * 60 + seconds;
        if (current_total > s_timer_total_seconds_start || !s_timer_running) {
            s_timer_total_seconds_start = current_total;
        }

        if (bsp_display_lock(500)) {
            // Set mode AFTER acquiring display lock to ensure hide runs
            s_renderer.mode = DISPLAY_MODE_TIMER;
            // Hide ALL other screen elements first
            hide_all_screen_elements();

            lv_obj_t *scr = lv_scr_act();
            int arc_radius = 140;
            int arc_width = 20;  // Thicker stroke

            // Create timer arc if not exists
            if (!s_timer_arc) {
                s_timer_arc = lv_arc_create(scr);
                lv_arc_set_rotation(s_timer_arc, 270);  // Start from top
                lv_arc_set_bg_angles(s_timer_arc, 0, 360);
                lv_obj_remove_style(s_timer_arc, NULL, LV_PART_KNOB);  // Remove knob
                lv_obj_remove_flag(s_timer_arc, LV_OBJ_FLAG_CLICKABLE);
            }
            lv_obj_set_size(s_timer_arc, arc_radius * 2, arc_radius * 2);
            lv_obj_set_style_arc_width(s_timer_arc, arc_width, LV_PART_MAIN);
            lv_obj_set_style_arc_width(s_timer_arc, arc_width, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(s_timer_arc, lv_color_hex(COLOR_CARD_BG), LV_PART_MAIN);  // Dark card background
            lv_obj_align(s_timer_arc, LV_ALIGN_CENTER, 0, -30);
            lv_obj_clear_flag(s_timer_arc, LV_OBJ_FLAG_HIDDEN);

            // Calculate progress
            int current_seconds = minutes * 60 + seconds;
            int arc_angle = 0;
            if (s_timer_total_seconds_start > 0) {
                arc_angle = (current_seconds * 360) / s_timer_total_seconds_start;
            }
            lv_arc_set_angles(s_timer_arc, 0, arc_angle);

            // Arc color based on state - use Apple green for focus timer
            uint32_t arc_color = COLOR_ACCENT_GREEN;  // Apple system green for progress
            if (current_seconds <= 30 && current_seconds > 10 && is_running) {
                arc_color = COLOR_ACCENT_ORANGE;  // Apple orange when getting low
            } else if (current_seconds <= 10 && is_running) {
                arc_color = COLOR_ACCENT_RED;  // Apple red when very low
            }
            lv_obj_set_style_arc_color(s_timer_arc, lv_color_hex(arc_color), LV_PART_INDICATOR);

            // Time display - CENTERED INSIDE THE ARC
            char timer_text[64];
            snprintf(timer_text, sizeof(timer_text), "%02d:%02d", minutes, seconds);

            lv_obj_clear_flag(s_renderer.text_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_renderer.text_label, timer_text);
            lv_obj_set_style_text_color(s_renderer.text_label, lv_color_white(), 0);
            lv_obj_set_style_text_font(s_renderer.text_label, &lv_font_montserrat_48, 0);
            lv_obj_set_style_text_align(s_renderer.text_label, LV_TEXT_ALIGN_CENTER, 0);
            // Reset any scale transform from clock
            lv_obj_set_style_transform_scale_x(s_renderer.text_label, 256, 0);  // 256 = 1.0x normal
            lv_obj_set_style_transform_scale_y(s_renderer.text_label, 256, 0);
            lv_obj_set_width(s_renderer.text_label, s_renderer.width);
            lv_obj_align(s_renderer.text_label, LV_ALIGN_CENTER, 0, -30);  // Inside arc

            // "Focus" label in TOP-LEFT corner
            if (!s_timer_label_small) {
                s_timer_label_small = lv_label_create(scr);
            }
            lv_obj_clear_flag(s_timer_label_small, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_timer_label_small, label ? label : "Focus");
            lv_obj_set_style_text_color(s_timer_label_small, lv_color_hex(STYLE_TAG_COLOR), 0);
            lv_obj_set_style_text_font(s_timer_label_small, &lv_font_montserrat_20, 0);
            lv_obj_set_pos(s_timer_label_small, STYLE_TAG_POS_X, STYLE_TAG_POS_Y);

            // Create Start button
            if (!s_timer_btn_start) {
                s_timer_btn_start = lv_obj_create(scr);
                lv_obj_remove_style_all(s_timer_btn_start);
                lv_obj_set_size(s_timer_btn_start, 100, 45);
                lv_obj_set_style_bg_color(s_timer_btn_start, lv_color_hex(STYLE_BUTTON_ACTIVE), 0);
                lv_obj_set_style_bg_opa(s_timer_btn_start, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(s_timer_btn_start, 22, 0);

                // Make button clickable and register event callback
                lv_obj_add_flag(s_timer_btn_start, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(s_timer_btn_start, timer_btn_start_click_cb, LV_EVENT_CLICKED, NULL);

                s_timer_btn_label_start = lv_label_create(s_timer_btn_start);
                lv_label_set_text(s_timer_btn_label_start, "Start");
                lv_obj_set_style_text_color(s_timer_btn_label_start, lv_color_white(), 0);
                lv_obj_set_style_text_font(s_timer_btn_label_start, &lv_font_montserrat_20, 0);
                lv_obj_center(s_timer_btn_label_start);
            }
            lv_obj_align(s_timer_btn_start, LV_ALIGN_BOTTOM_MID, -60, -25);
            lv_obj_clear_flag(s_timer_btn_start, LV_OBJ_FLAG_HIDDEN);

            // Create Pause button
            if (!s_timer_btn_pause) {
                s_timer_btn_pause = lv_obj_create(scr);
                lv_obj_remove_style_all(s_timer_btn_pause);
                lv_obj_set_size(s_timer_btn_pause, 100, 45);
                lv_obj_set_style_bg_color(s_timer_btn_pause, lv_color_hex(STYLE_BUTTON_INACTIVE), 0);
                lv_obj_set_style_bg_opa(s_timer_btn_pause, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(s_timer_btn_pause, 22, 0);

                // Make button clickable and register event callback
                lv_obj_add_flag(s_timer_btn_pause, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(s_timer_btn_pause, timer_btn_pause_click_cb, LV_EVENT_CLICKED, NULL);

                s_timer_btn_label_pause = lv_label_create(s_timer_btn_pause);
                lv_label_set_text(s_timer_btn_label_pause, "Pause");
                lv_obj_set_style_text_color(s_timer_btn_label_pause, lv_color_white(), 0);
                lv_obj_set_style_text_font(s_timer_btn_label_pause, &lv_font_montserrat_20, 0);
                lv_obj_center(s_timer_btn_label_pause);
            }
            lv_obj_align(s_timer_btn_pause, LV_ALIGN_BOTTOM_MID, 60, -25);
            lv_obj_clear_flag(s_timer_btn_pause, LV_OBJ_FLAG_HIDDEN);

            // Update button states based on running - Apple Watch style
            if (is_running) {
                lv_obj_set_style_bg_color(s_timer_btn_start, lv_color_hex(COLOR_CARD_BG), 0);  // Dark inactive
                lv_obj_set_style_bg_color(s_timer_btn_pause, lv_color_hex(COLOR_ACCENT_ORANGE), 0);  // Orange pause
            } else {
                lv_obj_set_style_bg_color(s_timer_btn_start, lv_color_hex(COLOR_ACCENT_GREEN), 0);  // Green start
                lv_obj_set_style_bg_color(s_timer_btn_pause, lv_color_hex(COLOR_CARD_BG), 0);  // Dark inactive
            }

            bsp_display_unlock();
        }

        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Timer display: %02d:%02d %s running=%d", minutes, seconds,
                 label ? label : "Focus", is_running);
    }
}

// Timer control functions
void face_renderer_timer_start(void)
{
    s_timer_running = true;
    s_timer_last_tick = esp_timer_get_time() / 1000;
    face_renderer_show_timer(s_timer_minutes, s_timer_seconds, "Focus", true);
}

void face_renderer_timer_pause(void)
{
    s_timer_running = false;
    face_renderer_show_timer(s_timer_minutes, s_timer_seconds, "Focus", false);
}

void face_renderer_timer_reset(int minutes)
{
    s_timer_minutes = minutes;
    s_timer_seconds = 0;
    s_timer_total_seconds_start = minutes * 60;
    s_timer_running = false;
    face_renderer_show_timer(s_timer_minutes, s_timer_seconds, "Focus", false);
}

bool face_renderer_timer_is_running(void)
{
    return s_timer_running;
}

// Timer button click event callbacks (for LVGL touch/click events)
static void timer_btn_start_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Start button clicked");
        face_renderer_timer_start();
    }
}

static void timer_btn_pause_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Pause button clicked");
        face_renderer_timer_pause();
    }
}

void face_renderer_show_clock(int hours, int minutes, bool is_24h, const char *date_str)
{
    if (!s_renderer.initialized) return;

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (bsp_display_lock(500)) {
            // Set mode AFTER acquiring display lock to ensure hide runs
            s_renderer.mode = DISPLAY_MODE_CLOCK;
            hide_all_screen_elements();

            lv_obj_t *scr = lv_scr_act();
            int center_y = s_renderer.height / 2;

            // Format time
            char time_text[32];
            int display_hours = hours;
            if (!is_24h) {
                display_hours = hours % 12;
                if (display_hours == 0) display_hours = 12;
            }
            snprintf(time_text, sizeof(time_text), "%d:%02d", display_hours, minutes);

            // Hide card (not needed for simple layout)
            if (s_clock_card) {
                lv_obj_add_flag(s_clock_card, LV_OBJ_FLAG_HIDDEN);
            }

            // Date label at top (orange accent)
            if (!s_clock_date_label) {
                s_clock_date_label = lv_label_create(scr);
            }
            if (date_str && date_str[0]) {
                lv_obj_clear_flag(s_clock_date_label, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(s_clock_date_label, date_str);
                lv_obj_set_style_text_color(s_clock_date_label, lv_color_hex(COLOR_ACCENT_ORANGE), 0);
                lv_obj_set_style_text_font(s_clock_date_label, &lv_font_montserrat_28, 0);
                lv_obj_set_style_text_align(s_clock_date_label, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_width(s_clock_date_label, s_renderer.width);
                lv_obj_align(s_clock_date_label, LV_ALIGN_CENTER, 0, -80);
            } else {
                lv_obj_add_flag(s_clock_date_label, LV_OBJ_FLAG_HIDDEN);
            }

            // Main time display - large white text, centered
            lv_obj_clear_flag(s_renderer.text_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_renderer.text_label, time_text);
            lv_obj_set_style_text_color(s_renderer.text_label, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
#ifdef SIMULATOR
            lv_obj_set_style_text_font(s_renderer.text_label, &lv_font_montserrat_64, 0);
#else
            lv_obj_set_style_text_font(s_renderer.text_label, &lv_font_montserrat_48, 0);
#endif
            lv_obj_set_style_text_align(s_renderer.text_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_transform_scale_x(s_renderer.text_label, 256, 0);
            lv_obj_set_style_transform_scale_y(s_renderer.text_label, 256, 0);
            lv_obj_set_width(s_renderer.text_label, s_renderer.width);
            lv_obj_align(s_renderer.text_label, LV_ALIGN_CENTER, 0, 0);

            // AM/PM label (below time)
            if (!s_clock_ampm_label) {
                s_clock_ampm_label = lv_label_create(scr);
            }
            if (!is_24h) {
                const char* ampm = hours >= 12 ? "PM" : "AM";
                lv_obj_clear_flag(s_clock_ampm_label, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(s_clock_ampm_label, ampm);
                lv_obj_set_style_text_color(s_clock_ampm_label, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
                lv_obj_set_style_text_font(s_clock_ampm_label, &lv_font_montserrat_28, 0);
                lv_obj_set_style_text_align(s_clock_ampm_label, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_width(s_clock_ampm_label, s_renderer.width);
                lv_obj_align(s_clock_ampm_label, LV_ALIGN_CENTER, 0, 70);
            } else {
                lv_obj_add_flag(s_clock_ampm_label, LV_OBJ_FLAG_HIDDEN);
            }

            bsp_display_unlock();
        }

        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Clock display: %02d:%02d (24h=%d, date=%s)", hours, minutes, is_24h, date_str ? date_str : "none");
    }
}

void face_renderer_show_subway(const char *line, uint32_t line_color,
                                const char *station, const char *direction,
                                const int *times, int num_times)
{
    if (!s_renderer.initialized) return;
    if (num_times < 1 || num_times > 3) num_times = 1;

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (bsp_display_lock(500)) {
            // Set mode AFTER acquiring display lock to ensure hide runs
            s_renderer.mode = DISPLAY_MODE_SUBWAY;
            hide_all_screen_elements();

            lv_obj_t *scr = lv_scr_act();
            int center_x = s_renderer.width / 2;
            int center_y = s_renderer.height / 2;

            // Card dimensions (Apple Watch widget style)
            int card_width = s_renderer.width - 40;
            int card_height = 320;
            int card_x = 20;
            int card_y = center_y - card_height / 2;

            // Create card background
            if (!s_subway_card) {
                s_subway_card = lv_obj_create(scr);
                lv_obj_remove_style_all(s_subway_card);
            }
            lv_obj_set_size(s_subway_card, card_width, card_height);
            lv_obj_set_pos(s_subway_card, card_x, card_y);
            lv_obj_set_style_bg_color(s_subway_card, lv_color_hex(COLOR_CARD_BG), 0);
            lv_obj_set_style_bg_opa(s_subway_card, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s_subway_card, CARD_RADIUS, 0);
            lv_obj_set_style_border_width(s_subway_card, 0, 0);
            lv_obj_clear_flag(s_subway_card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(s_subway_card, LV_OBJ_FLAG_HIDDEN);

            // Positions relative to card
            int content_y = card_y + 25;

            // Train line circle (smaller MTA bullet on left side of card)
            int circle_radius = 30;
            if (!s_subway_circle) {
                s_subway_circle = lv_obj_create(scr);
                lv_obj_remove_style_all(s_subway_circle);
            }
            lv_obj_set_size(s_subway_circle, circle_radius * 2, circle_radius * 2);
            lv_obj_set_style_bg_color(s_subway_circle, lv_color_hex(line_color), 0);
            lv_obj_set_style_bg_opa(s_subway_circle, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s_subway_circle, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_pos(s_subway_circle, card_x + 25, content_y);
            lv_obj_clear_flag(s_subway_circle, LV_OBJ_FLAG_HIDDEN);

            // Line name inside circle (e.g., "1", "A", "N")
            if (!s_subway_line_label) {
                s_subway_line_label = lv_label_create(scr);
            }
            lv_label_set_text(s_subway_line_label, line);
            lv_obj_set_style_text_color(s_subway_line_label, lv_color_white(), 0);
            lv_obj_set_style_text_font(s_subway_line_label, &lv_font_montserrat_28, 0);
            lv_obj_set_style_text_align(s_subway_line_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(s_subway_line_label, card_x + 25, content_y + circle_radius - 14);
            lv_obj_set_width(s_subway_line_label, circle_radius * 2);
            lv_obj_clear_flag(s_subway_line_label, LV_OBJ_FLAG_HIDDEN);

            // Station name and direction (right of bullet)
            char station_text[64];
            snprintf(station_text, sizeof(station_text), "%s", station);
            if (!s_subway_station_label) {
                s_subway_station_label = lv_label_create(scr);
            }
            lv_label_set_text(s_subway_station_label, station_text);
            lv_obj_set_style_text_color(s_subway_station_label, lv_color_white(), 0);
            lv_obj_set_style_text_font(s_subway_station_label, &lv_font_montserrat_28, 0);
            lv_obj_set_pos(s_subway_station_label, card_x + 25 + circle_radius * 2 + 15, content_y + 5);
            lv_obj_clear_flag(s_subway_station_label, LV_OBJ_FLAG_HIDDEN);

            // Direction below station (gray text)
            if (!s_subway_time_labels[2]) {
                s_subway_time_labels[2] = lv_label_create(scr);
            }
            lv_label_set_text(s_subway_time_labels[2], direction);
            lv_obj_set_style_text_color(s_subway_time_labels[2], lv_color_hex(COLOR_TEXT_SECONDARY), 0);
            lv_obj_set_style_text_font(s_subway_time_labels[2], &lv_font_montserrat_20, 0);
            lv_obj_set_pos(s_subway_time_labels[2], card_x + 25 + circle_radius * 2 + 15, content_y + 35);
            lv_obj_clear_flag(s_subway_time_labels[2], LV_OBJ_FLAG_HIDDEN);

            // Arrival times - prominently displayed in center/bottom of card
            int time_y = content_y + 95;

            // First arrival time - large and prominent with "min" using 48pt (has lowercase)
            if (!s_subway_time_labels[0]) {
                s_subway_time_labels[0] = lv_label_create(scr);
            }
            char time_text[32];
            if (num_times > 0) {
                if (times[0] <= 0) {
                    snprintf(time_text, sizeof(time_text), "NOW");
                } else {
                    snprintf(time_text, sizeof(time_text), "%d min", times[0]);  // Include "min"
                }
                lv_label_set_text(s_subway_time_labels[0], time_text);
                lv_obj_set_style_text_font(s_subway_time_labels[0], &lv_font_montserrat_48, 0);  // 48pt has lowercase
                lv_obj_set_style_text_color(s_subway_time_labels[0], lv_color_white(), 0);
                lv_obj_set_style_text_align(s_subway_time_labels[0], LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_width(s_subway_time_labels[0], card_width);
                lv_obj_set_pos(s_subway_time_labels[0], card_x, time_y);
                lv_obj_clear_flag(s_subway_time_labels[0], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_subway_time_labels[0], LV_OBJ_FLAG_HIDDEN);
            }

            // Following arrivals (smaller, below main time)
            if (!s_subway_time_labels[1]) {
                s_subway_time_labels[1] = lv_label_create(scr);
            }
            if (num_times > 1) {
                char next_times[64] = "";
                for (int i = 1; i < num_times && i < 3; i++) {
                    char buf[16];
                    if (times[i] <= 0) {
                        snprintf(buf, sizeof(buf), "NOW");
                    } else {
                        snprintf(buf, sizeof(buf), "%d min", times[i]);
                    }
                    if (i > 1) strcat(next_times, "  •  ");
                    strcat(next_times, buf);
                }
                lv_label_set_text(s_subway_time_labels[1], next_times);
                lv_obj_set_style_text_font(s_subway_time_labels[1], &lv_font_montserrat_24, 0);
                lv_obj_set_style_text_color(s_subway_time_labels[1], lv_color_hex(COLOR_TEXT_SECONDARY), 0);
                lv_obj_set_style_text_align(s_subway_time_labels[1], LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_width(s_subway_time_labels[1], card_width);
                lv_obj_set_pos(s_subway_time_labels[1], card_x, time_y + 80);
                lv_obj_clear_flag(s_subway_time_labels[1], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_subway_time_labels[1], LV_OBJ_FLAG_HIDDEN);
            }

            bsp_display_unlock();
        }

        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Subway display: %s line at %s %s, %d arrivals",
                 line, station, direction, num_times);
    }
}

void face_renderer_show_calendar(const calendar_event_t *events, int num_events)
{
    if (!s_renderer.initialized || !events) return;
    if (num_events < 1) num_events = 1;
    if (num_events > MAX_CALENDAR_CARDS) num_events = MAX_CALENDAR_CARDS;

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (bsp_display_lock(500)) {
            // Set mode AFTER acquiring display lock to ensure hide_all_screen_elements runs
            s_renderer.mode = DISPLAY_MODE_CALENDAR;
            hide_all_screen_elements();

            lv_obj_t *scr = lv_scr_act();

            // Card dimensions - Apple Watch style
            int card_width = s_renderer.width - 40;  // 20px margin each side
            int card_height = (num_events == 1) ? 180 :
                              (num_events == 2) ? 130 : 100;  // Adjust height based on count
            int card_spacing = 12;
            int start_y = 30;  // Start below top

            for (int i = 0; i < num_events; i++) {
                // Create card container
                int card_y = start_y + i * (card_height + card_spacing);
                s_calendar_cards[i] = create_card(scr, 20, card_y, card_width, card_height);

                // Time label (blue accent at top of card)
                s_calendar_time_labels[i] = lv_label_create(s_calendar_cards[i]);
                lv_label_set_text(s_calendar_time_labels[i], events[i].time_str);
                lv_obj_set_style_text_color(s_calendar_time_labels[i], lv_color_hex(COLOR_ACCENT_BLUE), 0);
                lv_obj_set_style_text_font(s_calendar_time_labels[i], &lv_font_montserrat_20, 0);
                lv_obj_align(s_calendar_time_labels[i], LV_ALIGN_TOP_LEFT, 0, 0);

                // Title label (white, bold - using larger font)
                s_calendar_title_labels[i] = lv_label_create(s_calendar_cards[i]);
                lv_label_set_text(s_calendar_title_labels[i], events[i].title);
                lv_obj_set_style_text_color(s_calendar_title_labels[i], lv_color_hex(COLOR_TEXT_PRIMARY), 0);
                lv_obj_set_style_text_font(s_calendar_title_labels[i], &lv_font_montserrat_28, 0);
                lv_obj_set_width(s_calendar_title_labels[i], card_width - 2 * CARD_PADDING);
                lv_label_set_long_mode(s_calendar_title_labels[i], LV_LABEL_LONG_WRAP);
                lv_obj_align(s_calendar_title_labels[i], LV_ALIGN_TOP_LEFT, 0, 26);

                // Location label (gray secondary text, if provided)
                if (events[i].location[0] != '\0') {
                    s_calendar_location_labels[i] = lv_label_create(s_calendar_cards[i]);
                    lv_label_set_text(s_calendar_location_labels[i], events[i].location);
                    lv_obj_set_style_text_color(s_calendar_location_labels[i], lv_color_hex(COLOR_TEXT_SECONDARY), 0);
                    lv_obj_set_style_text_font(s_calendar_location_labels[i], &lv_font_montserrat_20, 0);
                    lv_obj_set_width(s_calendar_location_labels[i], card_width - 2 * CARD_PADDING);
                    lv_label_set_long_mode(s_calendar_location_labels[i], LV_LABEL_LONG_DOT);
                    lv_obj_align(s_calendar_location_labels[i], LV_ALIGN_TOP_LEFT, 0,
                                 num_events == 1 ? 70 : 58);
                }
            }

            bsp_display_unlock();
            ESP_LOGI(TAG, "Calendar display: %d events", num_events);
        } else {
            ESP_LOGE(TAG, "Failed to acquire display lock for calendar");
        }

        xSemaphoreGive(s_renderer.mutex);
    }
}

// Initialize particles for animation
static void init_particles(animation_type_t type)
{
    lv_obj_t *scr = lv_scr_act();
    s_current_animation = type;

    // Particle appearance based on type
    uint32_t color = 0x4FC3F7;  // Default light blue for rain
    int size = 6;

    switch (type) {
        case ANIMATION_RAIN:
            color = COLOR_SKYBLUE;  // Sky blue for rain
            size = 4;
            break;
        case ANIMATION_SNOW:
            color = 0xFFFFFF;  // Pure white for snow
            size = 8;
            break;
        case ANIMATION_STARS:
            color = COLOR_SUNSHINE;  // Sunshine for stars
            size = 6;
            break;
        case ANIMATION_MATRIX:
            color = COLOR_MOSS;  // Moss green for matrix
            size = 10;
            break;
    }

    // Create particles
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!s_particles[i]) {
            s_particles[i] = lv_obj_create(scr);
            lv_obj_remove_style_all(s_particles[i]);
        }

        // Random initial position
        s_particle_x[i] = (float)(esp_random() % s_renderer.width);
        s_particle_y[i] = (float)(esp_random() % s_renderer.height);

        // Random speed
        float base_speed = (type == ANIMATION_SNOW) ? 30.0f : 100.0f;
        s_particle_speed[i] = base_speed + (esp_random() % 50);

        // Set particle appearance
        int particle_size = size + (esp_random() % 4) - 2;
        if (particle_size < 2) particle_size = 2;

        lv_obj_set_size(s_particles[i], particle_size,
            (type == ANIMATION_RAIN) ? particle_size * 3 : particle_size);
        lv_obj_set_pos(s_particles[i], (int)s_particle_x[i], (int)s_particle_y[i]);
        lv_obj_set_style_bg_color(s_particles[i], lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(s_particles[i], LV_OPA_COVER, 0);

        if (type == ANIMATION_STARS || type == ANIMATION_SNOW) {
            lv_obj_set_style_radius(s_particles[i], LV_RADIUS_CIRCLE, 0);
        } else {
            lv_obj_set_style_radius(s_particles[i], 2, 0);
        }

        lv_obj_clear_flag(s_particles[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_animation_active = true;
}

// Update particle animation (called from tick)
static void update_particles(float delta_time)
{
    if (!s_animation_active) return;

    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!s_particles[i]) continue;

        switch (s_current_animation) {
            case ANIMATION_RAIN:
            case ANIMATION_SNOW:
                // Fall down
                s_particle_y[i] += s_particle_speed[i] * delta_time;
                // Add slight horizontal drift for snow
                if (s_current_animation == ANIMATION_SNOW) {
                    s_particle_x[i] += sinf(s_particle_y[i] * 0.02f) * 20.0f * delta_time;
                }
                // Wrap around
                if (s_particle_y[i] > s_renderer.height) {
                    s_particle_y[i] = -10;
                    s_particle_x[i] = (float)(esp_random() % s_renderer.width);
                }
                if (s_particle_x[i] < 0) s_particle_x[i] += s_renderer.width;
                if (s_particle_x[i] > s_renderer.width) s_particle_x[i] -= s_renderer.width;
                break;

            case ANIMATION_STARS:
                // Twinkle (change opacity)
                {
                    int opa = 128 + (int)(sinf(s_particle_y[i] + s_particle_speed[i] * 0.1f) * 127);
                    s_particle_y[i] += delta_time * 50;  // Slow drift for twinkle phase
                    lv_obj_set_style_bg_opa(s_particles[i], opa, 0);
                }
                break;

            case ANIMATION_MATRIX:
                // Fall down fast
                s_particle_y[i] += s_particle_speed[i] * delta_time * 2;
                if (s_particle_y[i] > s_renderer.height) {
                    s_particle_y[i] = -20;
                    s_particle_x[i] = (float)(esp_random() % s_renderer.width);
                    s_particle_speed[i] = 100.0f + (esp_random() % 100);
                }
                break;
        }

        lv_obj_set_pos(s_particles[i], (int)s_particle_x[i], (int)s_particle_y[i]);
    }
}

void face_renderer_show_animation(animation_type_t type)
{
    if (!s_renderer.initialized) return;

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (bsp_display_lock(500)) {
            // Set mode AFTER acquiring display lock to ensure hide runs
            s_renderer.mode = DISPLAY_MODE_ANIMATION;
            // Hide ALL other screen elements first
            hide_all_screen_elements();

            // Show animation type as tag in top-left
            const char* tag_text = "Animation";
            switch (type) {
                case ANIMATION_RAIN: tag_text = "Rain"; break;
                case ANIMATION_SNOW: tag_text = "Snow"; break;
                case ANIMATION_STARS: tag_text = "Stars"; break;
                case ANIMATION_MATRIX: tag_text = "Matrix"; break;
            }
            show_screen_tag(tag_text);

            // Initialize new particles
            init_particles(type);

            bsp_display_unlock();
        }

        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Animation display: type=%d", type);
    }
}

void face_renderer_clear_display(void)
{
    if (!s_renderer.initialized) return;

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        display_mode_t prev_mode = s_renderer.mode;
        s_renderer.mode = DISPLAY_MODE_FACE;

        if (bsp_display_lock(500)) {
            // Hide ALL screen elements
            hide_all_screen_elements();

            // Show face widgets
            lv_obj_clear_flag(s_renderer.left_eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_renderer.right_eye, LV_OBJ_FLAG_HIDDEN);

            // Update background
            lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(BG_COLOR), 0);

            // Force full redraw
            s_renderer.last_eye_x = -1000;
            s_renderer.last_mouth_curve = -1000;

            bsp_display_unlock();
        }

        xSemaphoreGive(s_renderer.mutex);
        ESP_LOGI(TAG, "Display cleared (was mode %d)", prev_mode);
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

void face_renderer_tick(uint32_t delta_time_ms)
{
    if (!s_renderer.initialized) {
        return;
    }

    // Use minimum 5ms delta to ensure animations progress
    if (delta_time_ms < 5) delta_time_ms = 5;

    float delta_time = delta_time_ms / 1000.0f;

    // Clamp delta time
    if (delta_time > 0.1f) delta_time = 0.1f;

    // Take mutex (non-blocking in simulator - just try once)
    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_renderer.mode == DISPLAY_MODE_FACE) {
            // Update animation state (blink, emotion transition, gaze)
            update_animation(delta_time);

            // Update petting (touch-based face movement)
            // Now works in simulator too since bsp_display_get_input_dev() returns SDL mouse
            update_petting(delta_time);

            // Update face widgets
            update_face_widgets();
        } else if (s_renderer.mode == DISPLAY_MODE_ANIMATION) {
            // Update particle animation
            if (bsp_display_lock(10)) {
                update_particles(delta_time);
                bsp_display_unlock();
            }
        } else if (s_renderer.mode == DISPLAY_MODE_TIMER && s_timer_running) {
            // Update timer countdown
            int64_t now = esp_timer_get_time() / 1000;
            if (s_timer_last_tick == 0) {
                s_timer_last_tick = now;
            }

            int64_t elapsed = now - s_timer_last_tick;
            if (elapsed >= 1000) {  // 1 second passed
                s_timer_last_tick = now;

                // Decrement timer
                if (s_timer_seconds > 0) {
                    s_timer_seconds--;
                } else if (s_timer_minutes > 0) {
                    s_timer_minutes--;
                    s_timer_seconds = 59;
                } else {
                    // Timer finished
                    s_timer_running = false;
                }

                // Update display (release mutex first to avoid deadlock)
                xSemaphoreGive(s_renderer.mutex);
                face_renderer_show_timer(s_timer_minutes, s_timer_seconds, "Focus", s_timer_running);
                return;  // Already released mutex
            }
        }
        xSemaphoreGive(s_renderer.mutex);
    }
}
