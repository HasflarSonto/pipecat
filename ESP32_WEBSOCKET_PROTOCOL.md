# ESP32-Luna WebSocket Protocol Specification

## Overview

This document defines the communication protocol between the ESP32-Luna device and the Pipecat backend server. The protocol uses WebSocket for bidirectional communication with both JSON text messages and binary audio frames.

## Connection

**Endpoint**: `ws://<SERVER_IP>:7860/luna-esp32`

**Connection Flow**:
1. ESP32 connects to WiFi
2. ESP32 establishes WebSocket connection to server
3. Server accepts connection and begins streaming
4. Both sides exchange audio and commands until disconnect

**Reconnection**:
- ESP32 implements exponential backoff (starting at 1s, max 30s)
- Server maintains connection state per client

---

## Message Types

### 1. JSON Commands (Server → ESP32)

All JSON messages use the format:
```json
{"cmd": "<command>", ...additional fields}
```

#### 1.1 Emotion Command
Set the face emotion.

```json
{
  "cmd": "emotion",
  "value": "happy"
}
```

**Valid emotions**: `neutral`, `happy`, `sad`, `angry`, `surprised`, `thinking`, `confused`, `excited`, `cat`

#### 1.2 Gaze Command
Set eye tracking position.

```json
{
  "cmd": "gaze",
  "x": 0.5,
  "y": 0.5
}
```

**Fields**:
- `x`: Horizontal position, 0.0 (left) to 1.0 (right), 0.5 = center
- `y`: Vertical position, 0.0 (up) to 1.0 (down), 0.5 = center

#### 1.3 Text Display Command
Show text on display.

```json
{
  "cmd": "text",
  "content": "Hello, World!",
  "size": "large",
  "color": "#FFFFFF",
  "bg": "#1E1E28"
}
```

**Fields**:
- `content`: Text to display (max 256 chars)
- `size`: Font size - `small`, `medium`, `large`, `xlarge`
- `color`: Text color as hex (optional, default `#FFFFFF`)
- `bg`: Background color as hex (optional, default `#1E1E28`)

#### 1.4 Text Clear Command
Return to face mode.

```json
{
  "cmd": "text_clear"
}
```

#### 1.5 Pixel Art Command
Display pixel art on 12x16 grid.

```json
{
  "cmd": "pixel_art",
  "pixels": [
    {"x": 0, "y": 0, "c": "#FF0000"},
    {"x": 1, "y": 0, "c": "#00FF00"},
    {"x": 2, "y": 0, "c": "#0000FF"}
  ],
  "bg": "#1E1E28"
}
```

**Fields**:
- `pixels`: Array of pixel objects
  - `x`: Column (0-11)
  - `y`: Row (0-15)
  - `c`: Color as hex
- `bg`: Background color (optional, default `#1E1E28`)

#### 1.6 Pixel Art Clear Command
Return to face mode.

```json
{
  "cmd": "pixel_art_clear"
}
```

#### 1.7 Audio Control Commands
Control audio streaming.

```json
{"cmd": "audio_start"}
{"cmd": "audio_stop"}
```

---

### 2. Binary Audio (Bidirectional)

Audio data is sent as binary WebSocket frames with a 2-byte length header.

**Format**:
```
[2-byte BE length][PCM audio data]
```

**Audio Specification**:
- Sample Rate: 16000 Hz (16kHz)
- Bit Depth: 16-bit signed
- Channels: Mono (1 channel)
- Byte Order: Little-endian samples

**Chunk Size**: 320 samples (20ms) = 640 bytes per chunk

#### 2.1 ESP32 → Server (Microphone Audio)

ESP32 captures audio from microphone and sends to server for STT processing.

```
+--------+--------+------------------+
| Len HI | Len LO | PCM samples...   |
+--------+--------+------------------+
   1B       1B        640 bytes
```

**Example** (640 bytes of audio):
```
0x02 0x80 [640 bytes of PCM data]
```

#### 2.2 Server → ESP32 (TTS Audio)

Server sends TTS-generated audio to ESP32 for playback.

Same format as ESP32 → Server.

---

## Message Flow Examples

### Normal Conversation Flow

```
ESP32                           Server
  |                                |
  |----[WS Connect]--------------->|
  |<---[Accept]--------------------|
  |                                |
  |----[Binary: mic audio]-------->|  (continuous)
  |----[Binary: mic audio]-------->|
  |                                |
  |                                |  (STT processes audio)
  |                                |  (LLM generates response)
  |                                |  (TTS generates audio)
  |                                |
  |<---[JSON: emotion "thinking"]----|
  |<---[Binary: TTS audio]---------|
  |<---[Binary: TTS audio]---------|
  |<---[JSON: emotion "happy"]-----|
  |                                |
```

### Tool Usage Flow (e.g., display_text)

```
ESP32                           Server
  |                                |
  |<---[JSON: text display]--------|
  |    {"cmd":"text",              |
  |     "content":"The weather...",|
  |     "size":"large"}            |
  |                                |
  |    (ESP32 shows text)          |
  |                                |
  |<---[JSON: text_clear]----------|
  |                                |
  |    (ESP32 returns to face)     |
```

---

## Error Handling

### Connection Errors

ESP32 handles disconnection by:
1. Stopping audio capture
2. Displaying "confused" emotion
3. Attempting reconnection with backoff

### Invalid Commands

ESP32 logs and ignores invalid JSON commands. No error response is sent.

### Buffer Overrun

If audio buffers overflow:
- ESP32: Drops oldest audio samples
- Server: May send `audio_stop` to pause ESP32 capture

---

## Implementation Notes

### ESP32 Side

1. Use `esp_websocket_client` component
2. Process JSON with `cJSON` library
3. Audio capture/playback run in separate FreeRTOS tasks
4. Use ring buffers for audio to handle timing jitter

### Server Side (my_bot.py)

1. Add WebSocket endpoint `/luna-esp32`
2. Bridge audio between WebSocket and Pipecat pipeline
3. Forward emotion changes from LunaFaceRenderer
4. Handle tool display commands

### Latency Targets

| Path | Target |
|------|--------|
| Mic capture → server receive | < 100ms |
| Full round-trip (speak → response) | < 500ms |
| Command → display update | < 50ms |

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2025-01 | Initial specification |
