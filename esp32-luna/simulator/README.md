# Luna ESP32 Simulator

Desktop simulator for ESP32-Luna, allowing development and testing without hardware.

## Features

- **LVGL Display**: 502x410 SDL2 window rendering the Luna face
- **WebSocket**: Connects to pipecat backend for full voice AI pipeline
- **Audio**: System microphone capture and speaker playback (16kHz mono)
- **Touch/Petting**: Mouse drag to pet the face (triggers cat mode)
- **Multiple Display Modes**: Face, clock, weather, timer, subway, calendar, animations

## Prerequisites

Install dependencies via Homebrew (macOS):

```bash
brew install sdl2 libwebsockets cjson cmake
```

## Building

```bash
cd esp32-luna/simulator
mkdir build && cd build
cmake ..
make
```

## Running

```bash
# Connect to localhost:7860 (default)
./luna_simulator

# Connect to custom server
./luna_simulator 192.168.1.100 7860
```

## Keyboard Controls

| Key | Mode | Description |
|-----|------|-------------|
| **F** | Face | Animated face with emotions |
| **C** | Clock | Apple Watch style time display |
| **W** | Weather | Weather display (cycles: sunny/cloudy/rainy) |
| **T** | Timer | Pomodoro timer with arc progress |
| **M** | Subway | MTA subway arrivals display |
| **G** | Calendar | Apple Watch style event cards |
| **A** | Animation | Rain/snow/stars effects |
| **D** | - | Trigger dizzy effect (wobbly face) |
| **H** | - | Show help |
| **ESC** | - | Exit simulator |

## Mouse Interactions

- **Drag on face**: Pet the face (switches to cat :3 mode)
- **Click on eye**: Poke eye (causes wink)

## Display Modes

### Face Mode (default)
Animated robot face with:
- Smooth eye movements and blinking
- Emotion expressions (happy, sad, angry, surprised, thinking, confused, excited, cat)
- Gaze tracking from server commands
- Dizzy effect with wavy mouth

### Clock Mode
Apple Watch inspired design:
- Large white time display
- Orange date label
- AM/PM indicator

### Weather Mode
Simple, clean icons:
- Sun with dot rays (yellow)
- Cloud as overlapping circles
- Rain drops as rounded rectangles
- Large temperature display

### Timer Mode
Pomodoro-style countdown:
- Circular progress arc
- Color changes (green/orange/red)
- Start/Pause/Reset via touch

### Subway Mode
MTA-style arrivals:
- Line bullet with color
- Station name and direction
- Multiple arrival times

### Calendar Mode
Apple Watch event cards:
- Time range in blue
- Event title in white
- Location in gray

## Architecture

```
+-----------------------------------------------------+
|                    main.c                           |
|  +----------+  +----------+  +------------------+   |
|  |  LVGL    |  |   SDL2   |  |   libwebsockets  |   |
|  | Display  |  |  Audio   |  |    WebSocket     |   |
|  +----+-----+  +----+-----+  +--------+---------+   |
|       |             |                 |             |
|       v             v                 v             |
|  +----------------------------------------------+   |
|  |         Luna Face Renderer (C)               |   |
|  |     (shared with ESP32 hardware code)        |   |
|  +----------------------------------------------+   |
+-----------------------------------------------------+
                        |
                        | WebSocket
                        v
+-----------------------------------------------------+
|              Pipecat Backend (Python)               |
|  +----------+  +----------+  +------------------+   |
|  |   STT    |  |   LLM    |  |       TTS        |   |
|  +----------+  +----------+  +------------------+   |
+-----------------------------------------------------+
```

## WebSocket Protocol

Same as ESP32-Luna hardware:

**JSON Commands (Server -> Simulator):**
```json
{"cmd": "emotion", "value": "happy"}
{"cmd": "gaze", "x": 0.5, "y": 0.5}
{"cmd": "text", "content": "Hello", "size": "large"}
{"cmd": "text_clear"}
{"cmd": "weather", "temp": "72F", "icon": "sunny", "desc": "Clear skies"}
{"cmd": "clock", "hours": 12, "minutes": 34, "date": "TUE JAN 21"}
{"cmd": "timer", "minutes": 25, "seconds": 0, "label": "Focus"}
{"cmd": "subway", "line": "1", "color": "#EE352E", "station": "110 St", "direction": "Downtown", "times": [3, 8, 12]}
{"cmd": "calendar", "events": [{"time": "10:00 AM", "title": "Meeting", "location": "Room 3"}]}
{"cmd": "audio_start"}
{"cmd": "audio_stop"}
```

**Binary Audio (Bidirectional):**
- 16kHz, 16-bit signed, mono PCM

## Troubleshooting

### No audio capture
- Check system microphone permissions
- Ensure SDL2 was built with audio support

### WebSocket connection failed
- Verify pipecat backend is running
- Check firewall settings
- Try `localhost` instead of `127.0.0.1`

### Display artifacts
- This can happen during emotion transitions
- Usually resolves within a few frames
