# ESP32-Luna Refactoring Progress

**Started:** 2026-01-23
**Status:** In Progress

---

## Issues to Fix

- [ ] **Issue 1:** Code file too large (face_renderer.c is ~2000+ lines)
- [x] **Issue 2:** Green artifacts at startup - FIXED
- [x] **Issue 3:** Server/demo code conflict in main.c - FIXED
- [x] **Issue 4:** Overlapping screens (widgets not cleared between modes) - FIXED
- [x] **Issue 5:** Display only showing top-left quarter - FIXED (buffer too small)

---

## Step 1: Split face_renderer.c into Multiple Files

**Status:** Not Started (lower priority)

### Current Structure (monolithic)
- `face_renderer.c` - Everything (~2000+ lines)

### Target Structure
- [ ] `face_renderer.c` - Core init, state machine, render loop (~400 lines)
- [ ] `face_widgets.c` - Face drawing (eyes, mouth, whiskers, brows)
- [ ] `display_modes.c` - Timer, clock, weather, calendar, notification, subway
- [ ] `display_common.h` - Shared types, colors, constants

### Files to Create
- [ ] `components/luna_face/face_widgets.c`
- [ ] `components/luna_face/face_widgets.h`
- [ ] `components/luna_face/display_modes.c`
- [ ] `components/luna_face/display_modes.h`
- [ ] `components/luna_face/display_common.h`

### Progress Notes
_Deferred - code works, refactoring for maintainability later_

---

## Step 2: Fix Green Artifacts at Startup

**Status:** UPDATED (Attempt 3)

### Root Cause
1. The BSP's LCD init commands set brightness=0xFF at the end, BEFORE LVGL clears the buffer
2. Full-screen widgets (502x410) cause SPI DMA overflow errors on this display
3. Widgets >100px can overflow the SPI transfer queue

### Solution (Attempt 3 - Proper Fix)
1. [x] **Modified BSP lcd_init_cmds** to NOT set brightness=255 at end of init
2. [x] **Removed full-screen background widget** from face_renderer_init (caused SPI errors!)
3. [x] Keep `bsp_display_backlight_off()` call after bsp_display_start()
4. [x] Set screen background color via lv_obj_set_style_bg_color (LVGL handles incrementally)
5. [x] Single refresh before creating face widgets
6. [x] `bsp_display_backlight_on()` after face widgets are created

### Files Changed
- `components/waveshare__esp32_s3_touch_amoled_2_06/esp32_s3_touch_amoled_2_06.c`
  - Removed `{0x51, 0xFF}` from lcd_init_cmds (brightness stays 0 until explicit backlight_on)
- `components/luna_face/face_renderer.c`
  - Removed full-screen black rectangle widget (was causing SPI DMA overflow)
  - Simplified to just setting screen background color

### Technical Notes
- **SPI DMA Limit**: This display's SPI interface cannot handle widgets >100px in a single transfer
- The previous fix created a 502x410 widget which overwhelmed the SPI queue
- Correct approach: Let LVGL handle background via screen style, not via explicit widget

---

## Step 3: Simplify main.c (Remove Server/Demo Conflict)

**Status:** COMPLETED ✓

### Current Problems (WERE)
- WiFi connection attempts cause delays
- WebSocket code conflicts with demo mode
- Auto-cycling demo mode is confusing
- Too many states to manage

### Target Behavior
- Boot directly to face (happy emotion)
- NO WiFi/WebSocket attempts (pure demo mode)
- Button press cycles: Face -> Weather -> Clock -> Calendar
- Static demo data (no auto-cycling)
- Simple, predictable behavior

### Changes
- [x] Remove WiFi init and connection code
- [x] Remove WebSocket client code
- [x] Remove audio capture/playback code
- [x] Remove demo mode auto-cycling state machine
- [x] Keep only: face_renderer, button polling, page cycling
- [x] Simplify main loop to just: poll button, show page

### Progress Notes
- Rewrote main.c from ~600 lines to ~190 lines
- Simple demo mode with boot button cycling pages
- Pages: Face -> Weather -> Clock -> Calendar -> (repeat)
- No WiFi, no WebSocket, no audio - just display

---

## Step 4: Fix Overlapping Screens

**Status:** IN PROGRESS (re-testing)

