# Screen Artifact Issues and Fixes

This document tracks LVGL display artifact issues encountered on the ESP32-Luna project and their solutions.

## Current Status (2026-02-02) - FIXED ✅

| Page | Status | Details |
|------|--------|---------|
| Face | ✅ Working | Eyes render correctly, distressed effect works |
| Weather | ✅ Working | Full screen, icons and text correct |
| Clock | ✅ Working | Full screen, text correct |
| Timer | ✅ Working | Full screen with buttons |
| Calendar | ✅ Working | Full screen |
| Subway | ✅ Working | Full screen |

---

## The ROOT CAUSE: SPI DMA Buffer Overflow

**The actual problem was the LVGL buffer was too large, causing SPI DMA overflow.**

### Symptoms
- `E (xxxx) lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed`
- Two consecutive errors = page completely broken (stuck in top-left)
- One error = page partially broken (background artifacts)

### The Fix
Reduce `CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT` from 200 to 150 in sdkconfig:
```
# Screen is 502x410, RGB565 (2 bytes/pixel)
# Buffer = 502 * 150 * 2 = 150KB (safe for SPI, under 160KB limit)
CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=150
```

### Why It Happened
The comment in sdkconfig said `410 * 200 * 2 = 164KB` but the screen width is actually **502**, not 410!
- Actual buffer was: 502 × 200 × 2 = **200KB** (exceeded 160KB SPI DMA limit)
- Fixed buffer is: 502 × 150 × 2 = **150KB** (under limit)

---

## Secondary Rule: Avoid Full-Screen Operations During Mode Switching

**DO NOT perform any full-screen operations during mode switching.**

The solution is to:
1. Set screen background ONCE during `face_renderer_init()` - never touch it again
2. Move widgets off-screen with **large offsets (-600, -600)** AND hide them when switching modes
   - IMPORTANT: Aligned widgets need offsets larger than screen dimensions (see "Alignment Persistence" below)
3. DO NOT show/hide any full-screen background panels during mode switching
4. DO NOT use `lv_obj_invalidate()`, `lv_refr_now()`, or modify screen properties during mode switching

Pre-created full-screen background panels can exist (created during init) but must NEVER be shown/hidden during mode switching - even showing/hiding pre-created full-screen panels triggers the top-left bug.

---

## The Two States We Discovered

During debugging, we oscillated between two broken states:

### State 1: Top-Left Corner Bug
- **Symptom**: All rendering stuck/squished in top-left quadrant
- **Trigger**: ANY full-screen operation during mode switching
- **Examples**: Setting screen bg, lv_obj_invalidate, creating full-screen panels, showing/hiding full-screen panels, lv_obj_move_to_index on full-screen panels

### State 2: Full-Screen But Artifacts (CURRENT - ALMOST FIXED)
- **Symptom**: Pages render full-screen, minor background artifacts may remain
- **Trigger**: Normal mode switching WITHOUT full-screen operations
- This is the acceptable state

---

## Attempted Fixes Log

| # | Approach | Result |
|---|----------|--------|
| 1 | `lv_obj_invalidate(scr)` | ❌ Top-left corner bug |
| 2 | Setting screen background during switch | ❌ Top-left corner bug |
| 3 | Temp panel + `lv_refr_now(NULL)` + delete | ❌ Top-left corner bug |
| 4 | Move widgets off-screen (-200, -200) AND hide | ⚠️ PARTIAL - doesn't work for aligned widgets |
| 5 | Create full-screen background panel during hide_all | ❌ Top-left corner bug |
| 6 | Delete/recreate full-screen panel each switch | ❌ Top-left corner bug |
| 7 | Move ALL widgets off-screen before hiding | ⚠️ PARTIAL - alignment persists |
| 8 | Pre-create per-page bg panels during init, show/hide | ❌ Top-left corner bug |
| 9 | Same as #8 but without lv_obj_move_to_index | ❌ Top-left corner bug |
| 10 | Create panels during init but NEVER show/hide them | ✅ WORKS |
| 11 | Set opacity to LV_OPA_TRANSP when hiding | ❌ Didn't trigger redraw |
| 12 | lv_obj_invalidate_area() on button coords | ❌ Caused State 2 regression |
| 13 | Use large offsets (-600,-600) for aligned widgets | ❌ Didn't help - alignment persists |
| 14 | lv_obj_invalidate(widget) before hiding | ❌ Didn't help |
| 15 | **Reduce LVGL buffer from 200 to 150 lines** | ✅ **FIXED** - SPI DMA overflow was the root cause |

---

## Key LVGL Behaviors

1. **Hidden widgets don't trigger background redraw** - LVGL doesn't repaint the area
2. **Moving widgets marks old AND new position dirty** - This is key to the fix
3. **Full-screen operations corrupt render state** - Causes top-left bug
4. **Widget creation order matters** - Background widgets must be created FIRST
5. **Arc size limit** - Keep arcs ≤60px to avoid SPI DMA overflow (per CLAUDE.md)
6. **ALIGNMENT PERSISTS** - Critical discovery (see below)

---

## Alignment Persistence (Critical Discovery - 2026-02-02)

**Problem**: `lv_obj_align()` sets alignment as a STYLE PROPERTY that persists.

When you call `lv_obj_set_pos(widget, x, y)` on an aligned widget, the position values are treated as **offsets from the alignment point**, NOT absolute positions.

**Example on 502x410 screen**:
```c
// Widget aligned to BOTTOM_LEFT
lv_obj_align(widget, LV_ALIGN_BOTTOM_LEFT, 35, -30);
// Position = (0 + 35, 410 + (-30)) = (35, 380)

// Try to move off-screen with -200
lv_obj_set_pos(widget, -200, -200);
// Position = (0 + (-200), 410 + (-200)) = (-200, 210)
// Y=210 is STILL ON SCREEN!
```

**For CENTER alignment on 502x410**:
- Center is at (251, 205)
- `lv_obj_set_pos(widget, -200, -200)` = (51, 5) - still visible!

**Solution**: Use offsets larger than screen dimensions:
- For `BOTTOM_*` alignment: Y offset must be < -410
- For `*_RIGHT` alignment: X offset must be < -502
- For `CENTER` alignment: offsets must be < -251 (X) and < -205 (Y)

**Safe value**: Use `(-600, -600)` for all aligned widgets to ensure they're off-screen regardless of alignment type

---

## Critical Rule

**NEVER perform full-screen operations during mode switching.**

This includes:
- `lv_obj_set_style_bg_color(scr, ...)`
- `lv_obj_invalidate(scr)`
- `lv_refr_now()`
- Creating full-screen widgets
- Showing/hiding full-screen widgets
- Moving full-screen widgets with `lv_obj_move_to_index()`

The screen background should be set ONCE during init and never touched again.

---

## Why Face Page Always Worked

The face page works because:
1. All widgets pre-created during init
2. NO full-screen operations ever
3. Just animates existing widgets by changing position/size
4. Screen background set ONCE during init, never touched again
