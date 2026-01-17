/**
 * FreeRTOS Task Stub for Simulator
 * Uses pthreads for real task creation
 */

#ifndef FREERTOS_TASK_H
#define FREERTOS_TASK_H

#include "FreeRTOS.h"
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

typedef pthread_t* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);

/* Delay function using usleep */
static inline void vTaskDelay(TickType_t ticks)
{
    usleep(ticks * 1000);  /* Convert ms to us */
}

/* Get tick count using time */
static inline TickType_t xTaskGetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (TickType_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Thread wrapper structure */
typedef struct {
    TaskFunction_t func;
    void* param;
} task_wrapper_t;

/* Thread wrapper function */
static void* task_thread_wrapper(void* arg)
{
    task_wrapper_t* wrapper = (task_wrapper_t*)arg;
    TaskFunction_t func = wrapper->func;
    void* param = wrapper->param;
    free(wrapper);

    /* Run the task function */
    func(param);

    return NULL;
}

/* Task creation - creates a real pthread */
static inline BaseType_t xTaskCreate(
    TaskFunction_t pxTaskCode,
    const char* pcName,
    uint32_t usStackDepth,
    void* pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t* pxCreatedTask)
{
    (void)pcName;
    (void)usStackDepth;
    (void)uxPriority;

    pthread_t* thread = malloc(sizeof(pthread_t));
    if (!thread) {
        if (pxCreatedTask) *pxCreatedTask = NULL;
        return pdFAIL;
    }

    task_wrapper_t* wrapper = malloc(sizeof(task_wrapper_t));
    if (!wrapper) {
        free(thread);
        if (pxCreatedTask) *pxCreatedTask = NULL;
        return pdFAIL;
    }

    wrapper->func = pxTaskCode;
    wrapper->param = pvParameters;

    int ret = pthread_create(thread, NULL, task_thread_wrapper, wrapper);
    if (ret != 0) {
        free(wrapper);
        free(thread);
        if (pxCreatedTask) *pxCreatedTask = NULL;
        return pdFAIL;
    }

    if (pxCreatedTask) *pxCreatedTask = thread;
    return pdPASS;
}

static inline BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t pxTaskCode,
    const char* pcName,
    uint32_t usStackDepth,
    void* pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t* pxCreatedTask,
    int xCoreID)
{
    (void)xCoreID;
    return xTaskCreate(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask);
}

static inline void vTaskDelete(TaskHandle_t xTask)
{
    if (xTask) {
        /* Note: pthread_cancel is not ideal but works for our use case */
        pthread_cancel(*xTask);
        pthread_join(*xTask, NULL);
        free(xTask);
    }
}

#endif /* FREERTOS_TASK_H */
