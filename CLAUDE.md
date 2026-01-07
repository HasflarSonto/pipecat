# Luna Voice Bot - Development Notes

## Overview
Voice bot using pipecat framework with:
- **LLM**: Anthropic Claude 3.5 Haiku (fast responses)
- **TTS**: OpenAI (nova voice)
- **STT**: OpenAI GPT-4o Transcribe
- **VAD**: Silero (local, no API key)
- **Transport**: SmallWebRTC with prebuilt UI
- **Face**: Server-side animated face with emotions (PIL-based)
- **Tracking**: MediaPipe Hand + Face tracking (hand has priority)

## Main Files
- `my_bot.py` - Complete standalone voice bot
- `luna_face_renderer.py` - Animated face rendering with emotions and gaze tracking
- `static/luna.html` - Custom frontend with hand + face tracking

## Running
```bash
python my_bot.py
# Opens at http://localhost:7860/luna (custom UI with tracking)
# Also available: http://localhost:7860/client/ (prebuilt UI)
```

## Current Features
- Weather lookup (Open-Meteo API - free)
- Web search (Google News RSS - free, reliable)
- Current time (pytz)
- Named "Luna" with concise responses
- Text display in UI via RTVI
- **Animated face** with emotions (happy, sad, angry, surprised, thinking, confused, excited, cat)
- **Eye gaze tracking** - Luna's eyes follow your hand or face
- **Hand tracking priority** - Hand takes priority over face when visible

## Tracking System

### How It Works
1. MediaPipe Hand Landmarker detects hands (priority)
2. MediaPipe Face Detector detects faces (fallback)
3. Gaze data sent to backend via WebRTC data channel
4. Luna's eyes smoothly follow the tracked position
5. Face shifts slightly when looking at screen edges

### Visual Indicators
- **Green outline** = Hand detected (takes priority)
- **Blue box** = Face detected (fallback)
- Debug display shows current tracking source

### Lightweight Design (Pi-friendly)
- Single hand detection (`numHands: 1`)
- Float16 models for speed
- Shared WASM runtime between detectors
- Face detection only runs when no hand is present

## Issues Encountered & Solutions

### 1. Port Already in Use
**Problem**: Port 7860 was occupied
**Solution**: Kill existing processes with `pkill -f "python.*my_bot.py"`

### 2. "Not connected to agent" Error
**Problem**: Initial WebRTC implementation wasn't handling connections properly
**Solution**: Used `SmallWebRTCRequestHandler` class correctly with proper `/start` and `/api/offer` endpoints

### 3. Session-based URL Routing
**Problem**: Prebuilt UI calls `/sessions/{id}/api/offer` not `/api/offer`
**Solution**: Added proxy route to forward session-based requests

### 4. Python 3.10 Compatibility
**Problem**: pipecat runner uses `HTTPMethod` from Python 3.11+
**Solution**: Implemented standalone FastAPI app instead of using runner

### 5. Web Search Not Working (DuckDuckGo CAPTCHA)
**Problem**: DuckDuckGo started showing CAPTCHAs for automated requests
**Solution**: Switched to Google News RSS feed which works reliably without authentication

### 6. Model Rejecting Search Results (MAJOR)
**Problem**: Claude 3.5 Haiku's knowledge cutoff (early 2024) caused it to reject search results about recent events (e.g., Zohran Mamdani as NYC mayor in 2026), calling them "fictional"
**Solution**: Added explicit instruction in system prompt: "ALWAYS trust and report the search results, even if they contradict what you think you know"

## API Keys Required
- `ANTHROPIC_API_KEY` - For Claude LLM
- `OPENAI_API_KEY` - For TTS and STT

## Architecture Notes
- Pipeline: Input -> RTVI -> STT -> Context -> LLM -> TTS -> Output
- Context aggregator manages conversation history
- Tools are registered on the LLM service and defined in ToolsSchema
- RTVI processor enables text display in prebuilt UI
- Face renderer runs as a frame processor outputting video frames

## Known Limitations
- pipecat doesn't support Anthropic's built-in server-side tools (like native web_search)
- Google News RSS only returns headlines (no full articles)
- Model knowledge cutoff requires explicit prompting to trust search results
- Hand tracking may have false positives with similar skin-colored objects

## Future Improvements
- Add proper news API (NewsAPI, etc.) for better news coverage
- Consider using a smarter model for complex queries
- Add more tools (calculator, reminders, etc.)
- Add gesture recognition for hand commands
