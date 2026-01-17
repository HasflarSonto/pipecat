/**
 * ESP-IDF Logging Stub for Simulator
 */

#ifndef ESP_LOG_H
#define ESP_LOG_H

#include <stdio.h>

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;

#define ESP_LOGE(tag, format, ...) fprintf(stderr, "E (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) fprintf(stderr, "W (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) printf("I (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) printf("D (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) printf("V (%s) " format "\n", tag, ##__VA_ARGS__)

#define esp_log_level_set(tag, level) ((void)0)

#endif /* ESP_LOG_H */
