/**
 * FreeRTOS Semaphore Stub for Simulator
 *
 * In the simulator, everything runs single-threaded in the main loop,
 * so we don't need real locking. Just use no-ops to avoid deadlocks.
 */

#ifndef FREERTOS_SEMPHR_H
#define FREERTOS_SEMPHR_H

#include "FreeRTOS.h"
#include <stdlib.h>

/* Dummy handle - no real mutex needed in single-threaded simulator */
typedef void* SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    /* Return non-NULL dummy pointer */
    return (void*)1;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return xSemaphoreCreateMutex();
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xBlockTime)
{
    (void)xBlockTime;
    /* Always succeed - single threaded, no contention */
    return (xSemaphore != NULL) ? pdTRUE : pdFALSE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
    /* Always succeed */
    return (xSemaphore != NULL) ? pdTRUE : pdFALSE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t xSemaphore)
{
    (void)xSemaphore;
    /* Nothing to free */
}

#endif /* FREERTOS_SEMPHR_H */
