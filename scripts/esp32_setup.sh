#!/bin/bash

# ESP32-S3 Setup Script for macOS
# This script automates ESP-IDF installation and project setup

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ESP32_PROJECT_DIR="$PROJECT_ROOT/esp32-luna"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}ESP32-S3 Setup Script${NC}"
echo "================================"
echo ""

# Check if running on macOS
if [[ "$OSTYPE" != "darwin"* ]]; then
    echo -e "${RED}Error: This script is designed for macOS${NC}"
    exit 1
fi

# Check for required tools
echo "Checking prerequisites..."
if ! command -v python3 &> /dev/null; then
    echo -e "${RED}Error: python3 is required but not installed${NC}"
    exit 1
fi

if ! command -v git &> /dev/null; then
    echo -e "${RED}Error: git is required but not installed${NC}"
    exit 1
fi

echo -e "${GREEN}✓ Prerequisites met${NC}"
echo ""

# ESP-IDF Installation
ESP_IDF_VERSION="v5.2"
ESP_IDF_DIR="$HOME/esp/esp-idf"

if [ -d "$ESP_IDF_DIR" ]; then
    echo -e "${YELLOW}ESP-IDF already installed at $ESP_IDF_DIR${NC}"
    echo "Checking version..."
    cd "$ESP_IDF_DIR"
    CURRENT_VERSION=$(git describe --tags 2>/dev/null || echo "unknown")
    echo "Current version: $CURRENT_VERSION"
    echo ""
    read -p "Do you want to reinstall/update ESP-IDF? (y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "Updating ESP-IDF..."
        git fetch --tags
        git checkout "$ESP_IDF_VERSION"
        git submodule update --init --recursive
    fi
else
    echo "Installing ESP-IDF $ESP_IDF_VERSION..."
    mkdir -p "$HOME/esp"
    cd "$HOME/esp"
    git clone --recursive --branch "$ESP_IDF_VERSION" https://github.com/espressif/esp-idf.git
    cd "$ESP_IDF_DIR"
    ./install.sh esp32s3
    echo -e "${GREEN}✓ ESP-IDF installed${NC}"
fi

# Source ESP-IDF environment
echo ""
echo "Setting up ESP-IDF environment..."
if [ -f "$ESP_IDF_DIR/export.sh" ]; then
    . "$ESP_IDF_DIR/export.sh"
    echo -e "${GREEN}✓ ESP-IDF environment loaded${NC}"
else
    echo -e "${RED}Error: ESP-IDF export.sh not found${NC}"
    exit 1
fi

# Verify installation
if ! command -v idf.py &> /dev/null; then
    echo -e "${RED}Error: idf.py not found. ESP-IDF installation may have failed.${NC}"
    exit 1
fi

echo -e "${GREEN}✓ ESP-IDF verified (version: $(idf.py --version))${NC}"
echo ""

# Create project structure
echo "Creating project structure..."
mkdir -p "$ESP32_PROJECT_DIR"/{main,components/{display,audio,webrtc_client,luna_face}}

# Create main CMakeLists.txt
cat > "$ESP32_PROJECT_DIR/CMakeLists.txt" << 'EOF'
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-luna)
EOF

# Create main/main.c template
cat > "$ESP32_PROJECT_DIR/main/main.c" << 'EOF'
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 Luna Bot Starting...");
    
    // TODO: Initialize display
    // TODO: Initialize audio codec
    // TODO: Initialize WebRTC client
    // TODO: Connect to Pipecat server
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Running...");
    }
}
EOF

# Create main/CMakeLists.txt
cat > "$ESP32_PROJECT_DIR/main/CMakeLists.txt" << 'EOF'
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
)
EOF

# Create sdkconfig.defaults for ESP32-S3
cat > "$ESP32_PROJECT_DIR/sdkconfig.defaults" << 'EOF'
# ESP32-S3 Configuration
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y

# Enable PSRAM
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y

# WiFi
CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=10
CONFIG_ESP32_WIFI_DYNAMIC_RX_BUFFER_NUM=32

# FreeRTOS
CONFIG_FREERTOS_HZ=1000

# Logging
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y
EOF

# Create .vscode settings for Cursor/VS Code
mkdir -p "$ESP32_PROJECT_DIR/.vscode"

cat > "$ESP32_PROJECT_DIR/.vscode/settings.json" << EOF
{
    "idf.adapterTargetName": "esp32s3",
    "idf.customExtraPaths": "$HOME/esp/esp-idf/tools:$HOME/.espressif/tools/xtensa-esp32s3-elf/esp-2022r1-11.2.0/xtensa-esp32s3-elf/bin:$HOME/.espressif/tools/esp32ulp-elf/2.38.51-esp-20220505/esp32ulp-elf-binutils/bin:$HOME/.espressif/tools/openocd-esp32/v0.12.0-esp32-20230419/openocd-esp32/bin:$HOME/.espressif/python_env/idf5.2_py3.11_env/bin",
    "idf.customExtraVars": {
        "OPENOCD_SCRIPTS": "$HOME/.espressif/tools/openocd-esp32/v0.12.0-esp32-20230419/openocd-esp32/share/openocd/scripts",
        "IDF_CCACHE_ENABLE": "1"
    },
    "idf.espIdfPath": "$ESP_IDF_DIR",
    "idf.openOcdConfigs": [
        "board/esp32s3-builtin.cfg"
    ],
    "idf.port": "/dev/cu.usbserial-*",
    "idf.toolsPath": "$HOME/.espressif",
    "idf.gitPath": "git",
    "files.associations": {
        "*.h": "c",
        "*.c": "c"
    },
    "C_Cpp.intelliSenseEngine": "default"
}
EOF

