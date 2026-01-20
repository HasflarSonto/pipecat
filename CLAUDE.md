# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Pipecat is an open-source Python framework for building real-time voice and multimodal conversational AI agents. It orchestrates audio/video, AI services, transports, and conversation pipelines.

## Development Commands

```bash
# Install dependencies (dev + all extras except problematic ones)
uv sync --group dev --all-extras \
  --no-extra gstreamer \
  --no-extra krisp \
  --no-extra local

# Install pre-commit hooks
uv run pre-commit install

# Run all tests
uv run pytest

# Run specific test file
uv run pytest tests/test_name.py

# Preview changelog
towncrier build --draft --version Unreleased

# Update dependencies (after editing pyproject.toml)
uv lock && uv sync
```

## Architecture

### Core Concepts

**Frames** (`src/pipecat/frames/frames.py`): Immutable data units that flow through pipelines. Types include:
- Data frames: `AudioRawFrame`, `ImageRawFrame`, `TextFrame`, `TranscriptionFrame`
- System frames: `StartFrame`, `EndFrame`, `CancelFrame`
- Control frames: `InterruptionFrame`, `LLMFullResponseStartFrame`

**FrameProcessors** (`src/pipecat/processors/`): Process frames and pass them along. Key base classes:
- `FrameProcessor`: Base class with `process_frame()` method
- `FrameDirection`: `DOWNSTREAM` (input→output) or `UPSTREAM` (output→input)

**Pipelines** (`src/pipecat/pipeline/`): Connect processors in sequence. A typical voice bot pipeline:
```
Transport → VAD → STT → LLMContextAggregator → LLM → TTS → Transport
```

**Services** (`src/pipecat/services/`): AI service integrations organized by provider:
- STT: `services/{provider}/stt.py`
- TTS: `services/{provider}/tts.py`
- LLM: `services/{provider}/llm.py`

### Service Categories

| Type | Location | Base Class |
|------|----------|------------|
| Speech-to-Text | `services/*/stt.py` | `STTService` |
| Text-to-Speech | `services/*/tts.py` | `TTSService` |
| LLM | `services/*/llm.py` | `LLMService` |
| Speech-to-Speech | `services/*/` | Various |
| Transport | `transports/` | `BaseTransport` |

### Key Directories

- `src/pipecat/audio/`: VAD, filters, resamplers, turn detection
- `src/pipecat/processors/aggregators/`: Context management (`LLMContextAggregator`, `OpenAILLMContext`)
- `src/pipecat/transports/`: WebRTC (Daily, SmallWebRTC), WebSocket, local
- `src/pipecat/adapters/`: Tool/function schema adapters for different LLM providers

## Code Style

Uses Ruff for linting/formatting. Google-style docstrings with these conventions:

- **Classes**: Class docstring describes purpose; `__init__` has separate docstring with `Args:` section
- **Dataclasses**: Use `Parameters:` section in class docstring (no `__init__` docstring)
- **Enums**: Class docstring with `Parameters:` section describing each value
- **Properties**: Must have docstring with `Returns:` section
- **Lists in docstrings**: Use dashes (`-`), add blank line before bullet lists after colons

## Adding Dependencies

```bash
# 1. Edit pyproject.toml
# 2. Update lockfile
uv lock
# 3. Install
uv sync
# 4. Commit both files together
git add pyproject.toml uv.lock
```

Optional dependencies are defined as extras in `pyproject.toml` (e.g., `pipecat-ai[anthropic,openai]`).

## Changelog Entries

Every PR with user-facing changes needs a changelog fragment in `changelog/`:

```
changelog/<PR_number>.<type>.md
```

Types: `added`, `changed`, `deprecated`, `removed`, `fixed`, `security`, `other`

Content format (include the `-`):
```markdown
- Added support for new feature X.
```

## Creating a New Service Integration

