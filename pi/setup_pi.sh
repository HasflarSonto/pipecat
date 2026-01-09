#!/bin/bash
# Luna Pi Setup Script
# Sets up a Raspberry Pi to run Luna voice assistant standalone

set -e

echo "========================================"
echo "  Luna Pi Standalone Setup"
echo "========================================"

# Check if running on Raspberry Pi
if [ ! -f /proc/device-tree/model ]; then
    echo "Warning: This doesn't appear to be a Raspberry Pi"
    echo "Continuing anyway..."
else
    echo "Detected: $(cat /proc/device-tree/model)"
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
    libffi-dev \
    libssl-dev \
    pkg-config

# Create virtual environment
echo ""
echo ">>> Creating Python virtual environment..."
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."
python3 -m venv .venv-pi
source .venv-pi/bin/activate

# Install pipecat from local source
echo ""
echo ">>> Installing pipecat..."
pip install --upgrade pip
pip install -e .

# Install Pi-specific dependencies
echo ""
echo ">>> Installing Pi dependencies..."
pip install -r pi/requirements-pi.txt

# Add user to required groups
echo ""
echo ">>> Adding user to audio and video groups..."
sudo usermod -aG audio,video $USER

# Create .env file template if not exists
if [ ! -f .env ]; then
    echo ""
    echo ">>> Creating .env template..."
    cat > .env << 'ENV_EOF'
# Luna Pi Configuration
# Get your API keys from:
# - Anthropic: https://console.anthropic.com/
# - OpenAI: https://platform.openai.com/api-keys

ANTHROPIC_API_KEY=your_anthropic_key_here
OPENAI_API_KEY=your_openai_key_here
ENV_EOF
    echo "Created .env - EDIT THIS FILE to add your API keys!"
fi

# Create systemd service
echo ""
echo ">>> Creating systemd service..."
WORK_DIR="$(pwd)"
USER_NAME="$(whoami)"

sudo tee /etc/systemd/system/luna.service > /dev/null << SERVICE_EOF
[Unit]
Description=Luna Voice Assistant
After=network.target sound.target

[Service]
Type=simple
User=$USER_NAME
WorkingDirectory=$WORK_DIR
ExecStart=$WORK_DIR/.venv-pi/bin/python pi/luna_pi_standalone.py
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
SERVICE_EOF

sudo systemctl daemon-reload

# Done
echo ""
echo "========================================"
echo "  Setup Complete!"
echo "========================================"
echo ""
echo "NEXT STEPS:"
echo ""
echo "1. ADD YOUR API KEYS:"
echo "   nano .env"
echo "   # Add your ANTHROPIC_API_KEY and OPENAI_API_KEY"
echo ""
echo "2. CHECK YOUR AUDIO DEVICES:"
echo "   arecord -l    # List microphones"
echo "   aplay -l      # List speakers"
echo ""
echo "3. TEST AUDIO:"
echo "   # Test speaker"
echo "   speaker-test -t wav -c 1"
echo "   # Test mic (records 3 sec, plays back)"
echo "   arecord -d 3 test.wav && aplay test.wav"
echo ""
echo "4. RUN LUNA:"
echo "   source .venv-pi/bin/activate"
echo "   python pi/luna_pi_standalone.py"
echo ""
echo "5. (OPTIONAL) RUN ON BOOT:"
echo "   sudo systemctl enable luna"
echo "   sudo systemctl start luna"
echo "   # View logs: journalctl -u luna -f"
echo ""
echo "Display should already be configured per Readmetouchdisplayscreen.md"
echo ""
