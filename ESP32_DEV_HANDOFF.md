# ESP32 Luna Frontend - Developer Handoff

This document summarizes the current state of the ESP32 frontend work for developer handoff.

## Key Files to Work On

| File | Location | Description |
|------|----------|-------------|
| `face_renderer.c` | `esp32-luna/components/luna_face/` | Main face rendering logic (1042 lines) |
| `face_renderer.h` | `esp32-luna/components/luna_face/` | Public API |
| `emotions.c` | `esp32-luna/components/luna_face/` | Emotion parameter definitions |
| `emotions.h` | `esp32-luna/components/luna_face/` | Emotion types and structs |
| `main.c` | `esp32-luna/main/` | Application entry point |

## Current Architecture

### Display
- **Hardware**: Waveshare ESP32-S3 Touch AMOLED 2.06"
- **Resolution**: 502x410 (landscape, rotated 90° from native 410x502)
- **Rendering**: Widget-based using LVGL (NOT canvas - canvas caused SPI overflow)
- **Frame Rate**: ~5 FPS (200ms period) to avoid SPI bus saturation

### Scale Factors
```c
SCALE_X = 2.092  // 502/240
SCALE_Y = 1.281  // 410/320
```

### Face Geometry (Landscape Mode)
```
Center: (251, 185)
Eye spacing: ~115 pixels
Left eye X: 136, Right eye X: 366
Eye base Y: 156
Mouth base Y: 241
```

---

## Known Issues (Need Fixing)

### 1. Smile/Frown Shows as Neutral

**Location**: `face_renderer.c` lines 367-418

**Problem**: The mouth always appears as a straight horizontal line regardless of emotion.

**Current Implementation**: We removed `lv_arc` widgets because they caused rendering artifacts. Now using simple line positioning:
- Smile: Line positioned +25 pixels DOWN from base
- Frown: Line positioned -25 pixels UP from base
- Neutral: Line at base position

**Why It Might Not Work**:
1. The 25-pixel offset may not be visually noticeable on the display
2. The `last_mouth_curve` tracking might prevent updates
3. The curve_category thresholds might be wrong

**Debug Output**: Look for these log messages:
```
I (xxx) face_renderer: Mouth curve changed: X -> Y (mouth_curve=Z.ZZ)
I (xxx) face_renderer: Mouth pos: y=XXX (base=250), len=XXX
```

**Suggested Fixes**:
- Increase offset from 25 to 40-50 pixels
- Add visible width/color change for smile vs frown
- Consider adding curved line segments using multiple points

### 2. Cat Face Not Distinctive

**Location**: `face_renderer.c` lines 389-394

**Current**: Just positions mouth line lower with 3x width

**Suggested Fixes**:
- Add "whisker" lines on sides
- Make eyes more cat-like (narrower, angled)
- Add ear shapes at top corners

### 3. Excited/Sparkle Face Untested

**Location**: `face_renderer.c` lines 335-359

**Current**: Small circles should appear near eye centers when `sparkle=true`

**Code exists but needs verification**:
```c
if (sparkle && !s_renderer.sparkles_visible) {
    // Show sparkle circles near eyes
    lv_obj_remove_flag(s_renderer.left_sparkle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_renderer.right_sparkle, LV_OBJ_FLAG_HIDDEN);
}
```

---

## Mouth Rendering Deep Dive

The mouth uses a **curve category** system:

| Category | Value | Condition | Appearance |
|----------|-------|-----------|------------|
| Frown | -1 | `mouth_curve < -0.1` | Line 25px UP |
| Neutral | 0 | `abs(mouth_curve) < 0.1` | Line at base |
| Smile | 1 | `mouth_curve > 0.1` | Line 25px DOWN |
| Surprised | 50 | `mouth_open > 0.3` | Thick square (35x35) |
| Cat | 100 | `cat_face = true` | Wide line, much lower |