1. Create provider directory: `src/pipecat/services/{provider}/`
2. Add `__init__.py` with public exports
3. Implement service classes inheriting from base (`STTService`, `TTSService`, `LLMService`)
4. Add optional dependency in `pyproject.toml` under `[project.optional-dependencies]`
5. Add tests in `tests/`
6. Create changelog fragment

## Python Version

- Minimum: 3.10
- Recommended: 3.12

---

## Luna Voice Bot (Local Project)

Luna is a voice assistant with an animated robot face, built on pipecat. Located in project root.

### Running Luna

```bash
cd /Users/antonioli/Desktop/pipecat
python my_bot.py
# Access: http://localhost:7860/luna
```

### Key Files

| File | Purpose |
|------|---------|
| `my_bot.py` | Main bot: pipeline, tools, WebRTC server |
| `luna_face_renderer.py` | Animated face with emotions, gaze tracking |
| `static/luna.html` | Frontend: WebRTC client, MediaPipe tracking |

### Features

- **Animated Face**: Robot-style eyes with smooth animations, blinking
- **Emotions**: neutral, happy, sad, angry, surprised, thinking, confused, excited, cat
- **Eye Tracking**: Eyes follow hand (priority) or face via MediaPipe
- **Tools**: weather, web search (Google News), time, set_emotion, draw_pixel_art, display_text, take_photo, stay_quiet
- **Voice**: Anthropic Claude 3.5 Haiku + OpenAI TTS/STT
- **Display Modes**:
  - Face: Normal animated face with emotions
  - Text: Show text/numbers/emojis (display_text tool)
  - Pixel Art: 12x16 grid drawings (draw_pixel_art tool)
  - Vision: Photo capture and analysis (take_photo tool)

### Performance Tuning

Detection intervals in `static/luna.html`:
```javascript
const FACE_DETECTION_INTERVAL_MS = 150;  // ~7 FPS (light)
const HAND_DETECTION_INTERVAL_MS = 400;  // ~2.5 FPS (heavy)
const DISABLE_HAND_TRACKING = false;     // Set true for Pi
```

### Architecture

```
Frontend (luna.html)          Backend (my_bot.py)
─────────────────────         ──────────────────
Camera → MediaPipe ─┐         ┌─ LunaFaceRenderer
                    │ WebRTC  │    ↓ video frames
Gaze data ──────────┼────────→│ on_app_message
                    │         │    ↓ set_gaze()
Audio/Video ←───────┼─────────┤ Pipeline:
                              │ Transport→VAD→STT→LLM→TTS→Transport
```

### Known Issues / TODOs

- **Wake Word Filter**: The `WakeWordFilter` class exists in `my_bot.py` but is disabled. It has lifecycle issues with pipecat's `FrameProcessor` (StartFrame handling). Re-enable when proper start handling is figured out.

---

## ESP32-Luna (Embedded Hardware Client)

Standalone ESP32-S3 hardware client that connects to the pipecat backend via WebSocket. Located in `esp32-luna/`.

### Hardware

- **Board**: Waveshare ESP32-S3-Touch-AMOLED-2.06
- **Display**: 502x410 AMOLED (SH8601, QSPI, RGB565)
- **Touch**: FT5x06 capacitive touchscreen
- **Audio**: ES8311 codec (I2S), speaker + microphone
- **Power**: AXP2101 PMU with battery support
- **Memory**: 8MB PSRAM, 32MB Flash

### Development Commands

```bash
cd esp32-luna

# Source ESP-IDF environment
source ~/esp/esp-idf/export.sh

# Build
idf.py build

# Flash (auto-detects port)
./flash.sh
# Or manually:
idf.py -p /dev/cu.usbmodem1101 flash

# Monitor serial output
idf.py -p /dev/cu.usbmodem1101 monitor

# Clean build
idf.py fullclean
```

### Key Files

