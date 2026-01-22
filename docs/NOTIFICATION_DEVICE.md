# Luna Device Notification Display Specification

## Overview

This document specifies how the ESP32 device (and simulator) displays notifications received from the server via WebSocket.

## Display Mode

Add new display mode: `DISPLAY_MODE_NOTIFICATION`

```c
typedef enum {
    DISPLAY_MODE_FACE,
    DISPLAY_MODE_TEXT,
    DISPLAY_MODE_PIXEL_ART,
    DISPLAY_MODE_WEATHER,
    DISPLAY_MODE_TIMER,
    DISPLAY_MODE_CLOCK,
    DISPLAY_MODE_ANIMATION,
    DISPLAY_MODE_SUBWAY,
    DISPLAY_MODE_CALENDAR,
    DISPLAY_MODE_NOTIFICATION,  // NEW
} display_mode_t;
```

## UI Design

### Single Notification Card

Similar to Apple Watch notification style:

```
┌─────────────────────────────────┐
│  ┌───┐                          │
│  │ @ │  Mail              10:30 │  <- App icon, name, time
│  └───┘                          │
├─────────────────────────────────┤
│                                 │
│  John Doe                       │  <- Title (bold, white)
│                                 │
│  Hey, are you free for lunch    │  <- Body (gray, smaller)
│  tomorrow? I was thinking...    │
│                                 │
└─────────────────────────────────┘
        [Dismiss]                     <- Touch to dismiss
```

### Notification Queue View

When multiple notifications pending:

```
┌─────────────────────────────────┐
│           3 Notifications       │
├─────────────────────────────────┤
│  @ Mail      John Doe     10:30 │
│  # Slack     @channel     10:28 │
│  * Calendar  Meeting      10:25 │
└─────────────────────────────────┘
     [<]  1 of 3  [>]              <- Swipe navigation
```

## Data Structures

### C Header (`face_renderer.h`)

```c
/**
 * @brief Notification priority
 */
typedef enum {
    NOTIFY_PRIORITY_LOW,
    NOTIFY_PRIORITY_NORMAL,
    NOTIFY_PRIORITY_HIGH,
    NOTIFY_PRIORITY_CRITICAL,
} notification_priority_t;

/**
 * @brief App icon type (for built-in icons)
 */
typedef enum {
    APP_ICON_GENERIC,
    APP_ICON_MAIL,
    APP_ICON_MESSAGE,
    APP_ICON_CALENDAR,
    APP_ICON_SLACK,
    APP_ICON_PHONE,
    APP_ICON_REMINDER,
} app_icon_t;

/**
 * @brief Notification structure
 */
typedef struct {
    char id[64];                      // Unique ID
    char app_name[32];                // App display name
    app_icon_t app_icon;              // Icon type
    char title[64];                   // Notification title
    char body[256];                   // Body text (truncated if needed)
    char timestamp[16];               // Display time (e.g., "10:30 AM")
    notification_priority_t priority; // Priority level
} notification_t;

/**
 * @brief Show a notification
 * @param notification Notification to display
 * @param interrupt If true, immediately switch to notification mode
 */
void face_renderer_show_notification(const notification_t *notification, bool interrupt);

/**
 * @brief Dismiss current notification
 * @return ID of dismissed notification, or NULL if none
 */
const char* face_renderer_dismiss_notification(void);

/**
 * @brief Clear all notifications
 */
void face_renderer_clear_notifications(void);

/**
 * @brief Get notification queue count
 * @return Number of pending notifications
 */
int face_renderer_notification_count(void);

/**
 * @brief Navigate to next notification in queue
 */
void face_renderer_next_notification(void);

/**
 * @brief Navigate to previous notification in queue
 */
void face_renderer_prev_notification(void);
```

## WebSocket Protocol

### Receiving Notifications

**Server -> Device:**
```json
{
  "cmd": "notification",
  "id": "uuid-123",
  "app": "Mail",
  "app_icon": "mail",
  "title": "John Doe",
  "body": "Hey, are you free for lunch tomorrow?",
  "timestamp": "10:30 AM",
  "priority": "normal"
}
```

