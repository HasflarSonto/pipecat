# Screen Page Switching Fix - Debug Log

## Problem Summary
When switching between display pages (Face, Weather, Clock, Timer, Subway), artifacts from previous pages remain visible, and SPI errors occur.

## Hardware
- Waveshare ESP32-S3-Touch-AMOLED-2.06
- Display: 502x410 AMOLED (SH8601, QSPI, RGB565)

## The Error
```
E (xxxx) lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
```

---

## CRITICAL FINDING (2026-02-04): WiFi is NOT the Root Cause

### The Test
We added `esp_wifi_stop()` before page transitions and `esp_wifi_start()` after:
```c
ESP_LOGI(TAG, "Suspending WiFi for page switch test...");
esp_wifi_stop();
// ... show page ...
vTaskDelay(pdMS_TO_TICKS(200));
esp_wifi_start();
```

### The Result
```
I (22816) wifi:flush txq
I (22816) wifi:stop sw txq
I (22816) wifi:lmac stop hw txq        <- WiFi STOPPED
E (22876) lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
E (22893) lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
```

**SPI errors STILL occurred even with WiFi completely stopped.**

### Conclusion
WiFi DMA contention is NOT the root cause. We spent significant time investigating the wrong hypothesis.

---

## Complete List of Failed Attempts (10 total)

| # | Attempt | What We Did | Result |
|---|---------|-------------|--------|
| 1 | Off-screen init | Position widgets at (-100,-100) during creation | Partial - helps ghost artifacts only |
| 2 | Move + Hide | Move to (-200,-200) AND set HIDDEN flag | Partial - some improvement |
| 3 | lv_refr_now() | Force refresh with full-screen black panel | SCREEN FREEZE |
| 4 | lv_obj_invalidate(scr) | Invalidate entire screen | Top-left corner bug |
| 5 | lv_obj_invalidate(widget) | Invalidate each widget before hiding | MORE SPI errors |
| 6 | -600 offsets | Increase hide offset to -600 (alignment fix) | No change |
| 7 | Core pinning + IRAM | WiFi→Core0, LVGL→Core1, IRAM optimization | No change |
| 8 | Buffer 50 lines | Reduce LVGL buffer to 50KB | No change |
| 9 | Queue depth 30 | Increase SPI trans_queue_depth 10→30, add delays, WiFi sleep | No change |
| 10 | **WiFi suspend** | esp_wifi_stop() during page switch | **No change - DISPROVES WiFi HYPOTHESIS** |

---

## What We Ruled Out

The SPI queue overflow happens regardless of:
- **Buffer size**: 50KB, 100KB, 150KB all fail identically
- **Queue depth**: 10 or 30 makes no difference
- **Core pinning**: WiFi on Core 0, LVGL on Core 1 doesn't help
- **WiFi power save**: WIFI_PS_MIN_MODEM doesn't help
- **Widget hiding strategy**: Offsets, hiding, moving - all same result
- **Timing delays**: 30ms, 50ms, 200ms delays don't help
- **WiFi being active**: Errors occur even with WiFi STOPPED

---

## Error Timing Analysis

```
I (51610) luna_main: Showing page: Weather
I (51650) face_renderer: Weather display: ...   <- Widget creation done
E (51678) lcd_panel.io.spi: ...                 <- Error 28ms LATER
```

The error occurs during LVGL's flush operation (when it sends pixels to display), NOT during widget creation or hide phase.

---

## Remaining Hypotheses (Post-WiFi Investigation)

Since WiFi is NOT the cause, the issue must be:

### Hypothesis A: SPI Device/Driver Bug
The SPI LCD panel driver may have a bug that causes queue failures under certain conditions. This would explain why it happens regardless of WiFi state.

### Hypothesis B: LVGL Flush Callback Issue
The `on_color_trans_done` callback may not be firing properly, causing LVGL to queue new transactions before previous ones complete.

### Hypothesis C: Display Panel Driver Bug (SH8601)
The Waveshare SH8601 driver may have issues with rapid widget updates or certain LVGL operations.

### Hypothesis D: PSRAM/Memory Bandwidth
Display buffer is in PSRAM. Rapid LVGL operations may cause memory bandwidth issues unrelated to WiFi.

### Hypothesis E: LVGL Internal State
LVGL may have internal state corruption when rapidly creating/hiding widgets during page switches.

---

## Files Modified During Investigation (to be reverted)

1. `components/waveshare__esp32_s3_touch_amoled_2_06/esp32_s3_touch_amoled_2_06.c`
   - Changed trans_queue_depth from 10 to 30

2. `components/network/wifi_manager.c`
   - Added WIFI_PS_MIN_MODEM after IP acquired

3. `components/luna_face/face_renderer.c`
   - Added 30ms vTaskDelay in hide_all_screen_elements()