| File | Purpose |
|------|---------|
| `main/main.c` | Entry point, WiFi/WebSocket connection, event handlers |
| `components/luna_face/face_renderer.c` | LVGL-based face rendering, emotions, petting |
| `components/luna_face/emotions.c` | Emotion configurations (eye size, mouth curve, etc.) |
| `components/network/ws_client.c` | WebSocket client for server communication |
| `components/network/luna_protocol.c` | JSON command parsing |
| `components/audio/audio_capture.c` | Microphone input (16kHz mono) |
| `components/audio/audio_playback.c` | Speaker output |
| `sdkconfig.defaults` | Build configuration |

### Display Modes

The ESP32 has eight display modes controlled by `face_renderer.c`:

| Mode | Enum | Description |
|------|------|-------------|
| **Face** | `DISPLAY_MODE_FACE` | Animated face with emotions, gaze tracking, petting |
| **Text** | `DISPLAY_MODE_TEXT` | Text/numbers/emojis with configurable font, color |
| **Pixel Art** | `DISPLAY_MODE_PIXEL_ART` | 12x16 grid for drawings |
| **Weather** | `DISPLAY_MODE_WEATHER` | Temperature + weather icon (sunny, cloudy, etc.) |
| **Timer** | `DISPLAY_MODE_TIMER` | Pomodoro-style countdown with arc indicator |
| **Clock** | `DISPLAY_MODE_CLOCK` | Digital time display (12h/24h) |
| **Subway** | `DISPLAY_MODE_SUBWAY` | MTA train arrival times |
| **Animation** | `DISPLAY_MODE_ANIMATION` | Animated effects (rain, snow, stars, matrix) |

**Emotions** (9 total): neutral, happy, sad, angry, surprised, thinking, confused, excited, cat

**Cat Face**: `:3` mouth using dual arcs + angled whiskers (lv_line), triggered by petting

### Touch Interactions

Touch-based interactions in face mode:

| Interaction | Trigger | Effect |
|-------------|---------|--------|
| **Petting** | Drag up/down on face | Auto-switches to cat face, face "giggles" with drag |
| **Eye Poke** | Click directly on eye | That eye winks closed briefly |
| **Dizzy** | Shake window (simulator) or D key | Face wobbles with wavy mouth for 3 seconds |

Note: These interactions only activate when in `DISPLAY_MODE_FACE`

### LLM Tools (Display-Related)

Tools the AI can call in `my_bot.py`:

| Tool | Parameters | Description |
|------|-----------|-------------|
| `set_emotion` | emotion | Set face emotion |
| `display_text` | text, font_size, color, background, duration | Show text, auto-clears |
| `draw_pixel_art` | pixels (12x16), background, duration | Draw pixel art |
| `clear_drawing` | - | Clear pixel art → face |
| `clear_text_display` | - | Clear text → face |

### Simulator

Desktop simulator for development without hardware. Located in `esp32-luna/simulator/`.

```bash
cd esp32-luna/simulator
mkdir -p build && cd build
cmake .. && cmake --build .
./luna_simulator [host] [port]  # Default: localhost:7860
```

**Keyboard Controls:**
| Key | Action |
|-----|--------|
| 1-9 | Set emotion (1=neutral, 2=happy, ...9=cat) |
| F | Face mode |
| C | Clock mode |
| W | Weather mode |
| T | Timer mode (S=start, P=pause, R=reset) |
| A | Animation mode |
| M | Subway/MTA mode |
| D | Trigger dizzy effect |
| B | Force blink |
| SPACE | Toggle demo mode |
| H | Show help |

**Mouse:** Click eye to poke, drag up/down to pet, shake window for dizzy.

### WebSocket Protocol

Connects to `ws://<SERVER_IP>:8765/luna-esp32`

