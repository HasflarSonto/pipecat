# ESP32-IDF Branch

This branch contains all the ESP32-S3 ESP-IDF migration work for running Pipecat on embedded hardware.

## What's Included

### Documentation
- **ESP32_COMPLETE_GUIDE.md** - Comprehensive combined guide (start here!)
- **ESP32_MIGRATION_GUIDE.md** - Migration strategy and answers to common questions
- **ESP32_SETUP_INSTRUCTIONS.md** - Detailed step-by-step setup
- **ESP32_QUICK_START.md** - Quick reference guide
- **ESP32S3Display.md** - Board documentation

### Setup & Scripts
- **scripts/esp32_setup.sh** - Automated ESP-IDF installation and project setup

### Project Structure
- **esp32-luna/** - Main project for Pipecat on ESP32
  - WebRTC client component skeleton
  - Display, audio, and face rendering component structures
  - Cursor IDE configuration

### Working Examples
- **ESP-IDF-v5.4.2/05_Spec_Analyzer/** - Working audio spectrum analyzer
  - Demonstrates audio capture (ES8311)
  - FFT processing
  - AMOLED display rendering
  - Successfully built and tested!

## Quick Start

1. Read `ESP32_COMPLETE_GUIDE.md` for the full guide
2. Run `./scripts/esp32_setup.sh` to set up ESP-IDF
3. Build and flash the Spec_Analyzer example to verify setup
4. Start implementing Pipecat integration in `esp32-luna/`

## Status

✅ ESP-IDF v5.4.2 installed and configured  
✅ Example project built and flashed successfully  
✅ Cursor IDE configuration ready  
✅ Project structure created  
🔄 Pipecat integration in progress

## Next Steps

1. Study the Spec_Analyzer example for audio/display patterns
2. Implement WebRTC client connection to Pipecat server
3. Port Luna face rendering logic
4. Optimize for ESP32-S3 memory constraints

