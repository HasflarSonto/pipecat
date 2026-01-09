# Luna Pi Client

Headless Raspberry Pi client for Luna voice assistant. Renders Luna's animated face to an ILI9341 display, captures audio from USB microphone, and plays responses through speaker.

## Hardware Requirements

- **Raspberry Pi** (tested on Pi 4, should work on Pi 3B+)
- **ILI9341 2.4" SPI Display** (240x320, connected via SPI)
- **USB Microphone** (any USB mic should work)
- **Speaker** (3.5mm audio jack or USB speaker)
- **Optional**: Pi Camera Module for vision features

## Quick Start

### 1. Clone the Repository

```bash
cd ~
git clone https://github.com/YOUR_REPO/pipecat.git
cd pipecat
git checkout pi-integration
```

### 2. Run Setup Script

```bash
chmod +x pi/setup_pi.sh
./pi/setup_pi.sh
```

### 3. Configure Display

Add to `/boot/firmware/config.txt`:

```ini
# Enable SPI
dtparam=spi=on

# ILI9341 display on SPI0
dtoverlay=fbtft,spi0-0,ili9341,width=240,height=320,speed=32000000,rotate=90,fps=30,bgr=1

# Optional: Disable HDMI to save power
# hdmi_blanking=2
```

Then reboot:
```bash
sudo reboot
```

### 4. Configure Audio

List available audio devices:
```bash
arecord -l  # List microphones
aplay -l    # List speakers
```

Edit `~/.asoundrc` to match your devices:
```
pcm.!default {
    type asym
    playback.pcm {
        type plug
        slave.pcm "hw:0,0"  # Change to your speaker device
    }
    capture.pcm {
        type plug
        slave.pcm "hw:1,0"  # Change to your mic device
    }
}
```

Test audio:
```bash
# Test speaker
speaker-test -t wav -c 1

# Test microphone (records 5 seconds, plays back)
arecord -d 5 test.wav && aplay test.wav
```

### 5. Run Luna Client

First, start the Luna server on your main computer:
```bash
# On your server (Mac/PC)
python my_bot.py
```

Then run the Pi client:
```bash
# On Pi
source .venv-pi/bin/activate
python pi/luna_pi_client.py --server http://YOUR_SERVER_IP:7860
```

## Command Line Options

```
python pi/luna_pi_client.py [OPTIONS]

Options:
  -s, --server URL     Luna server URL (default: http://localhost:7860)
  -d, --display DEV    Framebuffer device (default: /dev/fb0)
  -v, --verbose        Enable debug logging
```

## Running as a Service

To start Luna automatically on boot:

1. Edit the service file:
```bash
sudo nano /etc/systemd/system/luna.service
```

2. Set your server IP in the `ExecStart` line

3. Enable and start the service:
```bash
sudo systemctl daemon-reload
sudo systemctl enable luna
sudo systemctl start luna
```

4. View logs:
```bash
journalctl -u luna -f
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Server (Mac/PC)                                            │
│  ┌─────────────────────────────────────────────────────┐    │
│  │ my_bot.py                                           │    │
│  │  ├── LLM (Anthropic Claude)                         │    │
│  │  ├── STT (OpenAI Whisper)                           │    │
│  │  ├── TTS (OpenAI)                                   │    │
│  │  └── LunaFaceRenderer → Video frames                │    │
│  └─────────────────────────────────────────────────────┘    │
│                          │                                   │
│                     WebRTC                                   │
│                          │                                   │
└──────────────────────────┼──────────────────────────────────┘
                           │
                     ┌─────┴─────┐
                     │  Network  │
                     └─────┬─────┘
                           │
┌──────────────────────────┼──────────────────────────────────┐
│  Raspberry Pi            │                                   │
│  ┌───────────────────────┴─────────────────────────────┐    │
│  │ luna_pi_client.py                                   │    │
│  │  ├── WebRTC Connection (aiortc)                     │    │
│  │  ├── Video → Framebuffer (/dev/fb0) → ILI9341      │    │
│  │  ├── Audio In ← USB Microphone                      │    │
│  │  └── Audio Out → Speaker                            │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

## Troubleshooting

### Display not working

1. Check framebuffer exists: `ls -la /dev/fb*`
2. Test with: `cat /dev/urandom > /dev/fb0` (shows static)
3. Check dmesg for SPI errors: `dmesg | grep -i spi`
4. Verify config.txt settings and reboot

### No audio input

1. Check USB mic is detected: `lsusb`
2. Check ALSA devices: `arecord -l`
3. Test recording: `arecord -d 3 test.wav`
4. Adjust ~/.asoundrc device numbers

### No audio output

1. Check speaker device: `aplay -l`
2. Test: `speaker-test -t wav`
3. Check volume: `alsamixer`

### Connection issues

1. Check server is running: `curl http://SERVER:7860/`
2. Check firewall allows port 7860
3. Ensure Pi and server are on same network
4. Try with verbose logging: `--verbose`

### WebRTC connection fails

1. Check STUN server accessibility
2. If behind strict NAT, may need TURN server
3. Check both devices can reach each other directly

## Performance Notes

- Video is 240x320 @ 15 FPS (tuned for Pi performance)
- Audio is 48kHz mono
- CPU usage on Pi 4: ~20-30%
- SPI display speed: 32MHz (can try 40MHz on Pi 4)

## Display Alternatives

If not using ILI9341, the client supports any Linux framebuffer device. For HDMI displays, the framebuffer is usually `/dev/fb0` by default.

For other SPI displays, adjust the `dtoverlay` line in config.txt for your specific display driver.