**Server → ESP32 (JSON text frames):**
```json
{"cmd": "emotion", "value": "happy"}
{"cmd": "gaze", "x": 0.5, "y": 0.5}
{"cmd": "text", "content": "Hello", "size": "large", "color": "#FFFFFF", "bg": "#1E1E28"}
{"cmd": "text_clear"}
{"cmd": "pixel_art", "pixels": [{"x": 0, "y": 0, "c": "#FF0000"}], "bg": "#1E1E28"}
{"cmd": "pixel_art_clear"}
{"cmd": "weather", "temp": "72°F", "icon": "sunny", "desc": "Clear"}
{"cmd": "timer", "minutes": 25, "seconds": 0, "label": "Focus", "running": true}
{"cmd": "clock", "hours": 14, "minutes": 30, "is_24h": true}
{"cmd": "subway", "line": "1", "color": "#EE352E", "station": "110 St", "direction": "↓", "times": [3, 8, 12]}
{"cmd": "animation", "type": "rain"}
{"cmd": "clear_display"}
{"cmd": "audio_start"}
{"cmd": "audio_stop"}
```

**Server → ESP32 (Binary frames):**
- Raw 16kHz 16-bit PCM audio for playback

**ESP32 → Server (Binary frames):**
- Raw 16kHz 16-bit mono PCM audio from microphone

### Adding New Display Screens

To add a new display screen (e.g., weather, timer, clock):

**1. Add display mode enum** in `face_renderer.c`:
```c
typedef enum {
    DISPLAY_MODE_FACE,
    DISPLAY_MODE_TEXT,
    DISPLAY_MODE_PIXEL_ART,
    DISPLAY_MODE_WEATHER,    // NEW
    DISPLAY_MODE_TIMER,      // NEW
} display_mode_t;
```

**2. Add WebSocket command** in `luna_protocol.c`:
```c
else if (strcmp(cmd, "weather") == 0) {
    const char* temp = cJSON_GetStringValue(cJSON_GetObjectItem(root, "temp"));
    const char* icon = cJSON_GetStringValue(cJSON_GetObjectItem(root, "icon"));
    face_renderer_show_weather(temp, icon);
}
```

**3. Add renderer function** in `face_renderer.c`:
```c
void face_renderer_show_weather(const char* temp, const char* icon) {
    s_renderer.display_mode = DISPLAY_MODE_WEATHER;
    // Create/update LVGL widgets for weather display
}
```

**4. Add LLM tool** in `my_bot.py`:
```python
async def show_weather(self, function_name, tool_call_id, args, llm, context, result_callback):
    await self._send_esp32_command({
        "cmd": "weather",
        "temp": args.get("temp", "72°F"),
        "icon": args.get("icon", "sunny")
    })
    await result_callback({"status": "displayed"})
```

**5. Register tool** in LLM context:
```python
tools = [
    # ... existing tools ...
    {"type": "function", "function": {"name": "show_weather", "description": "Show weather on display", "parameters": {...}}}
]
```

### Configuration

WiFi and server settings in `main/Kconfig.projbuild` (menuconfig) or set via:
```bash
idf.py menuconfig
# Navigate to: Luna Configuration
```

### Architecture

```
ESP32-Luna                          Pipecat Backend
──────────                          ───────────────
Touchscreen ──→ Petting detection
                     ↓
Face Renderer ←── Emotion/Gaze ←─── WebSocket ←─── LLM tools
     ↓                                  ↑
  AMOLED Display                        │
                                        │
Microphone ───→ Audio Capture ─────────→│ (binary PCM)
                                        │
Speaker ←───── Audio Playback ←────────┘ (binary PCM)
```

### Display Constraints

- **Arc size limit**: Keep arcs ≤60px square to avoid SPI DMA overflow. Use 0-180° or 180-360° angle ranges (not custom ranges like 200-340°).
- **Off-screen initialization**: CRITICAL - All arc/line widgets MUST be positioned off-screen at creation with `lv_obj_set_pos(widget, -100, -100)`. LVGL renders widgets at (0,0) by default before positioning, causing ghost artifacts. This applies to `mouth_arc`, `cat_arc_top`, `cat_arc_bottom`, and `whisker_lines`.
- **Partial refresh**: LVGL dirty rectangles cause ghost artifacts on widget movement
- **Transform rotation**: `lv_obj_set_style_transform_rotation` causes display corruption - use `lv_line` instead
