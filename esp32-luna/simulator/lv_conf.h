/**
 * LVGL Configuration for Luna Simulator (Desktop)
 * Based on lv_conf_template.h for LVGL 9.x
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/

/* Color depth: 16 (RGB565), 24 (RGB888), 32 (ARGB8888) */
#define LV_COLOR_DEPTH 16

/*=========================
   MEMORY SETTINGS
 *=========================*/

/* Size of the memory available for `lv_malloc()` in bytes (>= 2kB) */
#define LV_MEM_SIZE (256 * 1024U)  /* 256KB */

/* Use standard malloc/free - LVGL's builtin allocator */
#define LV_MEM_CUSTOM 0

/*====================
   HAL SETTINGS
 *====================*/

/* Default display refresh, input read and animation step period */
#define LV_DEF_REFR_PERIOD  16  /* ~60 FPS */

/* Default Dot Per Inch. Used to initialize default sizes such as widgets sized, style paddings */
#define LV_DPI_DEF 130

/* Default display resolution */
#define LV_HOR_RES_MAX 502
#define LV_VER_RES_MAX 410

/*====================
 * FEATURE CONFIGURATION
 *====================*/

/*-------------
 * Drawing
 *-----------*/

/* Enable complex draw engine (required for arcs, shadows, etc.) */
#define LV_DRAW_COMPLEX 1

/* Enable draw masks (needed for arcs and other effects) */
#define LV_USE_DRAW_MASKS 1

/* Enable SW rendering */
#define LV_USE_DRAW_SW 1

/* Disable ARM-specific optimizations (not applicable on desktop) */
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#define LV_USE_DRAW_ARM2D 0
#define LV_USE_NATIVE_HELIUM_ASM 0

/*-------------
 * Logging
 *-----------*/

/* Enable the log module */
#define LV_USE_LOG 1

#if LV_USE_LOG
/* How important log should be added */
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN

/* Use printf for log output */
#define LV_LOG_PRINTF 1
#endif /* LV_USE_LOG */

/*-------------
 * Asserts
 *-----------*/

#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/*-------------
 * Others
 *-----------*/

/* 1: Show CPU usage and FPS count */
#define LV_USE_PERF_MONITOR 1
#if LV_USE_PERF_MONITOR
#define LV_USE_PERF_MONITOR_POS LV_ALIGN_BOTTOM_RIGHT
#endif

/* 1: Show used memory and memory fragmentation */
#define LV_USE_MEM_MONITOR 0

/* Maximum buffer size to allocate for rotation in bytes */
#define LV_DISPLAY_ROT_MAX_BUF (10 * 1024)

/*==================
 *    FONT USAGE
 *==================*/

/* Montserrat fonts with ASCII range and some symbols */
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 1
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 1

/* Default font */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Enable FreeType for custom fonts */
#define LV_USE_FREETYPE 0

/*==================
 *  WIDGET USAGE
 *==================*/

#define LV_USE_ANIMIMG    1
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMAGE      1
#define LV_USE_IMAGEBUTTON 1
#define LV_USE_KEYBOARD   1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_LIST       1
#define LV_USE_MENU       1
#define LV_USE_MSGBOX     1
#define LV_USE_ROLLER     1
#define LV_USE_SCALE      1
#define LV_USE_SLIDER     1
#define LV_USE_SPAN       1
#define LV_USE_SPINBOX    1
#define LV_USE_SPINNER    1
#define LV_USE_SWITCH     1
#define LV_USE_TABLE      1
#define LV_USE_TABVIEW    1
#define LV_USE_TEXTAREA   1
#define LV_USE_TILEVIEW   1
#define LV_USE_WIN        1

/*==================
 * LAYOUTS
 *==================*/

#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*==================
 * 3RD PARTY LIBS
 *==================*/

/* PNG decoder library */
#define LV_USE_PNG 0

/* BMP decoder library */
#define LV_USE_BMP 0

/* GIF decoder library */
#define LV_USE_GIF 0

/* QR code library */
#define LV_USE_QRCODE 0

/*==================
 * STDINT
 *==================*/

#define LV_STDINT_INCLUDE <stdint.h>
#define LV_STDDEF_INCLUDE <stddef.h>
#define LV_STDBOOL_INCLUDE <stdbool.h>
#define LV_STRING_INCLUDE <string.h>
#define LV_LIMITS_INCLUDE <limits.h>
#define LV_INTTYPES_INCLUDE <inttypes.h>

/* Define a custom attribute to `lv_tick_inc` function */
#define LV_ATTRIBUTE_TICK_INC

/* Define a custom attribute to `lv_timer_handler` function */
#define LV_ATTRIBUTE_TIMER_HANDLER

/* Define a custom attribute to `lv_display_flush_ready` function */
#define LV_ATTRIBUTE_FLUSH_READY

/* Required alignment size for buffers */
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 1

/* Will be added where memories needs to be aligned (with -Os data might not be aligned to boundary by default) */
#define LV_ATTRIBUTE_MEM_ALIGN

/* Attribute to mark large constant arrays */
#define LV_ATTRIBUTE_LARGE_CONST

/* Compiler prefix for large arrays declared in RAM */
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY

/* Place performance critical functions into a faster memory (e.g. RAM) */
#define LV_ATTRIBUTE_FAST_MEM

/* Export integer constant to binding. This macro is used with constants in the form of LV_<CONST> */
#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning

/* Extend the default -32k..32k coordinate range to -4M..4M by using int32_t for coordinates instead of int16_t */
#define LV_USE_LARGE_COORD 0

/*==================
 * STDLIB
 *==================*/

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

#endif /* LV_CONF_H */
