#!/bin/bash
# Luna Pi Setup Script
# Sets up a Raspberry Pi to run Luna voice assistant client

set -e

echo "========================================"
echo "  Luna Pi Setup"
echo "========================================"

# Check if running on Raspberry Pi
if [ ! -f /proc/device-tree/model ]; then
    echo "Warning: This doesn't appear to be a Raspberry Pi"
    echo "Continuing anyway..."
fi

# Update system
echo ""
echo ">>> Updating system packages..."
sudo apt-get update
sudo apt-get upgrade -y

# Install system dependencies
echo ""
echo ">>> Installing system dependencies..."
sudo apt-get install -y \
    python3-pip \
    python3-venv \
    python3-dev \
    libportaudio2 \
    libportaudiocpp0 \
    portaudio19-dev \
    libasound2-dev \
    libopus-dev \
    libvpx-dev \
    libsrtp2-dev \
    libavformat-dev \
    libavcodec-dev \
    libavdevice-dev \
    libavutil-dev \
    libswscale-dev \
    libffi-dev \
    libssl-dev \
    pkg-config \
    ffmpeg

# Create virtual environment
echo ""
echo ">>> Creating Python virtual environment..."
cd "$(dirname "$0")/.."
python3 -m venv .venv-pi
source .venv-pi/bin/activate

# Install Python dependencies
echo ""
echo ">>> Installing Python packages..."
pip install --upgrade pip
pip install -r pi/requirements-pi.txt

# Configure audio
echo ""
echo ">>> Configuring audio..."

# Add user to audio group
sudo usermod -aG audio $USER

# Create ALSA configuration for USB mic if not exists
if [ ! -f ~/.asoundrc ]; then
    echo "Creating ~/.asoundrc..."
    cat > ~/.asoundrc << 'ALSA_EOF'
# Default to USB microphone for input, built-in audio for output
pcm.!default {
    type asym
    playback.pcm {
        type plug
        slave.pcm "hw:0,0"
    }
    capture.pcm {
        type plug
        slave.pcm "hw:1,0"
    }
}

ctl.!default {
    type hw
    card 0
}
ALSA_EOF
    echo "Created ~/.asoundrc"
    echo "Note: You may need to adjust hw:X,X based on your audio devices"
    echo "Run 'arecord -l' to list recording devices"
    echo "Run 'aplay -l' to list playback devices"
fi

# Configure display
echo ""
echo ">>> Display setup..."
echo "Make sure your ILI9341 display is configured in /boot/firmware/config.txt"
echo "See pi/README.md for display configuration details"

# Create systemd service (optional)
echo ""
echo ">>> Creating systemd service (optional)..."
sudo tee /etc/systemd/system/luna.service > /dev/null << 'SERVICE_EOF'
[Unit]
Description=Luna Voice Assistant Client
After=network.target sound.target

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/pipecat
Environment=DISPLAY=:0
ExecStart=/home/pi/pipecat/.venv-pi/bin/python pi/luna_pi_client.py --server http://YOUR_SERVER_IP:7860
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
SERVICE_EOF

echo "Systemd service created at /etc/systemd/system/luna.service"
echo "Edit it to set your server IP, then:"
echo "  sudo systemctl daemon-reload"
echo "  sudo systemctl enable luna"
echo "  sudo systemctl start luna"

# Done
echo ""
echo "========================================"
echo "  Setup Complete!"
echo "========================================"
echo ""
echo "Next steps:"
echo "1. Configure your display in /boot/firmware/config.txt"
echo "2. Check audio devices: arecord -l && aplay -l"
echo "3. Update ~/.asoundrc if needed"
echo "4. Test with: source .venv-pi/bin/activate && python pi/luna_pi_client.py --server http://YOUR_SERVER:7860"
echo ""
echo "For display setup, see Readmetouchdisplayscreen.md"
echo ""
