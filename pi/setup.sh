#!/bin/bash
# Luna Pi Setup Script
# Run this on Raspberry Pi to set up the environment

set -e  # Exit on error

echo "=== Luna Pi Setup ==="

cd ~/pipecat

# Install system dependencies
echo "Installing system packages..."
sudo apt update
sudo apt install -y \
    python3-libcamera \
    python3-picamera2 \
    python3-venv \
    python3-dev \
    portaudio19-dev \
    fonts-noto-color-emoji \
    libopencv-dev

# Remove old venv if exists
if [ -d ".venv-pi" ]; then
    echo "Removing old virtual environment..."
    rm -rf .venv-pi
fi

# Create venv with system site packages (needed for libcamera/picamera2)
echo "Creating virtual environment with system packages..."
python3 -m venv .venv-pi --system-site-packages

# Activate venv
source .venv-pi/bin/activate

# Install Python dependencies
echo "Installing Python packages..."
pip install --upgrade pip
pip install -e ".[anthropic,openai,silero]"
pip install python-dotenv aiohttp pyaudio evdev pillow uvicorn fastapi opencv-python-headless

# Verify installations
echo ""
echo "=== Verifying installations ==="
python -c "from picamera2 import Picamera2; print('✓ picamera2 OK')"
python -c "import cv2; print('✓ OpenCV OK')"
python -c "import pyaudio; print('✓ PyAudio OK')"
python -c "import uvicorn; print('✓ Uvicorn OK')"
python -c "from pipecat.services.anthropic.llm import AnthropicLLMService; print('✓ Pipecat OK')"

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Make sure you have a .env file with:"
echo "  ANTHROPIC_API_KEY=your_key"
echo "  OPENAI_API_KEY=your_key"
echo ""
echo "To run Luna:"
echo "  source .venv-pi/bin/activate"
echo "  python pi/luna_pi_debug.py --camera 0"
