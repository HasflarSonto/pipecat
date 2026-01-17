# Luna ESP32 Simulator

Desktop simulator for ESP32-Luna, allowing development and testing without hardware.

## Features

- **LVGL Display**: 502x410 SDL2 window rendering the Luna face
- **WebSocket**: Connects to pipecat backend for full voice AI pipeline
- **Audio**: System microphone capture and speaker playback (16kHz mono)
- **Touch/Petting**: Mouse drag to pet the face (triggers cat mode)

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

## Controls

- **ESC** or close window: Exit simulator
- **Mouse drag**: Pet the face (triggers cat :3 mode)

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    main.c                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │  LVGL    │  │   SDL2   │  │   libwebsockets  │   │
│  │ Display  │  │  Audio   │  │    WebSocket     │   │
│  └────┬─────┘  └────┬─────┘  └────────┬─────────┘   │
│       │             │                 │             │
│       ▼             ▼                 ▼             │
│  ┌──────────────────────────────────────────────┐   │
│  │         Luna Face Renderer (C)               │   │
│  │     (shared with ESP32 hardware code)        │   │
│  └──────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
                        │
                        │ WebSocket
                        ▼
┌─────────────────────────────────────────────────────┐
│              Pipecat Backend (Python)               │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │   STT    │  │   LLM    │  │       TTS        │   │
│  └──────────┘  └──────────┘  └──────────────────┘   │
└─────────────────────────────────────────────────────┘
```

## WebSocket Protocol

Same as ESP32-Luna hardware:

**JSON Commands (Server → Simulator):**
```json
{"cmd": "emotion", "value": "happy"}
{"cmd": "gaze", "x": 0.5, "y": 0.5}
{"cmd": "text", "content": "Hello", "size": "large"}
{"cmd": "text_clear"}
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
