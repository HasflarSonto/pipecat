# LVGL Display Debugging

## Progress Log

### Attempt 1: Full Refresh + Double Buffering
- Added `CONFIG_BSP_DISPLAY_LVGL_FULL_REFRESH=y` to sdkconfig.defaults
- Set `BSP_LCD_DRAW_BUFF_DOUBLE=1` in BSP header
- **Result:** Did NOT fix artifacts. Config may not have been applied properly.

### Attempt 2: Reduced Arc Size (100px)
- Changed arc size from ~300px to 100px fixed
- **Result:** WORKING! Arcs showed up, SPI errors gone. Some ghost artifacts remained.

### Attempt 3: Added Explicit Invalidation (BROKEN - REVERTED)
- Added `lv_obj_invalidate()` before hiding widgets
- Reset widget sizes to 0
- Added full screen invalidation
- **Result:** BROKE EVERYTHING. Mouth stuck in neutral, eyes creating ghosts.
- **Action:** Reverted to simple hide (just `lv_obj_add_flag(LV_OBJ_FLAG_HIDDEN)`)

---

## Current Status

**What works:**
- Smaller arcs (100px) render without SPI errors
- Basic face rendering
- Mouth state transitions (smile/frown/neutral/cat)

**What's broken:**
- Ghost artifacts when widgets move (partial refresh issue)

**Next steps:**
- Verify reverted code works correctly
- Investigate ghost artifacts - may need fullclean rebuild to apply sdkconfig changes
- Consider direct framebuffer approach if LVGL dirty rect issues persist

---

## Original Issues

### Issue 1: SPI Transmit Failures with Arcs
```
E (3035) lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
```

**When it occurs:** Every time an arc is rendered
**Arc sizes reported:** 273, 345, 312, 225, 219 pixels

**Root cause:** Arc widget creates draw operations larger than SPI DMA buffer can handle.
**Solution:** Reduce arc size to 100px. WORKS.

### Issue 2: Ghost Artifacts
Despite enabling full refresh mode and double buffering, ghost artifacts persist when widgets move.

**Possible causes:**
1. sdkconfig.defaults changes weren't applied (need `fullclean`)
2. The BSP may be ignoring our config options
3. Partial refresh mode still active
4. LVGL dirty rectangle tracking not working correctly with this display

---

## Diagnostic Steps

### Step 1: Verify Config Was Applied

Check if full refresh is actually enabled in the build:
```bash
grep -r "FULL_REFRESH" build/
grep -r "BSP_DISPLAY_LVGL" build/config/sdkconfig
```

### Step 2: Check Actual Buffer Sizes

Add logging to see what buffer sizes are being used at runtime.

### Step 3: Understand the SPI Error

The error `spi transmit (queue) color failed` means:
- The SPI DMA queue is full
- The transfer size exceeds the configured limit
- There's a timing/synchronization issue

---

## Current Configuration

### sdkconfig.defaults (our changes)
```
CONFIG_BSP_DISPLAY_LVGL_FULL_REFRESH=y
CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=410
CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR=y
```

### BSP Header Change
```c
#define BSP_LCD_DRAW_BUFF_DOUBLE   (1)  // Changed from 0
```

---

## Arc Widget Analysis

### Current Arc Sizes
The arc sizes being created are huge:
- `size=273` → 273x273 pixels = 74,529 pixels × 2 bytes = **149KB per frame**
- `size=345` → 345x345 pixels = 119,025 pixels × 2 bytes = **238KB per frame**

### The Problem
These arc sizes are larger than the SPI DMA can handle in a single transfer.
The display is 502×410 = 205,820 pixels total.
An arc of 345×345 is **58% of the entire screen** in one widget!

### Solution: Use Smaller Arcs
The mouth doesn't need to be that large. We should:
1. Reduce arc size significantly (e.g., 100-150 pixels max)
2. Use arc_width to create the curve thickness instead of object size

---

## Next Steps

1. [ ] Verify config changes are in the actual build
2. [x] Reduce arc widget size from ~300px to ~100px (DONE)
3. [ ] Test if smaller arcs avoid SPI overflow
4. [ ] If still failing, investigate BSP buffer configuration
5. [ ] Consider using line-based curves instead of arc widget

---

## Fix Applied: Reduced Arc Size

**Before:**
```c
int arc_size = (int)(mouth_width * 3.0f);  // 273-345px - TOO BIG!
```

**After:**
```c
int arc_size = 100;  // Fixed 100px - within SPI limits
```

---

## Log Analysis

```
I (3018) face_renderer: Mouth curve changed: -1000 -> -1 (mouth_curve=-0.22)
I (3020) face_renderer: Frown (arc) at y=250, size=273
E (3035) lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
```

Timeline:
- Arc created at size 273
- 15ms later: SPI fails
- This is too fast - the SPI queue is immediately overwhelmed

---

## Technical Details

### LVGL Arc Widget
- Creates a square bounding box of `size × size`
- Draws arc curve within that box
- Even if only drawing a small portion of the arc, the entire bounding box may be processed

### SPI DMA Limits
- ESP32-S3 SPI DMA has transfer size limits
- Large transfers must be split into chunks
- If chunks exceed queue depth, transmit fails

### Display: SH8601 AMOLED (502×410)
- Interface: QSPI
- Color: RGB565 (2 bytes/pixel)
- Full screen: 411,640 bytes (~400KB)