4. `main/main.c`
   - Added esp_wifi.h include
   - Added esp_wifi_stop()/start() around show_page()

5. `sdkconfig.defaults`
   - Core pinning settings
   - Buffer size changes (currently 150)
   - IRAM optimization

---

## Next Steps

1. Revert all failed changes to clean state
2. Investigate LVGL flush callback mechanism
3. Consider simpler page architecture (delete/recreate vs hide/show)
4. Look at working demo mode code structure more carefully

---

## NEW FINDING (2026-02-04): Text Display Eyes Blocking Issue

### The Problem
Text from server shows but **eyes block the middle**:
- "Connected to chat!" displays as "Con [blank] t!"
- Eyes are in the center, blocking text

### Why It Happens
`face_renderer_show_text()` was modified to call `hide_all_screen_elements()` which triggers:
1. Massive dirty rectangle updates (moves dozens of widgets)
2. SPI queue overflow
3. Display freeze

### Failed Fix Attempt
Simplified to only `lv_obj_add_flag(eye, LV_OBJ_FLAG_HIDDEN)`:
- **Problem**: Hidden widgets don't trigger background redraw
- Eyes remain visible in display buffer, blocking text

### Correct Approach
Must **MOVE** widgets off-screen AND hide:
```c
lv_obj_set_pos(s_renderer.left_eye, -600, -600);  // Move off-screen
lv_obj_set_pos(s_renderer.right_eye, -600, -600);
lv_obj_add_flag(s_renderer.left_eye, LV_OBJ_FLAG_HIDDEN);
lv_obj_add_flag(s_renderer.right_eye, LV_OBJ_FLAG_HIDDEN);
```

Use -600 offset (not -200) because of alignment persistence - see SCREEN_ARTIFACT_ISSUES_AND_FIXES.md

---

## NEW FINDING (2026-02-04): PSRAM Buffer + WiFi = Internal RAM Exhaustion

### The Real Root Cause (Refined Understanding)

The issue is NOT WiFi DMA contention during transfers, but WiFi **consuming internal RAM** that SPI DMA needs for bounce buffers.

**The chain of events:**
1. LVGL buffer is in PSRAM (`buff_spiram = true, buff_dma = false`)
2. SPI DMA **cannot directly access PSRAM** - requires bounce buffer in internal RAM
3. WiFi initialization allocates internal RAM for its buffers
4. During page switch → many dirty rectangles → many bounce buffer allocations
5. **Internal RAM exhausted** → `ESP_ERR_NO_MEM` or `ESP_ERR_TIMEOUT` → SPI queue fails

**Why "WiFi suspend" test was misleading:**
- `esp_wifi_stop()` stops WiFi activity but **doesn't free the already-allocated internal RAM**
- The memory was consumed at `wifi_init()`, not during active WiFi use

### Memory Fix Attempts

| # | Config Change | Value | Result |
|---|--------------|-------|--------|
| 11 | CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL | 64KB | Still fails |
| 12 | Combined fix (all below) | - | ✅ **WORKS** |
| - | CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL | 128KB | Part of fix |
| - | CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP | y | Part of fix |
| - | CONFIG_ESP_WIFI_IRAM_OPT | n | Part of fix |
| - | CONFIG_ESP_WIFI_RX_IRAM_OPT | n | Part of fix |

### ✅ THE FIX (Combined Approach)

Add these to `sdkconfig.defaults`:
```
# Reserve 128KB internal RAM for SPI DMA bounce buffers
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=131072

# Move WiFi/LWIP buffers to PSRAM
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y

# Disable WiFi IRAM optimization to free ~27KB internal RAM
CONFIG_ESP_WIFI_IRAM_OPT=n
CONFIG_ESP_WIFI_RX_IRAM_OPT=n
```

This ensures enough internal RAM is reserved for SPI DMA bounce buffers when LVGL buffer is in PSRAM.

### Why Face Animations Work

Face animations only update **small regions**:
- Eye movement: ~100x100px dirty rectangles
- Small bounce buffers needed → allocation succeeds
- Steady rate gives memory time to be freed

### Why Page Switch Fails

Page switch creates **burst of large dirty rectangles**:
- hide_all moves dozens of widgets → many large dirty rects
- show_page creates new widgets → more dirty rects
- Many simultaneous bounce buffer allocations → internal RAM exhausted

---

## Lessons Learned

1. **Test hypotheses early**: We should have tested WiFi suspension (Attempt 10) before trying Attempts 7-9
2. **Correlation ≠ Causation**: "Worked without WiFi" didn't mean WiFi was the cause
3. **Document failures thoroughly**: This log prevents re-trying the same fixes
4. **Memory allocation vs activity**: WiFi memory is allocated at init, not during use - stopping WiFi doesn't free it