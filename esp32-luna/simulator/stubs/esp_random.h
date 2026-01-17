/**
 * ESP-IDF Random Number Generator Stub for Simulator
 */

#ifndef ESP_RANDOM_H
#define ESP_RANDOM_H

#include <stdint.h>
#include <stdlib.h>

/**
 * Get a random 32-bit value
 * Uses standard rand() for simulation
 */
static inline uint32_t esp_random(void)
{
    return (uint32_t)rand() ^ ((uint32_t)rand() << 16);
}

#endif /* ESP_RANDOM_H */