**Handling in `luna_protocol.c`:**
```c
static void handle_notification_cmd(cJSON *json) {
    notification_t notif = {0};

    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(json, "id"));
    const char *app = cJSON_GetStringValue(cJSON_GetObjectItem(json, "app"));
    const char *icon = cJSON_GetStringValue(cJSON_GetObjectItem(json, "app_icon"));
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(json, "title"));
    const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(json, "body"));
    const char *time = cJSON_GetStringValue(cJSON_GetObjectItem(json, "timestamp"));
    const char *prio = cJSON_GetStringValue(cJSON_GetObjectItem(json, "priority"));

    if (id) strncpy(notif.id, id, sizeof(notif.id) - 1);
    if (app) strncpy(notif.app_name, app, sizeof(notif.app_name) - 1);
    if (title) strncpy(notif.title, title, sizeof(notif.title) - 1);
    if (body) strncpy(notif.body, body, sizeof(notif.body) - 1);
    if (time) strncpy(notif.timestamp, time, sizeof(notif.timestamp) - 1);

    // Map icon string to enum
    notif.app_icon = map_icon_string(icon);

    // Map priority
    if (prio && strcmp(prio, "high") == 0) {
        notif.priority = NOTIFY_PRIORITY_HIGH;
    } else if (prio && strcmp(prio, "critical") == 0) {
        notif.priority = NOTIFY_PRIORITY_CRITICAL;
    } else {
        notif.priority = NOTIFY_PRIORITY_NORMAL;
    }

    // Show notification (interrupt for high/critical)
    bool interrupt = (notif.priority >= NOTIFY_PRIORITY_HIGH);
    face_renderer_show_notification(&notif, interrupt);
}
```

### Dismissing Notifications

**Device -> Server:**
```json
{
  "cmd": "notification_dismiss",
  "id": "uuid-123"
}
```

## Rendering Implementation

### Notification Card Drawing (`face_renderer.c`)

```c
// Notification queue
#define MAX_NOTIFICATIONS 10
static notification_t s_notification_queue[MAX_NOTIFICATIONS];
static int s_notification_count = 0;
static int s_current_notification_idx = 0;

// UI elements
static lv_obj_t *s_notif_card = NULL;
static lv_obj_t *s_notif_icon = NULL;
static lv_obj_t *s_notif_app_name = NULL;
static lv_obj_t *s_notif_time = NULL;
static lv_obj_t *s_notif_title = NULL;
static lv_obj_t *s_notif_body = NULL;
static lv_obj_t *s_notif_counter = NULL;

static void render_notification_mode(void) {
    if (s_notification_count == 0) {
        // No notifications, return to face
        face_renderer_clear_display();
        return;
    }

    notification_t *notif = &s_notification_queue[s_current_notification_idx];
    lv_obj_t *scr = lv_scr_act();

    // Clear previous
    clear_notification_ui();

    // Card background (rounded rectangle, dark gray)
    s_notif_card = lv_obj_create(scr);
    lv_obj_remove_style_all(s_notif_card);
    lv_obj_set_size(s_notif_card, 380, 420);
    lv_obj_center(s_notif_card);
    lv_obj_set_style_bg_color(s_notif_card, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(s_notif_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_notif_card, 24, 0);

    // App icon (simplified - colored circle with letter)
    int icon_y = 40;
    s_notif_icon = lv_obj_create(scr);
    lv_obj_remove_style_all(s_notif_icon);
    lv_obj_set_size(s_notif_icon, 40, 40);
    lv_obj_set_pos(s_notif_icon, 40, icon_y);
    lv_obj_set_style_bg_color(s_notif_icon, get_icon_color(notif->app_icon), 0);
    lv_obj_set_style_bg_opa(s_notif_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_notif_icon, 10, 0);

    // App name
    s_notif_app_name = lv_label_create(scr);
    lv_label_set_text(s_notif_app_name, notif->app_name);
    lv_obj_set_pos(s_notif_app_name, 90, icon_y + 8);
    lv_obj_set_style_text_color(s_notif_app_name, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(s_notif_app_name, &lv_font_montserrat_20, 0);

    // Timestamp (right-aligned)
    s_notif_time = lv_label_create(scr);
    lv_label_set_text(s_notif_time, notif->timestamp);
    lv_obj_set_pos(s_notif_time, 300, icon_y + 8);
    lv_obj_set_style_text_color(s_notif_time, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(s_notif_time, &lv_font_montserrat_20, 0);

    // Title
    s_notif_title = lv_label_create(scr);
    lv_label_set_text(s_notif_title, notif->title);
    lv_obj_set_pos(s_notif_title, 40, 100);
    lv_obj_set_style_text_color(s_notif_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_notif_title, &lv_font_montserrat_28, 0);

    // Body (with word wrap)
    s_notif_body = lv_label_create(scr);
    lv_label_set_text(s_notif_body, notif->body);
    lv_obj_set_pos(s_notif_body, 40, 140);
    lv_obj_set_width(s_notif_body, 320);
    lv_label_set_long_mode(s_notif_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_notif_body, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(s_notif_body, &lv_font_montserrat_20, 0);

    // Counter (if multiple notifications)
    if (s_notification_count > 1) {
        s_notif_counter = lv_label_create(scr);
        char counter[32];
        snprintf(counter, sizeof(counter), "%d of %d",
                 s_current_notification_idx + 1, s_notification_count);
        lv_label_set_text(s_notif_counter, counter);
        lv_obj_set_pos(s_notif_counter, 170, 450);
        lv_obj_set_style_text_color(s_notif_counter, lv_color_hex(0x8E8E93), 0);
        lv_obj_set_style_text_font(s_notif_counter, &lv_font_montserrat_14, 0);
    }
}

static lv_color_t get_icon_color(app_icon_t icon) {
    switch (icon) {
        case APP_ICON_MAIL:     return lv_color_hex(0x007AFF);  // Blue
        case APP_ICON_MESSAGE:  return lv_color_hex(0x34C759);  // Green
        case APP_ICON_CALENDAR: return lv_color_hex(0xFF3B30);  // Red
        case APP_ICON_SLACK:    return lv_color_hex(0x4A154B);  // Purple
        case APP_ICON_PHONE:    return lv_color_hex(0x34C759);  // Green
        case APP_ICON_REMINDER: return lv_color_hex(0xFF9500);  // Orange
        default:                return lv_color_hex(0x8E8E93);  // Gray
    }
}
```