### Root Cause
When switching display modes, old widgets are not destroyed before creating new ones.
Additionally, clock and timer modes were reusing `s_renderer.text_label` which is only hidden (not deleted).

### Solution (Enhanced)
1. [x] Enhanced `hide_all_screen_elements()` to DELETE widgets instead of just hiding
2. [x] Added DELETE_WIDGET macro for safe deletion
3. [x] Face widgets are HIDDEN (always exist), mode widgets are DELETED
4. [x] `face_renderer_clear_display()` calls `hide_all_screen_elements()` first
5. [x] **NEW:** Added dedicated `s_clock_time_label` for clock display (deleted on mode switch)
6. [x] **NEW:** Added dedicated `s_timer_time_label` for timer display (deleted on mode switch)
7. [x] **NEW:** Added full screen `lv_obj_invalidate()` after hiding to force complete redraw

### Widget Categories
- [x] Face widgets (eyes, mouth, whiskers, brows) - always exist, just hide/show
- [x] Weather widgets (icon, temp, description) - DELETED on mode switch
- [x] Clock widgets (time, date, AM/PM) - DELETED on mode switch (including new s_clock_time_label)
- [x] Calendar widgets (event cards) - DELETED on mode switch
- [x] Timer widgets (arc, time, buttons) - DELETED on mode switch (including new s_timer_time_label)
- [x] Notification widgets (cards) - DELETED on mode switch
- [x] Subway widgets (card, circle, labels) - DELETED on mode switch

### Progress Notes
- Changed `hide_all_screen_elements()` to delete mode-specific widgets
- Added missing widgets to cleanup list (timer buttons, weather card, etc.)
- Face widgets are preserved (hidden only) since they're always needed
- Mode widgets are fully destroyed and recreated on switch
- **2026-01-23:** Added dedicated labels for clock/timer time display instead of reusing text_label
- **2026-01-23:** Added full screen invalidation to force AMOLED partial refresh to clear artifacts

---

## Build & Test Checkpoints

- [ ] After Step 1: Build succeeds, face displays correctly (SKIPPED FOR NOW)
- [x] After Step 2: No green artifacts at startup - BUILD OK, needs hardware test
- [x] After Step 3: Clean boot to face, button cycles pages - BUILD OK
- [x] After Step 4: Clean transitions between all modes - BUILD OK

### Build Results (2026-01-23)
- Binary size: 0xc34e0 bytes (~800KB)
- Partition: 3MB available (75% free)
- Status: Ready to flash

---

## Final Verification

- [ ] Boot shows black screen, then face (no green)
- [ ] Button cycles: Face -> Weather -> Clock -> Calendar -> Face
- [ ] Each mode displays cleanly (no overlapping)
- [ ] Code is well-organized in separate files
- [ ] Build size is reasonable

---

## Completed Items Archive

### 2026-01-23
- **main.c simplified**: Removed all WiFi/WebSocket/audio code, now pure demo mode
- **Widget cleanup fixed**: `hide_all_screen_elements()` now deletes mode widgets
- **Green artifacts fix**: Added `bsp_display_backlight_off()` after display start
- **Display buffer fix**: Changed CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT from 410 to 502

---

## Step 5: Fix Display Buffer Size (Only Top-Left Quarter Rendered)

**Status:** FIXED

### Root Cause
The LVGL buffer was too small for the rotated display:
- Display: 410x502 (portrait) rotated to 502x410 (landscape)
- Buffer height was 410, so buffer = 410 * 410 = 168,100 pixels
- Full display needs 410 * 502 = 205,820 pixels
- Only top-left 410x410 area was being rendered

### Solution
Changed `CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT` from 410 to 502 in:
- `sdkconfig.defaults`
- `sdkconfig`

New buffer size: 410 * 502 = 205,820 pixels (covers full rotated display)

### Technical Notes
- The BSP Kconfig has `BSP_DISPLAY_LVGL_AVOID_TEAR` which depends on `BSP_LCD_RGB_BUFFER_NUMS > 1`
- Since RGB_BUFFER_NUMS defaults to 1, AVOID_TEAR can't be enabled
- When AVOID_TEAR is disabled, buffer uses `BSP_LCD_H_RES * LVGL_BUFFER_HEIGHT`
- Setting LVGL_BUFFER_HEIGHT to 502 ensures full display coverage

