# ESP32-S3 Luna Migration Plan (Revised)

## Overview

Port Luna voice assistant from web frontend to ESP32-S3 hardware:
- **Hardware**: Waveshare ESP32-S3-Touch-AMOLED-2.06
- **Display**: 410×502 AMOLED (CO5300 driver, QSPI)
- **Audio**: ES8311 codec (stereo mic input + speaker output)
- **Eye Tracking**: Sound-based DOA using stereo microphones (if spacing permits)
- **Backend**: Remains on computer, ESP32 connects via WiFi/WebSocket
- **Removed**: Camera, hand tracking, visual eye tracking (MediaPipe)

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      COMPUTER (Backend)                          │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Pipecat Server (my_bot.py --esp32)                       │  │
│  │  ├─ Anthropic Claude LLM                                  │  │
│  │  ├─ OpenAI TTS/STT                                        │  │
│  │  └─ WebSocket Server (ws://IP:7860/luna-esp32)            │  │
│  │      ├─ JSON: emotion/gaze/text/pixel_art commands        │  │
│  │      └─ Binary: PCM audio streams                         │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              │
                        WiFi/WebSocket
                              │
┌─────────────────────────────────────────────────────────────────┐
│                      ESP32-S3 (Frontend)                         │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  WebSocket Client                                          │  │
│  │  ├─ WiFi Manager (NVS credentials, auto-reconnect)        │  │
│  │  ├─ JSON command parser (emotion, gaze, text, pixel_art)  │  │
│  │  └─ Binary audio streams (16kHz PCM)                      │  │
│  └───────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Audio Pipeline (based on esp-brookesia patterns)         │  │
│  │  ├─ ES8311 Codec Driver (I2S)                             │  │
│  │  ├─ Audio Capture (mic → server)                          │  │
│  │  ├─ Audio Playback (server → speaker)                     │  │
│  │  └─ Optional: VAD (Voice Activity Detection)              │  │
│  └───────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Luna Face Renderer (C/LVGL)                              │  │
│  │  ├─ 410×502 AMOLED via CO5300 QSPI                        │  │
│  │  ├─ 9 Emotions (neutral, happy, sad, angry, etc.)         │  │
│  │  ├─ Eye animation (blink, gaze tracking)                  │  │
│  │  ├─ Text display mode                                     │  │
│  │  └─ Pixel art mode (12×16 grid)                           │  │
│  └───────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Power Management (AXP2101)                               │  │
│  │  ├─ Battery monitoring                                    │  │
│  │  ├─ Charging state                                        │  │
│  │  └─ Sleep/wake states                                     │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Key Technical Decisions

### 1. WebSocket over WebRTC
- **Decision**: Use WebSocket instead of WebRTC
- **Reason**: WebRTC is designed for P2P browser connections with NAT traversal. For local server connection, WebSocket is simpler and sufficient.
- **Protocol**: JSON for commands, binary frames for audio

### 2. Local Face Rendering
- **Decision**: Render face locally on ESP32
- **Reason**: Less bandwidth (~50 bytes/command vs ~100KB/frame), more responsive, works offline
- **Trade-off**: More code on ESP32, but LVGL makes this straightforward

### 3. Audio Format
- **Sample Rate**: 16kHz (Pipecat/OpenAI standard)
- **Format**: PCM 16-bit signed, mono
- **Chunk Size**: 320 samples (20ms) per WebSocket frame

### 4. Base Implementation
- **Audio**: Based on `esp-brookesia/ai_framework/agent/audio_processor.c` patterns
- **Display**: Based on `05_Spec_Analyzer` LVGL canvas patterns
- **Power**: Based on `01_AXP2101` example

---

## Phase 1: Foundation Setup ✅ (DONE)

- [x] ESP-IDF v5.4.2 installed
- [x] Project structure created (`esp32-luna/`)
- [x] Working examples available (Spec_Analyzer, esp-brookesia)
- [x] Hardware documentation reviewed

---

## Phase 2: Network & Protocol

### 2.1 WiFi Manager

**Component**: `esp32-luna/components/network/`

**Tasks**:
1. WiFi STA mode connection
2. NVS storage for credentials
3. Auto-reconnect on disconnect
4. Optional: mDNS for server discovery

**Files**:
- `wifi_manager.h/c`

### 2.2 WebSocket Client

**Tasks**:
1. Connect to `ws://SERVER_IP:7860/luna-esp32`
2. Handle JSON text frames (commands)
3. Handle binary frames (audio)
4. Reconnection with exponential backoff

**Files**:
- `ws_client.h/c`
- `luna_protocol.h/c`

### 2.3 Communication Protocol

**Server → ESP32 (JSON commands)**:
```json
{"cmd": "emotion", "value": "happy"}
{"cmd": "gaze", "x": 0.7, "y": 0.3}
{"cmd": "text", "content": "Hello!", "size": "large", "color": "#FFFFFF"}
{"cmd": "text_clear"}
{"cmd": "pixel_art", "pixels": [{"x":0,"y":0,"c":"#FF0000"}], "bg": "#1E1E28"}
{"cmd": "pixel_art_clear"}
{"cmd": "audio_start"}
{"cmd": "audio_stop"}
```

**ESP32 → Server (Binary audio)**:
```
[2-byte BE length][PCM samples...]
```

**Server → ESP32 (Binary audio)**:
```
[2-byte BE length][PCM samples...]
```

---

## Phase 3: Audio Pipeline

### 3.1 Audio Manager

**Base on**: `esp-brookesia/ai_framework/agent/audio_processor.c`

**Tasks**:
1. Initialize ES8311 codec via BSP
2. Configure I2S for 16kHz/16-bit/stereo input, mono output
3. Set playback volume (default 70%)

**Files**:
- `components/audio/audio_manager.h/c`

### 3.2 Audio Capture

**Tasks**:
1. I2S DMA read from ES8311
2. Stereo → mono conversion (average L+R channels)
3. Ring buffer for WebSocket sending
4. Optional: VAD integration (from esp-brookesia)

**API**:
```c
esp_err_t audio_capture_init(void);
esp_err_t audio_capture_start(void);
esp_err_t audio_capture_stop(void);
int audio_capture_read(uint8_t *buf, size_t len, TickType_t timeout);
```

**Files**:
- `components/audio/audio_capture.h/c`

### 3.3 Audio Playback

**Tasks**:
1. FIFO buffer for incoming audio
2. I2S DMA write to ES8311
3. Handle buffer underrun gracefully

**API**:
```c
esp_err_t audio_playback_init(void);
esp_err_t audio_playback_start(void);
esp_err_t audio_playback_stop(void);
esp_err_t audio_playback_feed(const uint8_t *data, size_t len);
```

**Files**:
- `components/audio/audio_playback.h/c`

---

## Phase 4: Face Renderer

### 4.1 Display Setup

**Hardware**:
- Resolution: 410×502 pixels (portrait)
- Driver: CO5300 via QSPI
- Color: RGB565 (16-bit)
- Target FPS: 20-30

**Base on**: BSP display functions from examples

### 4.2 Face Renderer Port

**Port from**: `luna_face_renderer.py`

**Emotion configurations**:
```c
typedef struct {
    float eye_height;      // Taller = more alert
    float eye_width;       // Wider = more surprised
    float eye_openness;    // 0-1, affects blink
    float mouth_curve;     // Positive = smile, negative = frown
    float mouth_open;      // 0-1, for surprised "O" shape
    float mouth_width;     // Mouth size
    bool angry_brows;      // Angled eyebrows
    bool look_side;        // Eyes look sideways (thinking)
    bool tilt_eyes;        // One eye higher (confused)
    bool sparkle;          // Sparkle effect (excited)
    bool cat_face;         // Cat mouth style
} emotion_config_t;

// 9 emotions: neutral, happy, sad, angry, surprised, thinking, confused, excited, cat
```

**Scale factor**: Original 240×320 → Target 410×502 = 1.7x scale

**Animation**:
- Blink every 3-5 seconds (random)
- Smooth emotion transitions (lerp over 250ms)
- Gaze follow with interpolation (10.0 speed factor)

**Files**:
```
components/luna_face/
├── face_renderer.h/c      # Main render loop, LVGL integration
├── emotions.h             # Emotion config structs and data
├── eye_renderer.h/c       # Eye drawing (rounded rects, pupils)
├── mouth_renderer.h/c     # Mouth curves and shapes
├── text_mode.h/c          # Text display with LVGL labels
└── pixel_art.h/c          # 12×16 grid scaled to display
```

### 4.3 Display Modes

1. **Face Mode** (default)
   - Animated robot/cat face
   - Emotion display
   - Eye tracking (gaze)
   - Blinking

2. **Text Mode**
   - Large text display
   - Word wrapping
   - Multiple sizes: small (20px), medium (32px), large (44px), xlarge (64px)
   - Scaled for 410×502 display

3. **Pixel Art Mode**
   - 12×16 grid
   - Each cell ~34×31 pixels on 410×502 display
   - Auto-centering

---

## Phase 5: DOA Eye Tracking (Optional)

**Note**: Effectiveness depends on physical microphone spacing. If mics are <5cm apart, DOA accuracy will be limited.

### 5.1 TDOA Calculation

```c
// Cross-correlation to find time delay between channels
float calculate_tdoa(const int16_t *left, const int16_t *right, size_t samples) {
    // Find peak in cross-correlation
    // Return sample offset
}

// Convert to angle
float tdoa_to_angle(float sample_offset, float sample_rate, float mic_spacing) {
    float time_delay = sample_offset / sample_rate;
    float angle = asinf(time_delay * SPEED_OF_SOUND / mic_spacing);
    return angle;  // Radians, -π/2 to +π/2
}

// Convert to gaze coordinates
void angle_to_gaze(float angle, float *gaze_x, float *gaze_y) {
    *gaze_x = 0.5f + 0.5f * sinf(angle);  // 0-1 range
    *gaze_y = 0.5f;  // Keep centered vertically
}
```

### 5.2 Alternative: Volume-Based Attention

If DOA is not accurate enough:
- Track audio energy level
- High energy = "attentive" look (eyes forward)
- Low energy = "idle" look (slight drift)

**Files**:
- `components/audio/doa_processor.h/c`

---

## Phase 6: Power Management

### 6.1 AXP2101 Driver

**Base on**: `01_AXP2101` example

**Tasks**:
1. I2C communication with PMU
2. Battery voltage/percentage reading
3. Charging state detection
4. Temperature monitoring

### 6.2 Power States

| State | Display | WiFi | Audio | Trigger |
|-------|---------|------|-------|---------|
| Active | Full brightness | Connected | Active | User speaking |
| Idle | Dim (50%) | Connected | Listening | 30s no activity |
| Sleep | Off | Maintain | Wake-on-VAD | 5min no activity |

**Files**:
```
components/power/
├── pmu_manager.h/c        # AXP2101 interface
└── power_states.h/c       # State machine
```

---

## Phase 7: Integration & Testing

### 7.1 Main Application

**File**: `esp32-luna/main/main.c`

**Initialization order**:
1. NVS init
2. Power manager init (AXP2101)
3. Display init (LVGL + CO5300)
4. Audio manager init (ES8311)
5. WiFi connect
6. WebSocket connect
7. Start face renderer task
8. Start audio tasks

**FreeRTOS Tasks**:
| Task | Priority | Stack | Core | Purpose |
|------|----------|-------|------|---------|
| audio_capture | 6 | 4KB | 0 | Mic → WebSocket |
| audio_playback | 6 | 4KB | 0 | WebSocket → Speaker |
| face_renderer | 5 | 8KB | 1 | Display updates |
| ws_client | 5 | 6KB | 0 | Network I/O |

### 7.2 Backend Modifications

**File**: `my_bot.py`

**Changes needed**:
```python
# Add argument
parser.add_argument("--esp32", action="store_true", help="Enable ESP32 mode")

# Add WebSocket endpoint
@app.websocket("/luna-esp32")
async def esp32_handler(websocket: WebSocket):
    await websocket.accept()
    # Handle JSON commands and binary audio

# When emotion changes (in LunaFaceRenderer or tools)
if esp32_mode:
    await esp32_ws.send_json({"cmd": "emotion", "value": emotion})
```

### 7.3 Test Matrix

**Connectivity**:
- [ ] WiFi connects on boot
- [ ] WiFi reconnects after disconnect
- [ ] WebSocket connects to server
- [ ] WebSocket reconnects after disconnect

**Audio**:
- [ ] Microphone captures audio
- [ ] Audio streams to server
- [ ] Server STT receives audio correctly
- [ ] TTS audio plays on speaker
- [ ] Audio latency < 500ms round-trip

**Display**:
- [ ] Face renders correctly
- [ ] All 9 emotions display
- [ ] Eyes blink at random intervals
- [ ] Gaze tracking works (if DOA implemented)
- [ ] Text mode displays correctly
- [ ] Pixel art mode displays correctly
- [ ] Display FPS >= 15

**Power**:
- [ ] Battery level reads correctly
- [ ] Charging state detects correctly
- [ ] Idle dim works after timeout
- [ ] Sleep mode works (if implemented)

**Integration**:
- [ ] Full conversation flow works
- [ ] Emotion changes from LLM reflect on display
- [ ] Tools (weather, time, etc.) work
- [ ] 10+ minute stable session

---

## File Structure

```
esp32-luna/
├── main/
│   ├── main.c                     # Application entry point
│   └── CMakeLists.txt
├── components/
│   ├── audio/
│   │   ├── audio_manager.h/c      # ES8311 codec setup
│   │   ├── audio_capture.h/c      # Microphone input
│   │   ├── audio_playback.h/c     # Speaker output
│   │   ├── doa_processor.h/c      # Direction of arrival (optional)
│   │   └── CMakeLists.txt
│   ├── network/
│   │   ├── wifi_manager.h/c       # WiFi connection
│   │   ├── ws_client.h/c          # WebSocket client
│   │   ├── luna_protocol.h/c      # Command parser
│   │   └── CMakeLists.txt
│   ├── luna_face/
│   │   ├── face_renderer.h/c      # Main render loop
│   │   ├── emotions.h             # Emotion configurations
│   │   ├── eye_renderer.h/c       # Eye drawing
│   │   ├── mouth_renderer.h/c     # Mouth drawing
│   │   ├── text_mode.h/c          # Text display
│   │   ├── pixel_art.h/c          # Pixel art mode
│   │   └── CMakeLists.txt
│   └── power/
│       ├── pmu_manager.h/c        # AXP2101 interface
│       ├── power_states.h/c       # Power state machine
│       └── CMakeLists.txt
├── managed_components/            # ESP-IDF component dependencies
├── CMakeLists.txt
├── sdkconfig.defaults
└── partitions.csv
```

---

## Implementation Order

### Step 1: Network Foundation
1. WiFi manager with NVS credentials
2. WebSocket client (esp_websocket_client)
3. Simple echo test with server

### Step 2: Audio Loopback
1. ES8311 initialization (copy from Spec_Analyzer BSP)
2. Audio capture task
3. Audio playback task
4. Test: mic → speaker loopback locally

### Step 3: Audio Streaming
1. Send mic audio over WebSocket
2. Receive TTS audio from WebSocket
3. Test: ESP32 mic → server → ESP32 speaker

### Step 4: Face Renderer (Static)
1. LVGL display setup
2. Port emotion configs from Python
3. Render static face (neutral emotion)
4. Test: Display shows face

### Step 5: Face Animation
1. Blink animation
2. Emotion transitions
3. Command handler for emotion changes
4. Test: Server sends emotion, ESP32 displays

### Step 6: Display Modes
1. Text mode
2. Pixel art mode
3. Command handlers
4. Test: All display modes work

### Step 7: DOA (Optional)
1. Stereo capture
2. Cross-correlation
3. Gaze updates
4. Test: Eyes follow sound

### Step 8: Power Management
1. AXP2101 driver
2. Battery monitoring
3. Power states
4. Test: Battery level, charging detection

### Step 9: Integration & Polish
1. Full conversation test
2. Error handling
3. Performance tuning
4. Documentation

---

## Hardware Reference

**ESP32-S3-Touch-AMOLED-2.06 Key Specs**:
- MCU: ESP32-S3R8 (dual-core 240MHz, 512KB SRAM, 8MB PSRAM, 32MB Flash)
- Display: 410×502 AMOLED, CO5300 QSPI, FT3168 touch
- Audio: ES8311 codec, I2S, stereo mic, mono speaker
- Power: AXP2101 PMU, 400mAh battery
- Sensors: QMI8658 IMU, PCF85063 RTC
- Connectivity: WiFi 2.4GHz, BLE 5

**Pin Reference** (from BSP):
- Display: QSPI (LCD_CS, LCD_SCLK, LCD_SDIO0-3)
- Audio: I2S (I2S_BCK, I2S_WS, I2S_DIN, I2S_DOUT)
- I2C: SDA/SCL for ES8311, AXP2101, touch
- Buttons: BOOT (GPIO0), PWR (via AXP2101)

---

## Success Criteria

- [ ] ESP32 connects to backend via WiFi/WebSocket
- [ ] Voice conversation works end-to-end
- [ ] Audio latency < 500ms (mic to response)
- [ ] Face renders at 15+ FPS
- [ ] All emotions display correctly
- [ ] Text and pixel art modes work
- [ ] Battery level displayed/monitored
- [ ] Stable for 10+ minute sessions
- [ ] Memory usage < 80% of PSRAM

---

## References

**ESP-IDF Examples**:
- `ESP-IDF-v5.4.2/05_Spec_Analyzer/` - Audio capture + LVGL display
- `ESP-IDF-v5.4.2/03_esp-brookesia/` - AI audio pipeline (VAD, AEC)
- `ESP-IDF-v5.4.2/01_AXP2101/` - Power management
- `ESP-IDF-v5.4.2/02_lvgl_demo_v9/` - LVGL v9 patterns

**Documentation**:
- `ESP32_HARDWAREDOCS.md` - Hardware specifications
- `luna_face_renderer.py` - Python face renderer (port source)
- `my_bot.py` - Backend server

---

**Last Updated**: January 2025
**Status**: Planning Complete - Ready for Implementation
