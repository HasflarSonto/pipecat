/*
 * Luna Protocol for ESP32-Luna
 * Parses JSON commands from server and formats responses
 */

#include "luna_protocol.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "luna_protocol";

static luna_cmd_handler_t s_handler = NULL;
static void *s_handler_ctx = NULL;

esp_err_t luna_protocol_init(void)
{
    ESP_LOGI(TAG, "Protocol parser initialized");
    return ESP_OK;
}

esp_err_t luna_protocol_deinit(void)
{
    s_handler = NULL;
    s_handler_ctx = NULL;
    return ESP_OK;
}

esp_err_t luna_protocol_parse_color(const char *hex, uint32_t *color)
{
    if (hex == NULL || color == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Skip leading '#' if present
    if (hex[0] == '#') {
        hex++;
    }

    // Parse hex string
    if (strlen(hex) != 6) {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned int r, g, b;
    if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) != 3) {
        return ESP_ERR_INVALID_ARG;
    }

    *color = (r << 16) | (g << 8) | b;
    return ESP_OK;
}

static luna_font_size_t parse_font_size(const char *size_str)
{
    if (size_str == NULL) {
        return LUNA_FONT_MEDIUM;
    }

    if (strcmp(size_str, "small") == 0) return LUNA_FONT_SMALL;
    if (strcmp(size_str, "medium") == 0) return LUNA_FONT_MEDIUM;
    if (strcmp(size_str, "large") == 0) return LUNA_FONT_LARGE;
    if (strcmp(size_str, "xlarge") == 0) return LUNA_FONT_XLARGE;

    return LUNA_FONT_MEDIUM;
}

