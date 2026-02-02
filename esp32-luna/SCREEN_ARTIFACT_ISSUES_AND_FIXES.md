# Screen Artifact Issues and Fixes

This document tracks LVGL display artifact issues encountered on the ESP32-Luna project and their solutions.

## Current Status (2026-02-02) - ALMOST FIXED

| Page | Status | Details |
|------|--------|---------|
| Face | ✅ Working | Eyes render correctly, distressed effect works |
| Weather | ✅ Working | Text/symbols show correctly |
| Clock | ✅ Working | Text/symbols show correctly |
| Timer | ✅ Working | Display works |
| Calendar | ✅ Working | Display works |
| Subway | ✅ Working | Display works |

---

## The Fix

**DO NOT perform any full-screen operations during mode switching.**

The solution is to:
1. Set screen background ONCE during `face_renderer_init()` - never touch it again
2. Move widgets off-screen (-200, -200) AND hide them when switching modes
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
| 4 | Move widgets off-screen (-200, -200) AND hide | ✅ WORKS - key part of solution |
| 5 | Create full-screen background panel during hide_all | ❌ Top-left corner bug |
| 6 | Delete/recreate full-screen panel each switch | ❌ Top-left corner bug |
| 7 | Move ALL widgets off-screen before hiding | ✅ WORKS - part of solution |
| 8 | Pre-create per-page bg panels during init, show/hide | ❌ Top-left corner bug |
| 9 | Same as #8 but without lv_obj_move_to_index | ❌ Top-left corner bug |
| 10 | Create panels during init but NEVER show/hide them | ✅ WORKS |

---

## Key LVGL Behaviors

1. **Hidden widgets don't trigger background redraw** - LVGL doesn't repaint the area
2. **Moving widgets marks old AND new position dirty** - This is key to the fix
3. **Full-screen operations corrupt render state** - Causes top-left bug
4. **Widget creation order matters** - Background widgets must be created FIRST
5. **Arc size limit** - Keep arcs ≤60px to avoid SPI DMA overflow (per CLAUDE.md)

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
