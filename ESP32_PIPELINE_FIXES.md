# ESP32-Luna Voice Pipeline - Bug Fixes Plan

## Overview

The ESP32-Luna voice pipeline is **functional but buggy**. All major components work but each has issues that need addressing. This document provides a comprehensive fix plan for both backend (Python/Pipecat) and frontend (ESP32/C).

## Pipeline Status Summary

| Component | Status | Issue |
|-----------|--------|-------|
| ESP32 → Backend Audio | Works | Ring buffer overflow, frequent disconnects |
| STT (Speech-to-Text) | Works | No major issues |
| LLM Processing | Works | Was overusing `stay_quiet` tool (fixed) |
| TTS Generation | Works | Audio generated but delivery inconsistent |
| Backend → ESP32 Audio | Partially Works | Stereo/mono mismatch causes gibberish |
| ESP32 Playback | Partially Works | Buffer overflow, wrong channel config |
| WiFi/WebSocket | Unstable | Frequent disconnects, demo mode fallback |

---

## Issue 1: Audio Gibberish on ESP32 Playback

### Root Cause
The ESP32 codec is configured for **stereo playback (2 channels)** but TTS audio is **mono (1 channel)**. This causes sample misalignment where every other sample is interpreted as the wrong channel.

### Location
`esp32-luna/components/audio/audio_manager.c` lines 45-56

### Current Code (Broken)
```c
esp_codec_dev_sample_info_t sample_info = {
    .sample_rate = AUDIO_SAMP199LE_RATE,
    .channel = AUDIO_CHANNELS,  // = 2 (stereo) - WRONG FOR PLAYBACK!
    .bits_per_sample = AUDIO_BIT_WIDTH,
};

// Used for BOTH playback and recording
esp_codec_dev_open(s_play_dev, &sample_info);
esp_codec_dev_open(s_record_dev, &sample_info);
```

### Fix
Use separate sample_info for playback (mono) vs recording (stereo):

```c
// For playback - use MONO
esp_codec_dev_sample_info_t play_sample_info = {
    .sample_rate = AUDIO_SAMPLE_RATE,
    .channel = AUDIO_OUTPUT_CHANNELS,  // = 1 (mono)
    .bits_per_sample = AUDIO_BIT_WIDTH,
};

// For recording - use STEREO (for DOA)
esp_codec_dev_sample_info_t record_sample_info = {
    .sample_rate = AUDIO_SAMPLE_RATE,
    .channel = AUDIO_CHANNELS,  // = 2 (stereo)
    .bits_per_sample = AUDIO_BIT_WIDTH,
};

// Open with correct configs
esp_codec_dev_open(s_play_dev, &play_sample_info);
esp_codec_dev_open(s_record_dev, &record_sample_info);
```

---

## Issue 2: ESP32 WebSocket Frequent Disconnects

### Root Cause
ESP32 is sending audio data too fast, causing:
1. Ring buffer overflow on capture side
2. WebSocket write failures (`transport_poll_write` errors)
3. Disconnection triggers demo mode (face emotion cycling)

### Symptoms
```
E (36265) transport_ws: Error transport_poll_write
E (36265) websocket_client: esp_transport_write() returned 0
W (34364) audio_capture: Ring buffer full, dropped 100 chunks
```

### Location
- `esp32-luna/components/audio/audio_capture.c`
- `esp32-luna/components/network/ws_client.c`
- `esp32-luna/main/main.c`

### Fix Options

**Option A: Reduce audio capture rate**
```c
// In audio_capture.c - increase chunk send interval
#define CAPTURE_SEND_INTERVAL_MS  40  // Instead of sending every 20ms
```

**Option B: Increase WebSocket buffer/timeout**
```c
// In ws_client.c - increase buffer and timeout
esp_websocket_client_config_t ws_config = {
    .buffer_size = 8192,  // Increase from default
    .task_stack = 8192,
};
```

**Option C: Stop capture when buffer is full (throttle)**
```c
// In audio_capture.c capture_task()
if (xRingbufferSend(s_ringbuf, output_buffer, output_size, 0) != pdTRUE) {
    // Instead of just logging, actually pause capture briefly
    vTaskDelay(pdMS_TO_TICKS(50));
}
```

---

## Issue 3: TTS Audio Not Always Reaching ESP32

### Root Cause
When TTS audio is ready to send, the ESP32 WebSocket might already be disconnected due to Issue 2. The `session.connected` check fails silently.

### Symptoms
```
🔊 TTS started - speaking to ESP32
🔇 TTS stopped - sent 0 audio chunks to ESP32
```

### Location
`my_bot.py` - `ESP32AudioOutputProcessor.process_frame()`

### Fix
Add debug logging and handle disconnection gracefully:

