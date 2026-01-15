# WebRTC Client Component for Pipecat

This component implements a WebRTC client that connects to your Pipecat server running on your computer.

## Architecture

```
ESP32-S3                    Your Computer
─────────────────          ──────────────────
WebRTC Client ────WiFi────→ Pipecat Server
     │                          │
     ├─ Audio Input ────────────┤
     │   (Microphone)           │
     │                          │
     ├─ Audio Output ←──────────┤
     │   (Speaker)              │
     │                          │
     └─ Display Updates ←───────┤
        (Luna Face)             │
```

## Server Setup

Start your Pipecat server with ESP32 mode enabled:

```bash
cd /Users/antonioli/Desktop/pipecat
python my_bot.py -t webrtc --esp32 --host YOUR_IP_ADDRESS
```

Replace `YOUR_IP_ADDRESS` with your Mac's IP address (e.g., `192.168.1.100`).

Find your IP:
```bash
ifconfig | grep "inet " | grep -v 127.0.0.1
```

## Implementation Notes

### SDP Munging

Pipecat already supports ESP32 with the `--esp32` flag, which enables SDP munging. This means:
- The server will automatically adjust SDP offers/answers for ESP32 compatibility
- You don't need to modify SDP on the ESP32 side

### WebRTC Libraries for ESP32

Options:
1. **ESP-IDF WebRTC** (if available in your IDF version)
2. **Custom implementation** using ESP-IDF's HTTP/WebSocket + RTP
3. **Third-party libraries** (check ESP32 community)

### Audio Format

- Sample rate: 16kHz (Pipecat standard)
- Format: PCM 16-bit mono
- Codec: PCM (no compression needed for WebRTC)

### Connection Flow

1. Connect to WiFi
2. Establish WebSocket connection to Pipecat server
3. Exchange SDP offer/answer
4. Exchange ICE candidates
5. Establish peer connection
6. Start audio streams:
   - Capture from ES8311 microphone
   - Play to ES8311 speaker
7. Handle display updates via data channel

## TODO

- [ ] Implement WebRTC client using ESP-IDF libraries
- [ ] Handle SDP offer/answer exchange
- [ ] Implement ICE candidate exchange
- [ ] Audio capture from ES8311
- [ ] Audio playback to ES8311
- [ ] Data channel for display updates
- [ ] Error handling and reconnection logic

## References

- Pipecat WebRTC transport: `src/pipecat/transports/smallwebrtc/`
- ESP-IDF WebRTC examples (if available)
- ESP32 WebRTC community projects

