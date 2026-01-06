# Luna Voice Bot - Development Notes

## Overview
Voice bot using pipecat framework with:
- **LLM**: Anthropic Claude 3.5 Haiku (fast responses)
- **TTS**: OpenAI (nova voice)
- **STT**: OpenAI GPT-4o Transcribe
- **VAD**: Silero (local, no API key)
- **Transport**: SmallWebRTC with prebuilt UI

## Main File
`my_bot.py` - Complete standalone voice bot

## Running
```bash
python my_bot.py
# Opens at http://localhost:7860
```

## Current Features
- Weather lookup (Open-Meteo API - free)
- Web search (DuckDuckGo HTML scraping - free)
- Current time (pytz)
- Named "Luna" with concise responses
- Text display in UI via RTVI

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

### 5. Web Search Not Working
**Problem**: DuckDuckGo instant answer API returns empty for news queries
**Solution**: Switched to HTML scraping of DuckDuckGo search results

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

## Known Limitations
- pipecat doesn't support Anthropic's built-in server-side tools (like native web_search)
- DuckDuckGo scraping may be rate-limited or blocked
- Model knowledge cutoff requires explicit prompting to trust search results

## Future Improvements
- Add proper news API (NewsAPI, etc.) for better news coverage
- Consider using a smarter model for complex queries
- Add more tools (calculator, reminders, etc.)
- Build custom frontend for better UX
