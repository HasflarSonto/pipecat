/**
 * ESP-IDF Timer Stub for Simulator
 */

#ifndef ESP_TIMER_H
#define ESP_TIMER_H

#include <stdint.h>
#include <time.h>
#include <sys/time.h>

/**
 * Get time since boot in microseconds
 * Uses gettimeofday for simulation
 */
static inline int64_t esp_timer_get_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

#endif /* ESP_TIMER_H */
