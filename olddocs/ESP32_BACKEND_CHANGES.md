# Backend Changes Required for ESP32-Luna

## Overview

This document outlines the modifications needed to `my_bot.py` to support the ESP32-Luna device as a frontend alternative to the web-based `luna.html`.

---

## 1. Command Line Arguments

Add ESP32 mode flag:

```python
# In argument parser section
parser.add_argument(
    "--esp32",
    action="store_true",
    help="Enable ESP32 mode (WebSocket instead of WebRTC)"
)
```

---

## 2. WebSocket Endpoint

Add a new WebSocket endpoint for ESP32 connection:

```python
from fastapi import WebSocket, WebSocketDisconnect
import struct

# Global ESP32 connection state
esp32_ws: Optional[WebSocket] = None
esp32_connected = False

@app.websocket("/luna-esp32")
async def esp32_websocket_endpoint(websocket: WebSocket):
    global esp32_ws, esp32_connected

    await websocket.accept()
    esp32_ws = websocket
    esp32_connected = True
    logger.info("ESP32 connected")

    try:
        # Create audio bridge to pipecat
        audio_queue = asyncio.Queue()

        async def audio_sender():
            """Send TTS audio to ESP32"""
            while esp32_connected:
                try:
                    audio_data = await asyncio.wait_for(
                        audio_queue.get(), timeout=0.1
                    )
                    # Send with 2-byte length header (big-endian)
                    header = struct.pack(">H", len(audio_data))
                    await websocket.send_bytes(header + audio_data)
                except asyncio.TimeoutError:
                    continue

        sender_task = asyncio.create_task(audio_sender())

        # Main receive loop
        while True:
            message = await websocket.receive()

            if message["type"] == "websocket.receive":
                if "bytes" in message:
                    # Binary audio from ESP32 microphone
                    audio_data = message["bytes"]
                    # Skip 2-byte length header
                    pcm_data = audio_data[2:] if len(audio_data) > 2 else audio_data
                    # Feed to STT pipeline
                    await handle_esp32_audio(pcm_data)

                elif "text" in message:
                    # JSON command (not typically sent from ESP32)
                    logger.info(f"ESP32 text: {message['text']}")

    except WebSocketDisconnect:
        logger.info("ESP32 disconnected")
    finally:
        esp32_connected = False
        esp32_ws = None
        sender_task.cancel()
```

---

## 3. ESP32 Command Sender

Add helper function to send commands to ESP32:

```python
async def send_esp32_command(cmd: str, **kwargs):
    """Send JSON command to ESP32 if connected."""
    global esp32_ws, esp32_connected

    if not esp32_connected or esp32_ws is None:
        return

    try:
        message = {"cmd": cmd, **kwargs}
        await esp32_ws.send_json(message)
    except Exception as e:
        logger.error(f"Failed to send ESP32 command: {e}")
```

---

## 4. Modify LunaFaceRenderer

Update the face renderer to optionally send commands to ESP32 instead of rendering locally:

```python
class LunaFaceRenderer(FrameProcessor):
    def __init__(self, ..., esp32_mode: bool = False):
        super().__init__(**kwargs)
        self.esp32_mode = esp32_mode
        # ... existing init code ...

    async def set_emotion(self, emotion: str):
        """Set emotion - either render locally or send to ESP32."""
        self.target_emotion = emotion
        self.emotion_transition = 0.0

        if self.esp32_mode:
            await send_esp32_command("emotion", value=emotion)

    def set_gaze(self, x: float, y: float):
        """Set gaze target."""
        self.target_gaze_x = max(0.0, min(1.0, x))
        self.target_gaze_y = max(0.0, min(1.0, y))

        if self.esp32_mode:
            # Note: ESP32 handles gaze via DOA, this is optional
            asyncio.create_task(
                send_esp32_command("gaze", x=x, y=y)
            )
```

---

## 5. Modify Tools

Update tools that affect display to support ESP32:

### display_text tool

```python
async def display_text(text: str, font_size: str = "medium"):
    """Display text on Luna's face."""
    if args.esp32:
        await send_esp32_command(
            "text",
            content=text,
            size=font_size,
            color="#FFFFFF",
            bg="#1E1E28"
        )
    else:
        face_renderer.set_text(text, font_size)

    return {"displayed": True}
```

### draw_pixel_art tool

```python
async def draw_pixel_art(pixels: list, background: str = "#1E1E28"):
    """Draw pixel art on Luna's display."""
    if args.esp32:
        # Convert to ESP32 format
        esp32_pixels = [
            {"x": p["x"], "y": p["y"], "c": p["color"]}
            for p in pixels
        ]
        await send_esp32_command(
            "pixel_art",
            pixels=esp32_pixels,
            bg=background
        )
    else:
        face_renderer.set_pixel_art(pixels, background)

    return {"displayed": True}
```

### set_emotion tool

```python
async def set_emotion(emotion: str):
    """Set Luna's emotional expression."""
    if args.esp32:
        await send_esp32_command("emotion", value=emotion)
    else:
        face_renderer.set_emotion(emotion)

    return {"emotion": emotion}
```

---

## 6. Audio Bridge

Create audio bridge between WebSocket and Pipecat pipeline:

```python
class ESP32AudioBridge:
    """Bridge audio between ESP32 WebSocket and Pipecat pipeline."""

    def __init__(self, transport):
        self.transport = transport
        self.input_queue = asyncio.Queue(maxsize=50)
        self.output_queue = asyncio.Queue(maxsize=50)

    async def handle_esp32_audio(self, pcm_data: bytes):
        """Handle audio received from ESP32 microphone."""
        # Convert to AudioRawFrame and inject into pipeline
        # This depends on your specific Pipecat setup
        pass

    async def send_tts_audio(self, audio_data: bytes):
        """Send TTS audio to ESP32 speaker."""
        await self.output_queue.put(audio_data)
```

---

## 7. Pipeline Modifications

When `--esp32` flag is set:

1. **Disable video rendering**: Don't start the face renderer render task
2. **Create WebSocket endpoint**: Accept ESP32 connections
3. **Bridge audio**: Route ESP32 mic audio to STT, TTS output to ESP32
4. **Forward commands**: Send emotion/text/pixel_art commands via WebSocket

```python
# In main() or run() function
if args.esp32:
    # Don't render video frames locally
    face_renderer = LunaFaceRenderer(
        width=0, height=0,  # Disabled
        fps=0,
        esp32_mode=True
    )

    # Note: WebRTC transport not needed for ESP32
    # Instead, use the WebSocket endpoint
else:
    # Normal web mode
    face_renderer = LunaFaceRenderer(
        width=240, height=320,
        fps=30
    )
```

---

## 8. Running in ESP32 Mode

```bash
# Start server in ESP32 mode
python my_bot.py --esp32

# ESP32 will connect to ws://YOUR_IP:7860/luna-esp32
```

---

## Summary of Changes

| File | Change |
|------|--------|
| `my_bot.py` | Add `--esp32` argument |
| `my_bot.py` | Add `/luna-esp32` WebSocket endpoint |
| `my_bot.py` | Add `send_esp32_command()` helper |
| `my_bot.py` | Modify tools for ESP32 mode |
| `luna_face_renderer.py` | Add `esp32_mode` option |

---

## Testing

1. Start server: `python my_bot.py --esp32`
2. Power on ESP32-Luna device
3. Verify WiFi/WebSocket connection in server logs
4. Test voice conversation
5. Test emotion changes
6. Test display_text tool
7. Test pixel_art tool
