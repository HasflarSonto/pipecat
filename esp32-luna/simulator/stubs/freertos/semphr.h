/**
 * FreeRTOS Semaphore Stub for Simulator
 * Uses pthread mutex for implementation
 */

#ifndef FREERTOS_SEMPHR_H
#define FREERTOS_SEMPHR_H

#include "FreeRTOS.h"
#include <pthread.h>
#include <stdlib.h>

typedef pthread_mutex_t* SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t* mutex = malloc(sizeof(pthread_mutex_t));
    if (mutex) {
        pthread_mutex_init(mutex, NULL);
    }
    return mutex;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return xSemaphoreCreateMutex();
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xBlockTime)
{
    (void)xBlockTime;
    if (xSemaphore == NULL) return pdFALSE;
    pthread_mutex_lock(xSemaphore);
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
    if (xSemaphore == NULL) return pdFALSE;
    pthread_mutex_unlock(xSemaphore);
    return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t xSemaphore)
{
    if (xSemaphore) {
        pthread_mutex_destroy(xSemaphore);
        free(xSemaphore);
    }
}

#endif /* FREERTOS_SEMPHR_H */