# Create tasks.json for build/flash/monitor
cat > "$ESP32_PROJECT_DIR/.vscode/tasks.json" << 'EOF'
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "Build",
            "type": "shell",
            "command": "idf.py build",
            "problemMatcher": [
                "$gcc"
            ],
            "group": {
                "kind": "build",
                "isDefault": true
            }
        },
        {
            "label": "Flash",
            "type": "shell",
            "command": "idf.py flash",
            "dependsOn": "Build",
            "problemMatcher": []
        },
        {
            "label": "Monitor",
            "type": "shell",
            "command": "idf.py monitor",
            "problemMatcher": []
        },
        {
            "label": "Build, Flash and Monitor",
            "dependsOrder": "sequence",
            "dependsOn": [
                "Build",
                "Flash",
                "Monitor"
            ],
            "problemMatcher": []
        },
        {
            "label": "Full Clean",
            "type": "shell",
            "command": "idf.py fullclean",
            "problemMatcher": []
        },
        {
            "label": "Menuconfig",
            "type": "shell",
            "command": "idf.py menuconfig",
            "problemMatcher": []
        }
    ]
}
EOF

# Create launch.json for debugging
cat > "$ESP32_PROJECT_DIR/.vscode/launch.json" << 'EOF'
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "ESP32-S3 Debug",
            "type": "cppdbg",
            "request": "launch",
            "MIMode": "gdb",
            "miDebuggerPath": "${config:idf.customExtraPaths}/xtensa-esp32s3-elf-gdb",
            "program": "${workspaceFolder}/build/esp32-luna.elf",
            "cwd": "${workspaceFolder}",
            "environment": [
                {
                    "name": "PATH",
                    "value": "${config:idf.customExtraPaths}"
                }
            ],
            "setupCommands": [
                {
                    "text": "target remote :3333"
                },
                {
                    "text": "set remote hardware-watchpoint-limit 2"
                },
                {
                    "text": "monitor reset halt"
                },
                {
                    "text": "monitor gdb_sync"
                },
                {
                    "text": "thb app_main"
                },
                {
                    "text": "c"
                }
            ],
            "externalConsole": false,
            "logging": {
                "engineLogging": false
            }
        }
    ]
}
EOF

# Create .gitignore
cat > "$ESP32_PROJECT_DIR/.gitignore" << 'EOF'
build/
sdkconfig
sdkconfig.old
dependencies.lock
managed_components/
*.pyc
__pycache__/
.vscode/settings.json
EOF

# Create README for the ESP32 project
cat > "$ESP32_PROJECT_DIR/README.md" << 'EOF'
# ESP32-S3 Luna Bot

Voice assistant bot running on ESP32-S3, connecting to Pipecat server.

## Setup

1. Install ESP-IDF (if not already done):
   ```bash
   ./scripts/esp32_setup.sh
   ```

2. Source ESP-IDF environment:
   ```bash
   . $HOME/esp/esp-idf/export.sh
   ```

3. Set target:
   ```bash
   idf.py set-target esp32s3
   ```

4. Configure (optional):
   ```bash
   idf.py menuconfig
   ```

5. Build:
   ```bash
   idf.py build
   ```

6. Flash (replace PORT with your device):
   ```bash
   idf.py -p /dev/cu.usbserial-* flash
   ```

7. Monitor:
   ```bash
   idf.py -p /dev/cu.usbserial-* monitor
   ```

## Connecting to Pipecat Server

1. Start your Pipecat server with ESP32 mode:
   ```bash
   cd /path/to/pipecat
   python my_bot.py -t webrtc --esp32 --host YOUR_IP_ADDRESS
   ```

2. Update the WebRTC client in this project with your server's IP address.

## Project Structure

- `main/` - Main application code
- `components/` - Custom components
  - `display/` - AMOLED display driver
  - `audio/` - ES8311 codec driver
  - `webrtc_client/` - WebRTC client for Pipecat
  - `luna_face/` - Face rendering logic
EOF

echo ""
echo -e "${GREEN}✓ Project structure created${NC}"
echo ""

# Create environment setup script
cat > "$ESP32_PROJECT_DIR/setup_env.sh" << EOF
#!/bin/bash
# Source this file to set up ESP-IDF environment
# Usage: source setup_env.sh

export IDF_PATH="$ESP_IDF_DIR"
. "\$IDF_PATH/export.sh"
echo "ESP-IDF environment loaded!"
echo "IDF_PATH: \$IDF_PATH"
EOF

chmod +x "$ESP32_PROJECT_DIR/setup_env.sh"

echo -e "${GREEN}================================${NC}"
echo -e "${GREEN}Setup Complete!${NC}"
echo ""
echo "Next steps:"
echo "1. Navigate to the project:"
echo "   ${YELLOW}cd $ESP32_PROJECT_DIR${NC}"
echo ""
echo "2. Source ESP-IDF environment:"
echo "   ${YELLOW}source setup_env.sh${NC}"
echo "   (or: . \$HOME/esp/esp-idf/export.sh)"
echo ""
echo "3. Set target to ESP32-S3:"
echo "   ${YELLOW}idf.py set-target esp32s3${NC}"
echo ""
echo "4. Build the project:"
echo "   ${YELLOW}idf.py build${NC}"
echo ""
echo "5. In Cursor IDE:"
echo "   - Install the 'Espressif IDF' extension"
echo "   - Open the esp32-luna folder"
echo "   - Use Cmd+Shift+P -> 'ESP-IDF: Set Espressif device target' -> esp32s3"
echo ""
echo -e "${GREEN}Happy coding!${NC}"