## Touch Interactions

### Gestures

| Gesture | Action |
|---------|--------|
| Tap anywhere | Dismiss current notification |
| Swipe left | Next notification |
| Swipe right | Previous notification |
| Swipe down | Return to face mode |
| Long press | Clear all notifications |

### Touch Handler

```c
static void notification_touch_handler(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        // Dismiss current notification
        const char *id = face_renderer_dismiss_notification();
        if (id) {
            // Send dismiss to server
            send_notification_dismiss(id);
        }
    } else if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT) {
            face_renderer_next_notification();
        } else if (dir == LV_DIR_RIGHT) {
            face_renderer_prev_notification();
        } else if (dir == LV_DIR_BOTTOM) {
            face_renderer_clear_display();  // Return to face
        }
    }
}
```

## Animation

### Notification Arrival Animation

When a new notification arrives:

1. If in face mode and priority >= normal:
   - Slide notification card in from top (200ms ease-out)
   - Optional: brief "ding" sound effect

2. If in another display mode:
   - Show small notification indicator at top
   - Don't interrupt unless priority is high/critical

```c
static void animate_notification_in(void) {
    // Start off-screen
    lv_obj_set_y(s_notif_card, -500);

    // Animate to center
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_notif_card);
    lv_anim_set_values(&a, -500, 40);  // From top to position
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_start(&a);
}
```

### Dismissal Animation

```c
static void animate_notification_dismiss(void) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_notif_card);
    lv_anim_set_values(&a, lv_obj_get_y(s_notif_card), -500);
    lv_anim_set_time(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_ready_cb(&a, notification_dismissed_cb);
    lv_anim_start(&a);
}
```

## Notification Indicator (Non-Interrupt)

When notification arrives but shouldn't interrupt current mode, show small indicator:

```c
static lv_obj_t *s_notif_indicator = NULL;

static void show_notification_indicator(int count) {
    if (!s_notif_indicator) {
        s_notif_indicator = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(s_notif_indicator);
        lv_obj_set_size(s_notif_indicator, 24, 24);
        lv_obj_set_pos(s_notif_indicator, 380, 10);
        lv_obj_set_style_bg_color(s_notif_indicator, lv_color_hex(0xFF3B30), 0);
        lv_obj_set_style_bg_opa(s_notif_indicator, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_notif_indicator, LV_RADIUS_CIRCLE, 0);
    }

    // Update count label
    // ...
}
```

## Sound Effects (ESP32 Only)

Optional notification sounds:

```c
// In audio_playback.c
void play_notification_sound(notification_priority_t priority) {
    switch (priority) {
        case NOTIFY_PRIORITY_HIGH:
        case NOTIFY_PRIORITY_CRITICAL:
            // Play alert tone
            play_tone(880, 100);  // A5, 100ms
            vTaskDelay(pdMS_TO_TICKS(50));
            play_tone(1100, 100); // C#6, 100ms
            break;
        case NOTIFY_PRIORITY_NORMAL:
            // Subtle ping
            play_tone(660, 50);   // E5, 50ms
            break;
        default:
            // Silent
            break;
    }
}
```

## Simulator Keyboard Controls

Add to simulator for testing:

| Key | Action |
|-----|--------|
| `N` | Show test notification |
| `Shift+N` | Show high-priority test notification |
| `X` | Dismiss current notification |
| `Left/Right` | Navigate notification queue |

## Memory Considerations

- Max 10 notifications in queue (configurable)
- Notification body truncated to 256 chars
- Icons use simple colored shapes, not bitmaps
- Reuse LVGL objects where possible
