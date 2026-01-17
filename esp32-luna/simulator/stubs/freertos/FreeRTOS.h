/**
 * FreeRTOS Stub for Simulator
 */

#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>
#include <stdbool.h>

/* Tick type */
typedef uint32_t TickType_t;
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;

#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdPASS  pdTRUE
#define pdFAIL  pdFALSE

/* Time conversion macros - 1 tick = 1 ms */
#define portTICK_PERIOD_MS  1
#define portMAX_DELAY       0xFFFFFFFF

/* Convert ms to ticks (1:1 in simulator) */
#define pdMS_TO_TICKS(xTimeInMs) ((TickType_t)(xTimeInMs))

/* Task priorities */
#define configMAX_PRIORITIES 25

/* Memory macros */
#define portENTER_CRITICAL()  ((void)0)
#define portEXIT_CRITICAL()   ((void)0)

/* Stack size */
#define configMINIMAL_STACK_SIZE 128

/* Get current core ID - always 0 in simulator */
static inline int xPortGetCoreID(void)
{
    return 0;
}

#endif /* FREERTOS_H */