```python
# In ESP32AudioOutputProcessor.process_frame()
elif isinstance(frame, TTSAudioRawFrame):
    if not self.session.connected:
        print(f"   ⚠️  Cannot send TTS audio - ESP32 disconnected")
        return
    if not self.session.audio_enabled:
        print(f"   ⚠️  Cannot send TTS audio - audio not enabled")
        return

    # ... rest of audio sending code
```

---

## Issue 4: ESP32 Falls Back to Demo Mode

### Root Cause
The WebSocket connection state (`ws_client_is_connected()`) returns false during brief disconnects, causing `main.c` to enter demo mode (emotion cycling).

### Symptoms
```
I (38750) luna_main: Demo: Setting emotion to 'thinking'
I (51761) luna_main: Demo: Setting emotion to 'excited'
```

### Location
`esp32-luna/main/main.c` lines 182-192

### Current Code
```c
// Emotion cycling demo when not connected to server
if (!ws_client_is_connected()) {
    if (++emotion_cycle_counter >= 3) {
        // Cycles through emotions - this happens during brief disconnects!
    }
}
```

### Fix
Add a debounce/grace period before entering demo mode:

```c
static int disconnect_counter = 0;
#define DISCONNECT_GRACE_PERIOD 10  // 10 seconds before demo mode

if (!ws_client_is_connected()) {
    disconnect_counter++;
    if (disconnect_counter >= DISCONNECT_GRACE_PERIOD) {
        // Only enter demo mode after sustained disconnect
        if (++emotion_cycle_counter >= 3) {
            // ... demo mode code
        }
    }
} else {
    disconnect_counter = 0;  // Reset on successful connection
    emotion_cycle_counter = 0;
}
```

---

## Issue 5: Playback Buffer Overflow

### Root Cause
TTS audio arrives faster than ESP32 can play it back. The 32KB buffer fills up and drops audio.

### Symptoms
```
W (307024) audio_playback: Playback buffer full, dropping 1074 bytes
W (307036) audio_playback: Playback buffer full, dropping 4096 bytes
```

### Location
`esp32-luna/components/audio/audio_playback.c`

### Fix Options

**Option A: Increase buffer size**
```c
#define DEFAULT_BUFFER_SIZE  (64 * 1024)  // 64KB instead of 32KB
```

**Option B: Flow control - tell backend to slow down**
This requires protocol changes to send backpressure signals.

**Option C: Drop old audio instead of new** (prioritize freshness)
```c
// In audio_playback_feed() - if buffer full, clear old data first
if (xRingbufferGetCurFreeSize(s_ringbuf) < len) {
    audio_playback_clear();  // Clear old audio
}
xRingbufferSend(s_ringbuf, data, len, pdMS_TO_TICKS(10));
```

---

## Fix Implementation Order

### Priority 1: Critical (Audio doesn't work)
1. **Fix stereo/mono mismatch** in `audio_manager.c` - This is why audio sounds like gibberish

### Priority 2: High (Stability issues)
2. **Add disconnect grace period** in `main.c` - Stop demo mode interference
3. **Increase playback buffer** in `audio_playback.c` - Reduce dropped audio
4. **Add WebSocket reconnect throttling** - Reduce connection churn

### Priority 3: Medium (Debug & Polish)
5. **Add debug logging** for TTS audio delivery in `my_bot.py`
6. **Reduce audio capture rate** if still having issues

---

## Files to Modify

### ESP32 (Frontend)
| File | Changes |
|------|---------|
| `components/audio/audio_manager.c` | Separate mono/stereo configs |
| `components/audio/audio_playback.c` | Increase buffer, handle overflow |
| `components/audio/audio_capture.c` | Throttle on buffer full |
| `main/main.c` | Add disconnect grace period |

### Backend (Python)
| File | Changes |
|------|---------|
| `my_bot.py` | Add debug logging for TTS delivery |

---

## Testing Checklist

After fixes, verify:
- [ ] Audio plays clearly (not gibberish)
- [ ] ESP32 maintains stable WebSocket connection (>30 seconds)
- [ ] No demo mode activation during active conversation
- [ ] TTS audio chunks sent > 0 consistently
- [ ] Ring buffer overflow warnings reduced
- [ ] Full conversation flow works end-to-end

---

## Quick Test Commands

```bash
# Backend
cd /Users/antonioli/Desktop/pipecat
python my_bot.py

# ESP32
cd /Users/antonioli/Desktop/pipecat/esp32-luna
source $HOME/esp/esp-idf/export.sh
idf.py build && idf.py -p /dev/cu.usbmodem1101 flash monitor
```

---

**Created:** January 2026
**Status:** IMPLEMENTED (January 16, 2026)