esp_err_t luna_protocol_parse(const char *json, luna_cmd_t *cmd)
{
    if (json == NULL || cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(cmd, 0, sizeof(luna_cmd_t));
    cmd->type = LUNA_CMD_UNKNOWN;

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (cmd_item == NULL || !cJSON_IsString(cmd_item)) {
        ESP_LOGE(TAG, "Missing 'cmd' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *cmd_str = cmd_item->valuestring;

    // Parse based on command type
    if (strcmp(cmd_str, "emotion") == 0) {
        cmd->type = LUNA_CMD_EMOTION;
        cJSON *value = cJSON_GetObjectItem(root, "value");
        if (value && cJSON_IsString(value)) {
            strncpy(cmd->data.emotion.emotion, value->valuestring,
                    sizeof(cmd->data.emotion.emotion) - 1);
        }
        ESP_LOGD(TAG, "Parsed emotion: %s", cmd->data.emotion.emotion);
    }
    else if (strcmp(cmd_str, "gaze") == 0) {
        cmd->type = LUNA_CMD_GAZE;
        cJSON *x = cJSON_GetObjectItem(root, "x");
        cJSON *y = cJSON_GetObjectItem(root, "y");
        cmd->data.gaze.x = (x && cJSON_IsNumber(x)) ? (float)x->valuedouble : 0.5f;
        cmd->data.gaze.y = (y && cJSON_IsNumber(y)) ? (float)y->valuedouble : 0.5f;
        ESP_LOGD(TAG, "Parsed gaze: %.2f, %.2f", cmd->data.gaze.x, cmd->data.gaze.y);
    }
    else if (strcmp(cmd_str, "text") == 0) {
        cmd->type = LUNA_CMD_TEXT;
        cJSON *content = cJSON_GetObjectItem(root, "content");
        cJSON *size = cJSON_GetObjectItem(root, "size");
        cJSON *color = cJSON_GetObjectItem(root, "color");
        cJSON *bg = cJSON_GetObjectItem(root, "bg");

        if (content && cJSON_IsString(content)) {
            strncpy(cmd->data.text.content, content->valuestring,
                    sizeof(cmd->data.text.content) - 1);
        }
        cmd->data.text.size = parse_font_size(
            (size && cJSON_IsString(size)) ? size->valuestring : NULL);

        if (color && cJSON_IsString(color)) {
            luna_protocol_parse_color(color->valuestring, &cmd->data.text.color);
        } else {
            cmd->data.text.color = 0xFFFFFF;  // White default
        }

        if (bg && cJSON_IsString(bg)) {
            luna_protocol_parse_color(bg->valuestring, &cmd->data.text.bg_color);
        } else {
            cmd->data.text.bg_color = 0x1E1E28;  // Dark default
        }

        ESP_LOGD(TAG, "Parsed text: %s", cmd->data.text.content);
    }
    else if (strcmp(cmd_str, "text_clear") == 0) {
        cmd->type = LUNA_CMD_TEXT_CLEAR;
        ESP_LOGD(TAG, "Parsed text_clear");
    }
    else if (strcmp(cmd_str, "pixel_art") == 0) {
        cmd->type = LUNA_CMD_PIXEL_ART;
        cJSON *pixels = cJSON_GetObjectItem(root, "pixels");
        cJSON *bg = cJSON_GetObjectItem(root, "bg");

        if (bg && cJSON_IsString(bg)) {
            luna_protocol_parse_color(bg->valuestring, &cmd->data.pixel_art.bg_color);
        } else {
            cmd->data.pixel_art.bg_color = 0x1E1E28;
        }

        if (pixels && cJSON_IsArray(pixels)) {
            int count = cJSON_GetArraySize(pixels);
            cmd->data.pixel_art.pixels = malloc(count * sizeof(luna_pixel_t));
            cmd->data.pixel_art.pixel_count = 0;

            if (cmd->data.pixel_art.pixels) {
                for (int i = 0; i < count; i++) {
                    cJSON *pixel = cJSON_GetArrayItem(pixels, i);
                    if (pixel) {
                        cJSON *x = cJSON_GetObjectItem(pixel, "x");
                        cJSON *y = cJSON_GetObjectItem(pixel, "y");
                        cJSON *c = cJSON_GetObjectItem(pixel, "c");

                        if (x && y && c && cJSON_IsNumber(x) && cJSON_IsNumber(y) && cJSON_IsString(c)) {
                            luna_pixel_t *p = &cmd->data.pixel_art.pixels[cmd->data.pixel_art.pixel_count];
                            p->x = (uint8_t)x->valueint;
                            p->y = (uint8_t)y->valueint;
                            luna_protocol_parse_color(c->valuestring, &p->color);
                            cmd->data.pixel_art.pixel_count++;
                        }
                    }
                }
            }
        }
        ESP_LOGD(TAG, "Parsed pixel_art: %d pixels", (int)cmd->data.pixel_art.pixel_count);
    }
    else if (strcmp(cmd_str, "pixel_art_clear") == 0) {
        cmd->type = LUNA_CMD_PIXEL_ART_CLEAR;
        ESP_LOGD(TAG, "Parsed pixel_art_clear");
    }
    else if (strcmp(cmd_str, "audio_start") == 0) {
        cmd->type = LUNA_CMD_AUDIO_START;
        ESP_LOGD(TAG, "Parsed audio_start");
    }
    else if (strcmp(cmd_str, "audio_stop") == 0) {
        cmd->type = LUNA_CMD_AUDIO_STOP;
        ESP_LOGD(TAG, "Parsed audio_stop");
    }
    else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd_str);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void luna_protocol_free_cmd(luna_cmd_t *cmd)
{
    if (cmd == NULL) {
        return;
    }

    if (cmd->type == LUNA_CMD_PIXEL_ART && cmd->data.pixel_art.pixels) {
        free(cmd->data.pixel_art.pixels);
        cmd->data.pixel_art.pixels = NULL;
        cmd->data.pixel_art.pixel_count = 0;
    }
}

void luna_protocol_set_handler(luna_cmd_handler_t handler, void *ctx)
{
    s_handler = handler;
    s_handler_ctx = ctx;
}