**Critical Code Block** (`face_renderer.c:381-416`):
```c
if (curve_category != s_renderer.last_mouth_curve) {
    int line_len;
    int line_y = mouth_y;
    int line_h = line_width;

    if (curve_category == 100) {        // Cat
        line_len = (int)(mouth_width * 3.0f);
        line_y = mouth_y + (int)(25 * SCALE_Y);
    } else if (curve_category == 50) {  // Surprised
        line_len = (int)(35 * SCALE_X);
        line_h = (int)(35 * SCALE_Y);
    } else if (curve_category == 0) {   // Neutral
        line_len = (int)(mouth_width * 1.5f);
    } else if (curve_category == 1) {   // Smile
        line_len = (int)(mouth_width * 3.0f);
        line_y = mouth_y + (int)(25 * SCALE_Y);
    } else {                            // Frown
        line_len = (int)(mouth_width * 3.0f);
        line_y = mouth_y - (int)(25 * SCALE_Y);
    }

    lv_obj_set_size(s_renderer.mouth_line, line_len, line_h);
    lv_obj_set_pos(s_renderer.mouth_line, mouth_x - line_len/2, line_y - line_h/2);
    s_renderer.last_mouth_curve = curve_category;
}
```

---

## Why We Abandoned Arc Widgets

Previously tried using `lv_arc` for curved smiles/frowns. Issues encountered:

1. **White line artifacts** accumulated over time on the display
2. **Arcs didn't render** at all for some configurations
3. **SPI queue overflow** when updating arc parameters frequently

The line-based approach eliminated these artifacts but lost visual expression variety.

---

## Emotion Parameters Reference

From `emotions.c`:

| Emotion | eye_height | eye_width | mouth_curve | mouth_open | sparkle | cat_face |
|---------|------------|-----------|-------------|------------|---------|----------|
| Neutral | 50 | 40 | 0.0 | 0.0 | false | false |
| Happy | 55 | 42 | 0.8 | 0.0 | false | false |
| Sad | 45 | 38 | -0.9 | 0.0 | false | false |
| Angry | 40 | 45 | -0.5 | 0.0 | false | false |
| Surprised | 70 | 50 | 0.2 | 0.8 | false | false |
| Thinking | 48 | 40 | 0.1 | 0.0 | false | false |
| Confused | 52 | 42 | -0.3 | 0.15 | false | false |
| Excited | 65 | 48 | 1.0 | 0.0 | true | false |
| Cat | 60 | 40 | 0.5 | 0.0 | false | true |

---

## Testing Tips

### Emotion Cycle Demo
When not connected to the backend server, the ESP32 automatically cycles through all emotions every 3 seconds. Watch the serial monitor for emotion changes.

### Manual Testing
1. Build and flash: `idf.py build && idf.py -p /dev/cu.usbmodem1101 flash monitor`
2. Watch for emotion logs in serial output
3. Disconnect from WiFi to trigger the demo cycle

### Debug Logging
Current debug output includes:
- `Mouth curve changed: X -> Y` - Category transitions
- `Mouth pos: y=XXX (base=250)` - Actual Y position being set
- `Emotion set to: XXX` - Which emotion was requested

---

## Build Commands

```bash
# Setup environment
cd /Users/antonioli/Desktop/pipecat/esp32-luna
source $HOME/esp/esp-idf/export.sh

# Build
idf.py build

# Flash and monitor
idf.py -p /dev/cu.usbmodem1101 flash monitor

# Just monitor (Ctrl+] to exit)
idf.py -p /dev/cu.usbmodem1101 monitor
```

---

## Backend Communication

The ESP32 connects to the Pipecat backend via WebSocket:
- **Endpoint**: `ws://SERVER_IP:7860/luna-esp32`
- **Protocol**: See `ESP32_WEBSOCKET_PROTOCOL.md`

Key commands from server:
```json
{"cmd": "emotion", "value": "happy"}
{"cmd": "gaze", "x": 0.5, "y": 0.5}
```

---

## Files Modified Recently

1. `face_renderer.c` - Removed arc widgets, added line-based mouth, increased eye height 40%
2. `wifi_manager.c` - Added event group clear for connection stability
3. `main.c` - Made WiFi connection non-fatal (won't crash on timeout)
4. `audio_capture.c` - Throttled ring buffer overflow warnings

---

## Related Documentation

- **`ESP32_WEBSOCKET_PROTOCOL.md`** - WebSocket communication protocol spec
- **`ESP32_BUILD_COMMANDS.md`** - Build and flash commands
- **`olddocs/`** - Archived docs (migration plans, hardware specs, etc.)

---

## Contact

For questions about the backend integration, see `my_bot.py` in the parent directory.
