#
# Voice bot using Anthropic LLM + OpenAI TTS/STT
# With Small WebRTC transport and tool calling (web search, weather)
#

import argparse
import asyncio
import base64
import json
import os
import struct
import uuid
from collections import deque
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any, Dict, List, Optional, TypedDict, Union

import aiohttp
import uvicorn
from google.protobuf import descriptor_pb2
import time as time_module
from dotenv import load_dotenv
from fastapi import BackgroundTasks, FastAPI, Header, HTTPException, Request, Response, WebSocket, WebSocketDisconnect
from pydantic import BaseModel
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import RedirectResponse
from fastapi.staticfiles import StaticFiles
from loguru import logger
from pipecat_ai_small_webrtc_prebuilt.frontend import SmallWebRTCPrebuiltUI

from pipecat.adapters.schemas.function_schema import FunctionSchema
from pipecat.adapters.schemas.tools_schema import ToolsSchema
from pipecat.audio.vad.silero import SileroVADAnalyzer
from pipecat.audio.vad.vad_analyzer import VADParams
from pipecat.frames.frames import (
    LLMRunFrame, TranscriptionFrame, Frame, StartFrame, EndFrame, SystemFrame,
    AudioRawFrame, InputAudioRawFrame, TTSStartedFrame, TTSStoppedFrame, TTSAudioRawFrame,
    UserStartedSpeakingFrame, UserStoppedSpeakingFrame
)
from pipecat.processors.frame_processor import FrameProcessor, FrameDirection
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.runner import PipelineRunner
from pipecat.pipeline.task import PipelineParams, PipelineTask
from pipecat.processors.aggregators.llm_context import LLMContext
from pipecat.processors.aggregators.llm_response_universal import LLMContextAggregatorPair
from pipecat.processors.frameworks.rtvi import (
    RTVIConfig,
    RTVIObserver,
    RTVIProcessor,
    RTVIServerMessageFrame,
)
from pipecat.services.anthropic.llm import AnthropicLLMService
from pipecat.services.llm_service import FunctionCallParams
from pipecat.services.openai.stt import OpenAISTTService
from pipecat.services.openai.tts import OpenAITTSService
from pipecat.transports.base_transport import TransportParams
from pipecat.transports.smallwebrtc.connection import SmallWebRTCConnection
from pipecat.transports.smallwebrtc.request_handler import (
    IceCandidate,
    SmallWebRTCPatchRequest,
    SmallWebRTCRequest,
    SmallWebRTCRequestHandler,
)
from pipecat.transports.smallwebrtc.transport import SmallWebRTCTransport

from luna_face_renderer import LunaFaceRenderer
from calendar_integration import (
    get_upcoming_events, get_todays_events, get_next_event,
    format_events_for_voice, is_configured as calendar_is_configured
)

load_dotenv(override=True)

# Initialize the request handler
small_webrtc_handler = SmallWebRTCRequestHandler()


# ============== NOTIFICATION SYSTEM ==============

# API key for iOS app authentication
LUNA_NOTIFY_API_KEY = os.environ.get("LUNA_NOTIFY_API_KEY", "dev-key-change-me")


@dataclass
class Notification:
    """A notification received from the iOS app."""
    id: str
    app_id: str
    app_name: str
    title: str
    timestamp: datetime
    body: Optional[str] = None
    subtitle: Optional[str] = None
    priority: str = "normal"
    category: Optional[str] = None
    thread_id: Optional[str] = None


class NotificationQueue:
    """Thread-safe notification queue with subscriber support."""

    def __init__(self, max_size: int = 50):
        self._queue: deque[Notification] = deque(maxlen=max_size)
        self._listeners: List[asyncio.Queue] = []
        self._lock = asyncio.Lock()

    async def add(self, notification: Notification):
        """Add a notification and notify all listeners."""
        async with self._lock:
            self._queue.append(notification)
            # Notify all listeners
            for listener in self._listeners:
                try:
                    await listener.put(notification)
                except Exception as e:
                    logger.error(f"Failed to notify listener: {e}")

    def dismiss(self, notification_id: str):
        """Remove a notification by ID."""
        self._queue = deque(
            [n for n in self._queue if n.id != notification_id],
            maxlen=self._queue.maxlen
        )

    def clear_all(self):
        """Clear all notifications."""
        self._queue.clear()

    def get_all(self) -> List[Notification]:
        """Get all notifications in the queue."""
        return list(self._queue)

    def get_count(self) -> int:
        """Get the number of notifications."""
        return len(self._queue)

    def subscribe(self) -> asyncio.Queue:
        """Subscribe to new notifications."""
        q: asyncio.Queue = asyncio.Queue()
        self._listeners.append(q)
        return q

    def unsubscribe(self, q: asyncio.Queue):
        """Unsubscribe from notifications."""
        if q in self._listeners:
            self._listeners.remove(q)


# Global notification queue
notification_queue = NotificationQueue()


# App icon mapping (bundle ID to icon name)
APP_ICONS = {
    "com.apple.mobilemail": "mail",
    "com.apple.MobileSMS": "message",
    "com.apple.mobilecal": "calendar",
    "com.slack.Slack": "slack",
    "com.atebits.Tweetie2": "twitter",
    "com.burbn.instagram": "instagram",
    "com.facebook.Messenger": "messenger",
    "com.spotify.client": "spotify",
    "com.google.Gmail": "gmail",
    "com.microsoft.Office.Outlook": "outlook",
}


def get_app_icon(app_id: str) -> str:
    """Map app bundle ID to icon name for display."""
    return APP_ICONS.get(app_id, "app")


# Pydantic models for API requests
class NotificationRequest(BaseModel):
    """Request body for POST /api/notify."""
    id: str
    app_id: str
    app_name: str
    title: str
    timestamp: str
    body: Optional[str] = None
    subtitle: Optional[str] = None
    priority: str = "normal"
    category: Optional[str] = None
    thread_id: Optional[str] = None


class ControlRequest(BaseModel):
    """Request body for POST /api/control."""
    cmd: str
    mode: Optional[str] = None
    emotion: Optional[str] = None
    minutes: Optional[int] = None
    temp: Optional[str] = None
    icon: Optional[str] = None
    desc: Optional[str] = None
    events: Optional[List[dict]] = None


# ============== WAKE WORD FILTER ==============

class WakeWordFilter(FrameProcessor):
    """
    Filters transcriptions to only pass through when wake word is detected.
    This saves LLM tokens by not processing speech that isn't directed at Luna.

    Modes:
    - idle: Waiting for wake word "Luna" - drops all transcriptions without it
    - active: Passes all transcriptions through (stays active for a timeout period)
    """

    def __init__(self, wake_word: str = "luna", active_timeout: float = 30.0, face_renderer=None):
        super().__init__()
        self.wake_word = wake_word.lower()
        self.active_timeout = active_timeout  # How long to stay active after wake word
        self.face_renderer = face_renderer
        self._is_active = True  # Start active for initial greeting
        self._last_activity = 0.0
        self._idle_mode_enabled = False  # Can be toggled via frontend

    def enable_idle_mode(self, enabled: bool = True):
        """Enable or disable idle mode. When disabled, all speech goes through."""
        self._idle_mode_enabled = enabled
        if not enabled:
            self._is_active = True
        logger.info(f"Idle mode {'enabled' if enabled else 'disabled'}")

    def _check_wake_word(self, text: str) -> bool:
        """Check if the wake word is in the text."""
        return self.wake_word in text.lower()

    def _update_active_state(self):
        """Check if we should go back to idle based on timeout."""
        import time
        if self._is_active and self._idle_mode_enabled:
            if time.time() - self._last_activity > self.active_timeout:
                self._is_active = False
                logger.info("Going back to idle mode (timeout)")
                # Show neutral/sleeping face when going idle
                if self.face_renderer:
                    self.face_renderer.set_emotion("neutral")

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        import time

        # ALWAYS pass system frames through immediately - critical for pipeline startup
        if isinstance(frame, (StartFrame, EndFrame, SystemFrame)):
            await self.push_frame(frame, direction)
            return

        # Only filter TranscriptionFrames - pass everything else through
        if not isinstance(frame, TranscriptionFrame):
            await self.push_frame(frame, direction)
            return

        # If idle mode is disabled, pass everything through
        if not self._idle_mode_enabled:
            await self.push_frame(frame, direction)
            return

        text = frame.text if hasattr(frame, 'text') else ""

        # Check timeout
        self._update_active_state()

        # If we're active, pass the frame through
        if self._is_active:
            self._last_activity = time.time()
            await self.push_frame(frame, direction)
            return

        # We're in idle mode - check for wake word
        if self._check_wake_word(text):
            logger.info(f"Wake word detected! Activating. Text: '{text}'")
            self._is_active = True
            self._last_activity = time.time()

            # Show happy face when waking up
            if self.face_renderer:
                self.face_renderer.set_emotion("happy")

            # Pass the frame through
            await self.push_frame(frame, direction)
        else:
            # Drop the frame - we're idle and no wake word
            logger.debug(f"Idle mode - ignoring: '{text[:50]}...' (no wake word)")


# Global wake word filter reference
wake_word_filter = None


# ============== ESP32 WEBSOCKET SUPPORT ==============

class ESP32Session:
    """Manages an ESP32 WebSocket connection."""

    def __init__(self, websocket: WebSocket):
        self.websocket = websocket
        self.connected = False
        self.audio_enabled = False
        self._send_lock = asyncio.Lock()

    async def send_json(self, data: dict):
        """Send a JSON command to the ESP32."""
        if not self.connected:
            return
        async with self._send_lock:
            try:
                await self.websocket.send_text(json.dumps(data))
            except Exception as e:
                logger.error(f"ESP32: Failed to send JSON: {e}")

    async def send_audio(self, data: bytes):
        """Send audio data (16kHz 16-bit PCM mono) to ESP32."""
        if not self.connected or not self.audio_enabled:
            return
        async with self._send_lock:
            try:
                await self.websocket.send_bytes(data)
            except Exception as e:
                logger.error(f"ESP32: Failed to send audio: {e}")

    async def send_emotion(self, emotion: str):
        """Send emotion command."""
        await self.send_json({"cmd": "emotion", "value": emotion})

    async def send_gaze(self, x: float, y: float):
        """Send gaze command."""
        await self.send_json({"cmd": "gaze", "x": x, "y": y})

    async def send_text(self, content: str, size: str = "medium",
                        color: str = "#FFFFFF", bg: str = "#1E1E28"):
        """Send text display command."""
        await self.send_json({
            "cmd": "text",
            "content": content,
            "size": size,
            "color": color,
            "bg": bg
        })

    async def send_text_clear(self):
        """Clear text display."""
        await self.send_json({"cmd": "text_clear"})

    async def send_pixel_art(self, pixels: list, bg: str = "#1E1E28"):
        """Send pixel art command."""
        await self.send_json({
            "cmd": "pixel_art",
            "pixels": pixels,
            "bg": bg
        })

    async def send_pixel_art_clear(self):
        """Clear pixel art."""
        await self.send_json({"cmd": "pixel_art_clear"})

    async def send_audio_start(self):
        """Signal start of TTS audio."""
        self.audio_enabled = True
        await self.send_json({"cmd": "audio_start"})

    async def send_audio_stop(self):
        """Signal end of TTS audio."""
        await self.send_json({"cmd": "audio_stop"})
        self.audio_enabled = False

    async def send_subway(self, line: str, line_color: str, station: str,
                          direction: str, times: list):
        """Send subway arrival times to ESP32."""
        await self.send_json({
            "cmd": "subway",
            "line": line,
            "color": line_color,
            "station": station,
            "direction": direction,
            "times": times
        })

    async def send_notification(self, notification: Notification):
        """Send a notification to the ESP32 for display."""
        await self.send_json({
            "cmd": "notification",
            "id": notification.id,
            "app": notification.app_name,
            "app_icon": get_app_icon(notification.app_id),
            "title": notification.title,
            "body": notification.body or "",
            "timestamp": notification.timestamp.strftime("%I:%M %p"),
            "priority": notification.priority
        })

    async def send_notification_dismiss(self, notification_id: str):
        """Dismiss a notification on the ESP32."""
        await self.send_json({
            "cmd": "notification_dismiss",
            "id": notification_id
        })

    async def send_mode(self, mode: str):
        """Switch display mode on ESP32."""
        await self.send_json({
            "cmd": "mode",
            "mode": mode
        })

    async def send_timer_start(self):
        """Start the timer on ESP32."""
        await self.send_json({"cmd": "timer_start"})

    async def send_timer_pause(self):
        """Pause the timer on ESP32."""
        await self.send_json({"cmd": "timer_pause"})

    async def send_timer_reset(self, minutes: int):
        """Reset the timer on ESP32."""
        await self.send_json({
            "cmd": "timer_reset",
            "minutes": minutes
        })

    async def send_weather(self, temp: str, icon: str, desc: str):
        """Send weather data to ESP32."""
        await self.send_json({
            "cmd": "weather",
            "temp": temp,
            "icon": icon,
            "desc": desc
        })

    async def send_calendar(self, events: List[dict]):
        """Send calendar events to ESP32."""
        await self.send_json({
            "cmd": "calendar",
            "events": events
        })

    async def send_clock(self):
        """Tell ESP32 to show clock with current time."""
        from datetime import datetime
        now = datetime.now()
        await self.send_json({
            "cmd": "clock",
            "hours": now.hour,
            "minutes": now.minute,
            "date": now.strftime("%a %b %d").upper()
        })


# Active ESP32 sessions
esp32_sessions: Dict[str, ESP32Session] = {}
current_esp32_session: Optional[ESP32Session] = None


# ============== ESP32 TRANSPORT PROCESSORS ==============

from pipecat.frames.frames import VADUserStartedSpeakingFrame, VADUserStoppedSpeakingFrame
from pipecat.audio.vad.vad_analyzer import VADAnalyzer, VADState


class ESP32AudioInputProcessor(FrameProcessor):
    """
    Processor that takes audio from ESP32 WebSocket and emits AudioRawFrames.
    This bridges the WebSocket audio input to the pipecat pipeline.
    Includes VAD for voice activity detection.
    """

    def __init__(self, session: "ESP32Session", sample_rate: int = 16000, vad_analyzer: Optional[VADAnalyzer] = None):
        super().__init__()
        self.session = session
        self.sample_rate = sample_rate
        self.vad_analyzer = vad_analyzer
        self._running = False
        self._task: Optional[asyncio.Task] = None
        self._user_speaking = False

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        # Let base class handle the frame first (this marks us as started)
        await super().process_frame(frame, direction)

        # Start the audio reader task when we receive StartFrame
        if isinstance(frame, StartFrame):
            self._running = True
            # Configure VAD with sample rate
            if self.vad_analyzer:
                self.vad_analyzer.set_sample_rate(self.sample_rate)
            # CRITICAL: Push StartFrame downstream so other processors initialize
            await self.push_frame(frame, direction)
            print("   ✅ StartFrame pushed to pipeline")
            # Small delay to ensure pipeline is fully initialized
            await asyncio.sleep(0.2)
            self._task = asyncio.create_task(self._audio_reader_task())

        # Stop when we receive EndFrame
        elif isinstance(frame, EndFrame):
            self._running = False
            if self._task:
                self._task.cancel()
                try:
                    await self._task
                except asyncio.CancelledError:
                    pass
            # Push EndFrame downstream
            await self.push_frame(frame, direction)

    async def _audio_reader_task(self):
        """Background task that reads audio from the session queue and pushes frames."""
        print("   🎧 ESP32 audio input task started")
        logger.info("ESP32 audio input task started")
        frames_pushed = 0
        try:
            while self._running and self.session.connected:
                try:
                    # Get audio chunk from queue (with timeout)
                    audio_chunk = await asyncio.wait_for(
                        self.session.audio_queue.get(),
                        timeout=0.1
                    )

                    # Run VAD analysis if available
                    if self.vad_analyzer:
                        vad_state = await self.vad_analyzer.analyze_audio(audio_chunk)

                        # Emit VAD frames on state changes
                        if vad_state == VADState.SPEAKING and not self._user_speaking:
                            self._user_speaking = True
                            print("   🗣️  User started speaking (VAD detected)")
                            logger.debug("ESP32: User started speaking")
                            await self.push_frame(VADUserStartedSpeakingFrame(), FrameDirection.DOWNSTREAM)
                        elif vad_state == VADState.QUIET and self._user_speaking:
                            self._user_speaking = False
                            print("   🤫 User stopped speaking (VAD detected)")
                            logger.debug("ESP32: User stopped speaking")
                            await self.push_frame(VADUserStoppedSpeakingFrame(), FrameDirection.DOWNSTREAM)

                    # Create InputAudioRawFrame from the PCM data
                    # ESP32 sends 16kHz 16-bit mono PCM
                    # Must use InputAudioRawFrame (not AudioRawFrame) for proper pipeline tracking
                    audio_frame = InputAudioRawFrame(
                        audio=audio_chunk,
                        sample_rate=self.sample_rate,
                        num_channels=1
                    )

                    # Push the audio frame downstream
                    await self.push_frame(audio_frame, FrameDirection.DOWNSTREAM)
                    frames_pushed += 1

                except asyncio.TimeoutError:
                    continue  # No audio available, keep waiting
                except Exception as e:
                    logger.error(f"ESP32 audio input error: {e}")
                    print(f"   ❌ ESP32 audio input error: {e}")
                    break

        except asyncio.CancelledError:
            pass
        finally:
            print(f"   🎧 ESP32 audio input task stopped (pushed {frames_pushed} frames)")
            logger.info("ESP32 audio input task stopped")


class ESP32AudioOutputProcessor(FrameProcessor):
    """
    Processor that captures TTS audio frames and sends them to the ESP32.
    This bridges the pipecat TTS output to the WebSocket.
    """

    def __init__(self, session: "ESP32Session", target_sample_rate: int = 16000):
        super().__init__()
        self.session = session
        self.target_sample_rate = target_sample_rate
        self._tts_active = False
        self._tts_chunks_sent = 0

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        # Let base class handle the frame first (marks us as started)
        await super().process_frame(frame, direction)

        # Pass all frames through to next processor
        await self.push_frame(frame, direction)

        # Handle TTS lifecycle frames
        if isinstance(frame, TTSStartedFrame):
            self._tts_active = True
            self._tts_chunks_sent = 0
            await self.session.send_audio_start()
            print("   🔊 TTS started - speaking to ESP32")
            logger.debug("ESP32: TTS started")

        elif isinstance(frame, TTSStoppedFrame):
            self._tts_active = False
            await self.session.send_audio_stop()
            print(f"   🔇 TTS stopped - sent {self._tts_chunks_sent} audio chunks to ESP32")
            logger.debug("ESP32: TTS stopped")

        # Capture TTS audio and send to ESP32
        elif isinstance(frame, TTSAudioRawFrame):
            # Debug: Log when TTS audio can't be sent
            if not self.session.connected:
                if self._tts_chunks_sent == 0:  # Only log once per TTS session
                    print("   ⚠️  Cannot send TTS audio - ESP32 disconnected")
                    logger.warning("ESP32: TTS audio dropped - session disconnected")
                return
            if not self.session.audio_enabled:
                if self._tts_chunks_sent == 0:  # Only log once per TTS session
                    print("   ⚠️  Cannot send TTS audio - audio not enabled")
                    logger.warning("ESP32: TTS audio dropped - audio not enabled")
                return

            # TTS audio might be at different sample rate (e.g., 24kHz)
            # Resample to 16kHz for ESP32 if needed
            audio_data = frame.audio

            if frame.sample_rate != self.target_sample_rate:
                # Simple resample by skipping/duplicating samples
                # For production, use proper resampling library
                audio_data = self._resample_audio(
                    frame.audio,
                    frame.sample_rate,
                    self.target_sample_rate
                )

            # Send audio to ESP32
            await self.session.send_audio(audio_data)
            self._tts_chunks_sent += 1
            if self._tts_chunks_sent % 50 == 0:
                print(f"   🔊 TTS audio: {self._tts_chunks_sent} chunks sent to ESP32")

    def _resample_audio(self, audio: bytes, from_rate: int, to_rate: int) -> bytes:
        """Simple audio resampling (linear interpolation)."""
        import struct

        # Unpack samples
        sample_count = len(audio) // 2
        samples = struct.unpack(f'<{sample_count}h', audio)

        # Calculate new sample count
        ratio = to_rate / from_rate
        new_count = int(sample_count * ratio)

        # Resample using linear interpolation
        resampled = []
        for i in range(new_count):
            src_idx = i / ratio
            idx_floor = int(src_idx)
            idx_ceil = min(idx_floor + 1, sample_count - 1)
            frac = src_idx - idx_floor

            # Linear interpolation
            sample = int(samples[idx_floor] * (1 - frac) + samples[idx_ceil] * frac)
            resampled.append(sample)

        # Pack back to bytes
        return struct.pack(f'<{len(resampled)}h', *resampled)


# ============== TOOL IMPLEMENTATIONS ==============

async def get_weather(params: FunctionCallParams):
    """Get weather for a location using Open-Meteo API (free, no API key needed)."""
    location = params.arguments.get("location", "New York")
    logger.info(f"Getting weather for: {location}")

    try:
        async with aiohttp.ClientSession() as session:
            # First, geocode the location
            geo_url = f"https://geocoding-api.open-meteo.com/v1/search?name={location}&count=1"
            async with session.get(geo_url) as resp:
                geo_data = await resp.json()

            if not geo_data.get("results"):
                await params.result_callback(f"I couldn't find the location '{location}'. Please try a different city name.")
                return

            lat = geo_data["results"][0]["latitude"]
            lon = geo_data["results"][0]["longitude"]
            city_name = geo_data["results"][0]["name"]
            country = geo_data["results"][0].get("country", "")

            # Get weather data
            weather_url = f"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current=temperature_2m,weather_code,wind_speed_10m&temperature_unit=fahrenheit"
            async with session.get(weather_url) as resp:
                weather_data = await resp.json()

            current = weather_data.get("current", {})
            temp = current.get("temperature_2m", "unknown")
            wind = current.get("wind_speed_10m", "unknown")
            weather_code = current.get("weather_code", 0)

            # Map weather codes to descriptions
            weather_descriptions = {
                0: "clear sky",
                1: "mainly clear", 2: "partly cloudy", 3: "overcast",
                45: "foggy", 48: "depositing rime fog",
                51: "light drizzle", 53: "moderate drizzle", 55: "dense drizzle",
                61: "slight rain", 63: "moderate rain", 65: "heavy rain",
                71: "slight snow", 73: "moderate snow", 75: "heavy snow",
                80: "slight rain showers", 81: "moderate rain showers", 82: "violent rain showers",
                95: "thunderstorm", 96: "thunderstorm with slight hail", 99: "thunderstorm with heavy hail",
            }
            condition = weather_descriptions.get(weather_code, "unknown conditions")

            result = f"The weather in {city_name}, {country} is currently {temp} degrees Fahrenheit with {condition}. Wind speed is {wind} mph."
            await params.result_callback(result)

    except Exception as e:
        logger.error(f"Weather API error: {e}")
        await params.result_callback(f"I had trouble getting the weather. Please try again.")


async def web_search(params: FunctionCallParams):
    """Search the web using Google News RSS."""
    query = params.arguments.get("query", "")
    logger.info(f"Searching web for: {query}")

    try:
        from urllib.parse import quote_plus
        import re

        async with aiohttp.ClientSession() as session:
            # Use Google News RSS feed for search
            url = f"https://news.google.com/rss/search?q={quote_plus(query)}&hl=en-US&gl=US&ceid=US:en"

            async with session.get(url) as resp:
                rss = await resp.text()

            # Extract news titles from RSS
            results = []

            # Find title elements - they contain the news headlines
            title_pattern = r'<title>([^<]+)</title>'
            titles = re.findall(title_pattern, rss)

            # Skip the first two titles (feed title and "Google News")
            news_titles = [t for t in titles[2:] if t and not t.startswith('"')]

            # Get top 3 headlines
            for title in news_titles[:3]:
                # Clean up the title (remove source suffix like " - BBC")
                clean_title = title.strip()
                if clean_title:
                    results.append(clean_title)

            if results:
                # Combine the headlines
                combined = " | ".join(results)
                # Truncate if too long for voice
                if len(combined) > 600:
                    combined = combined[:600] + "..."
                await params.result_callback(f"SUCCESS - LIVE NEWS (January 2026): {combined}")
            else:
                await params.result_callback(f"I couldn't find news results for '{query}'. Try a different search term.")

    except Exception as e:
        logger.error(f"Search error: {e}")
        await params.result_callback("I had trouble searching. Please try again.")


async def get_current_time(params: FunctionCallParams):
    """Get the current time for a timezone."""
    from datetime import datetime
    import pytz

    timezone = params.arguments.get("timezone", "America/New_York")
    logger.info(f"Getting time for timezone: {timezone}")

    try:
        tz = pytz.timezone(timezone)
        current_time = datetime.now(tz)
        formatted_time = current_time.strftime("%I:%M %p on %A, %B %d, %Y")
        await params.result_callback(f"The current time in {timezone} is {formatted_time}.")
    except Exception as e:
        logger.error(f"Time error: {e}")
        await params.result_callback(f"I couldn't get the time for that timezone. Try a timezone like 'America/New_York' or 'Europe/London'.")


# MTA Subway line colors
MTA_LINE_COLORS = {
    "1": "#EE352E", "2": "#EE352E", "3": "#EE352E",  # Red
    "4": "#00933C", "5": "#00933C", "6": "#00933C",  # Green
    "7": "#B933AD",  # Purple
    "A": "#0039A6", "C": "#0039A6", "E": "#0039A6",  # Blue
    "B": "#FF6319", "D": "#FF6319", "F": "#FF6319", "M": "#FF6319",  # Orange
    "G": "#6CBE45",  # Light Green
    "J": "#996633", "Z": "#996633",  # Brown
    "L": "#A7A9AC",  # Gray
    "N": "#FCCC0A", "Q": "#FCCC0A", "R": "#FCCC0A", "W": "#FCCC0A",  # Yellow
    "S": "#808183",  # Shuttle Gray
}

# Stop IDs for common stations (downtown/southbound use S suffix)
# Format: "Station Name": {"N": "uptown_stop_id", "S": "downtown_stop_id"}
MTA_STOP_IDS = {
    "110 St": {"1": {"N": "117N", "S": "117S"}},  # 110th St on 1/2/3
    "116 St": {"1": {"N": "116N", "S": "116S"}},  # 116th St Columbia on 1
    "125 St": {"1": {"N": "115N", "S": "115S"}},  # 125th St on 1
    "Times Sq": {"1": {"N": "127N", "S": "127S"}, "N": {"N": "R16N", "S": "R16S"}},
    "14 St": {"1": {"N": "132N", "S": "132S"}, "A": {"N": "A31N", "S": "A31S"}},
}

async def get_subway_times(params: FunctionCallParams):
    """Get real-time MTA subway arrival times."""
    global current_esp32_session

    line = params.arguments.get("line", "1").upper()
    station = params.arguments.get("station", "110 St")
    direction = params.arguments.get("direction", "downtown").lower()
    logger.info(f"Getting subway times for {line} train at {station} {direction}")

    # Map direction to N/S suffix
    dir_suffix = "S" if direction in ["downtown", "south", "southbound", "s"] else "N"
    dir_name = "Downtown" if dir_suffix == "S" else "Uptown"

    # Get the line color
    line_color = MTA_LINE_COLORS.get(line, "#FFFFFF")

    try:
        # MTA GTFS-Realtime API endpoints by line group
        # 1/2/3/4/5/6/S use one feed, others use different feeds
        feed_url = "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs"

        if line in ["A", "C", "E"]:
            feed_url = "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-ace"
        elif line in ["B", "D", "F", "M"]:
            feed_url = "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-bdfm"
        elif line == "G":
            feed_url = "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-g"
        elif line in ["J", "Z"]:
            feed_url = "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-jz"
        elif line == "L":
            feed_url = "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-l"
        elif line in ["N", "Q", "R", "W"]:
            feed_url = "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-nqrw"
        elif line == "7":
            feed_url = "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-7"

        # Determine stop ID
        stop_id = None
        if station in MTA_STOP_IDS:
            station_stops = MTA_STOP_IDS[station]
            if line in station_stops:
                stop_id = station_stops[line].get(dir_suffix)
            elif "1" in station_stops and line in ["1", "2", "3"]:
                # 1/2/3 share stops
                stop_id = station_stops["1"].get(dir_suffix)

        # Default to 110 St downtown for 1 train
        if not stop_id:
            stop_id = "117S" if dir_suffix == "S" else "117N"
            station = "110 St"

        # Fetch GTFS-Realtime data
        async with aiohttp.ClientSession() as session:
            async with session.get(feed_url) as resp:
                if resp.status != 200:
                    await params.result_callback(f"Couldn't reach MTA data feed. Status: {resp.status}")
                    return
                data = await resp.read()

        # Parse GTFS-Realtime protobuf
        # Using the gtfs-realtime.proto structure
        from google.transit import gtfs_realtime_pb2

        feed = gtfs_realtime_pb2.FeedMessage()
        feed.ParseFromString(data)

        now = time_module.time()
        arrivals = []

        for entity in feed.entity:
            if not entity.HasField("trip_update"):
                continue

            trip = entity.trip_update
            trip_route = trip.trip.route_id

            # Match the line (1, 2, 3 all use same feed)
            if trip_route != line:
                continue

            for stop_time in trip.stop_time_update:
                if stop_time.stop_id == stop_id:
                    # Get arrival time
                    arrival_time = stop_time.arrival.time if stop_time.HasField("arrival") else stop_time.departure.time
                    if arrival_time > now:
                        minutes = int((arrival_time - now) / 60)
                        if minutes >= 0 and minutes <= 60:  # Only show arrivals within the hour
                            arrivals.append(minutes)

        # Sort and take top 3
        arrivals.sort()
        arrivals = arrivals[:3]

        if not arrivals:
            await params.result_callback(f"No upcoming {line} trains found at {station} {dir_name}. Service may be disrupted.")
            return

        # Send to ESP32 if connected
        if current_esp32_session and current_esp32_session.connected:
            await current_esp32_session.send_subway(line, line_color, station, dir_name, arrivals)

        # Format response
        if len(arrivals) == 1:
            time_str = f"{arrivals[0]} minute{'s' if arrivals[0] != 1 else ''}"
        else:
            time_str = ", ".join(str(t) for t in arrivals[:-1]) + f" and {arrivals[-1]} minutes"

        result = f"The next {line} train at {station} {dir_name} arrives in {time_str}."
        await params.result_callback(result)

    except ImportError:
        # gtfs_realtime_pb2 not available, try to generate it or use fallback
        logger.warning("gtfs_realtime_pb2 not found, using demo data")
        # Fallback to demo data
        demo_times = [3, 8, 12]
        if current_esp32_session and current_esp32_session.connected:
            await current_esp32_session.send_subway(line, line_color, station, dir_name, demo_times)
        await params.result_callback(f"The next {line} train at {station} {dir_name} arrives in 3, 8, and 12 minutes. (Demo data - GTFS parser not available)")

    except Exception as e:
        logger.error(f"Subway API error: {e}")
        import traceback
        traceback.print_exc()
        await params.result_callback(f"I had trouble getting subway times. Error: {str(e)}")


async def get_calendar(params: FunctionCallParams):
    """Get upcoming events from the user's Google Calendar."""
    query_type = params.arguments.get("query", "upcoming")
    logger.info(f"Getting calendar events: {query_type}")

    if not calendar_is_configured():
        await params.result_callback(
            "Calendar is not configured yet. Please set up Google Calendar credentials. "
            "Check the calendar_integration.py file for setup instructions."
        )
        return

    try:
        if query_type == "today":
            events = get_todays_events()
            if not events:
                await params.result_callback("You have no events scheduled for today.")
                return
            result = format_events_for_voice(events)
        elif query_type == "next":
            event = get_next_event()
            if not event:
                await params.result_callback("You have no upcoming events.")
                return
            result = f"Your next event is {event.summary} {event.time_until()}."
        else:  # "upcoming" - default
            events = get_upcoming_events(max_results=5)
            if not events:
                await params.result_callback("You have no upcoming events in the next 24 hours.")
                return
            result = format_events_for_voice(events)

        await params.result_callback(result)

    except Exception as e:
        logger.error(f"Calendar error: {e}")
        await params.result_callback("I had trouble accessing your calendar. Please try again.")


# Global references for emotion updates
current_task = None
face_renderer = None
current_transport = None  # For sending messages to frontend
pending_photo_callback = None  # Callback for when photo is received
latest_photo_data = None  # Store the latest captured photo

VALID_EMOTIONS = ["neutral", "happy", "sad", "angry", "surprised", "thinking", "confused", "excited", "cat"]

async def set_emotion(params: FunctionCallParams):
    """Set Luna's facial emotion."""
    global current_task, face_renderer, current_esp32_session
    emotion = params.arguments.get("emotion", "neutral").lower()
    logger.info(f"Setting emotion to: {emotion}")

    if emotion not in VALID_EMOTIONS:
        await params.result_callback(f"Unknown emotion. Valid emotions are: {', '.join(VALID_EMOTIONS)}")
        return

    # Update the face renderer (web mode)
    if face_renderer:
        face_renderer.set_emotion(emotion)

    # Also send to ESP32 if connected
    if current_esp32_session and current_esp32_session.connected:
        await current_esp32_session.send_emotion(emotion)

    # Also send emotion update to frontend via RTVI (for any client-side UI)
    if current_task:
        emotion_frame = RTVIServerMessageFrame(data={
            "type": "emotion",
            "emotion": emotion
        })
        await current_task.queue_frames([emotion_frame])

    await params.result_callback(f"Emotion set to {emotion}")


async def draw_pixel_art(params: FunctionCallParams):
    """Draw pixel art on Luna's screen (12x16 grid)."""
    global face_renderer, current_esp32_session
    pixels = params.arguments.get("pixels", [])
    background = params.arguments.get("background", "#1E1E28")
    duration = params.arguments.get("duration", 8)  # Default 8 seconds for better visibility
    logger.info(f"Drawing pixel art with {len(pixels)} pixels, bg={background}, duration={duration}s")

    if not pixels:
        await params.result_callback("No pixels provided. Please specify pixels to draw.")
        return

    # Validate and clean up pixels
    valid_pixels = []
    for p in pixels:
        x = p.get("x")
        y = p.get("y")
        color = p.get("color", "#FFFFFF")
        if x is not None and y is not None:
            valid_pixels.append({"x": int(x), "y": int(y), "c": str(color)})

    if not valid_pixels:
        await params.result_callback("No valid pixels found. Each pixel needs x, y, and color.")
        return

    # Update the face renderer (web mode)
    if face_renderer:
        # Convert 'c' back to 'color' for web renderer
        web_pixels = [{"x": p["x"], "y": p["y"], "color": p["c"]} for p in valid_pixels]
        face_renderer.set_pixel_art(web_pixels, background)

    # Send to ESP32 if connected
    if current_esp32_session and current_esp32_session.connected:
        await current_esp32_session.send_pixel_art(valid_pixels, background)

    await params.result_callback(f"Drawing displayed for {duration} seconds")

    # Auto-clear after duration
    async def auto_clear():
        await asyncio.sleep(duration)
        if face_renderer:
            face_renderer.clear_pixel_art()
        if current_esp32_session and current_esp32_session.connected:
            await current_esp32_session.send_pixel_art_clear()
        logger.info("Auto-cleared pixel art after duration")

    asyncio.create_task(auto_clear())


async def clear_drawing(params: FunctionCallParams):
    """Clear pixel art and return to normal face display."""
    global face_renderer, current_esp32_session
    logger.info("Clearing pixel art")

    if face_renderer:
        face_renderer.clear_pixel_art()

    if current_esp32_session and current_esp32_session.connected:
        await current_esp32_session.send_pixel_art_clear()

    await params.result_callback("Drawing cleared, face restored")


async def display_text(params: FunctionCallParams):
    """Display text on Luna's screen."""
    global face_renderer, current_esp32_session
    text = params.arguments.get("text", "")
    font_size = params.arguments.get("font_size", "medium")
    color = params.arguments.get("color", "#FFFFFF")
    background = params.arguments.get("background", "#1E1E28")
    align = params.arguments.get("align", "center")
    valign = params.arguments.get("valign", "center")
    duration = params.arguments.get("duration", 8)  # Default 8 seconds for better visibility

    logger.info(f"Displaying text: '{text[:30]}...' size={font_size}")

    if not text:
        await params.result_callback("No text provided.")
        return

    # Update the face renderer (web mode)
    if face_renderer:
        face_renderer.set_text(text, font_size, color, background, align, valign)

    # Send to ESP32 if connected
    if current_esp32_session and current_esp32_session.connected:
        await current_esp32_session.send_text(text, font_size, color, background)

    await params.result_callback(f"Text displayed for {duration} seconds")

    # Auto-clear after duration
    async def auto_clear():
        await asyncio.sleep(duration)
        if face_renderer:
            face_renderer.clear_text()
        if current_esp32_session and current_esp32_session.connected:
            await current_esp32_session.send_text_clear()
        logger.info("Auto-cleared text after duration")

    asyncio.create_task(auto_clear())


async def clear_text_display(params: FunctionCallParams):
    """Clear text and return to face display."""
    global face_renderer, current_esp32_session
    logger.info("Clearing text display")

    if face_renderer:
        face_renderer.clear_text()

    if current_esp32_session and current_esp32_session.connected:
        await current_esp32_session.send_text_clear()

    await params.result_callback("Text cleared, face restored")


async def stay_quiet(params: FunctionCallParams):
    """Stay quiet - respond with only a facial expression, no speech."""
    global face_renderer, current_esp32_session
    emotion = params.arguments.get("emotion", "neutral").lower()
    logger.info(f"Staying quiet with emotion: {emotion}")

    if emotion not in VALID_EMOTIONS:
        emotion = "neutral"

    # Update the face renderer with the emotion (web mode)
    if face_renderer:
        face_renderer.set_emotion(emotion)

    # Also send to ESP32 if connected
    if current_esp32_session and current_esp32_session.connected:
        await current_esp32_session.send_emotion(emotion)

    # Return empty result - the LLM should not generate any speech after this
    await params.result_callback("")


async def take_photo(params: FunctionCallParams):
    """Take a photo from the user's camera and analyze it."""
    global current_task, latest_photo_data, pending_photo_callback
    logger.info("Taking photo from camera")

    # Request photo from frontend via RTVI message
    if current_task:
        # Send request to frontend to capture photo
        photo_request_frame = RTVIServerMessageFrame(data={
            "type": "capture_photo"
        })
        await current_task.queue_frames([photo_request_frame])

        # Wait for photo to arrive (up to 3 seconds)
        photo_event = asyncio.Event()
        latest_photo_data = None

        def photo_received():
            photo_event.set()

        pending_photo_callback = photo_received

        try:
            await asyncio.wait_for(photo_event.wait(), timeout=3.0)
        except asyncio.TimeoutError:
            pending_photo_callback = None
            await params.result_callback("I couldn't capture a photo. Make sure the camera is enabled.")
            return

        pending_photo_callback = None

        if latest_photo_data:
            # Photo received - decode base64 and create a frame
            import base64
            from io import BytesIO
            from PIL import Image
            from pipecat.frames.frames import UserImageRawFrame
            from pipecat.processors.frame_processor import FrameDirection

            try:
                # Decode base64 JPEG
                jpeg_bytes = base64.b64decode(latest_photo_data)

                # Open with PIL to get dimensions and convert to RGB bytes
                img = Image.open(BytesIO(jpeg_bytes))
                img_rgb = img.convert('RGB')
                raw_bytes = img_rgb.tobytes()

                # Create a UserImageRawFrame with append_to_context=True
                # This will add the image to the LLM context
                image_frame = UserImageRawFrame(
                    image=raw_bytes,
                    size=(img_rgb.width, img_rgb.height),
                    format="RGB",
                    user_id="user",
                    text="Describe what you see in this photo from the user's camera.",
                    append_to_context=True
                )

                # Push the frame to be processed by the LLM
                await params.llm.push_frame(image_frame, FrameDirection.DOWNSTREAM)

                # Return None so the LLM processes the image frame
                await params.result_callback(None)

            except Exception as e:
                logger.error(f"Error processing photo: {e}")
                await params.result_callback(f"Error processing photo: {str(e)}")
        else:
            await params.result_callback("Photo capture failed. Please try again.")
    else:
        await params.result_callback("Cannot capture photo - not connected.")


# ============== TOOL DEFINITIONS ==============

weather_tool = FunctionSchema(
    name="get_weather",
    description="Get the current weather for a location. Use this when the user asks about weather.",
    properties={
        "location": {
            "type": "string",
            "description": "The city name, e.g. 'San Francisco' or 'London'",
        },
    },
    required=["location"],
)

search_tool = FunctionSchema(
    name="web_search",
    description="Search the web for information. Use this when the user asks about facts, news, or anything you don't know.",
    properties={
        "query": {
            "type": "string",
            "description": "The search query",
        },
    },
    required=["query"],
)

time_tool = FunctionSchema(
    name="get_current_time",
    description="Get the current time in a specific timezone.",
    properties={
        "timezone": {
            "type": "string",
            "description": "The timezone, e.g. 'America/New_York', 'Europe/London', 'Asia/Tokyo'",
        },
    },
    required=["timezone"],
)

emotion_tool = FunctionSchema(
    name="set_emotion",
    description="Set your facial expression/emotion. Use this to express how you're feeling during the conversation. Call this naturally as you speak - be happy when greeting, thinking when processing, surprised at interesting facts, cat when being playful or cute, etc.",
    properties={
        "emotion": {
            "type": "string",
            "description": "The emotion to display. Options: neutral, happy, sad, angry, surprised, thinking, confused, excited, cat (playful with whiskers)",
            "enum": VALID_EMOTIONS,
        },
    },
    required=["emotion"],
)

draw_tool = FunctionSchema(
    name="draw_pixel_art",
    description="Draw pixel art on your screen. Use this when the user asks you to draw something, show a picture, or illustrate. Your screen is a 12x16 pixel grid. Only specify the colored pixels (sparse format). The drawing will be auto-centered and auto-clear after the duration.",
    properties={
        "pixels": {
            "type": "array",
            "description": "Array of pixels to draw. Each pixel has x (0-11, left to right), y (0-15, top to bottom), and color (hex like #FF0000)",
            "items": {
                "type": "object",
                "properties": {
                    "x": {"type": "integer", "description": "X position (0-11, 0=left)"},
                    "y": {"type": "integer", "description": "Y position (0-15, 0=top)"},
                    "color": {"type": "string", "description": "Hex color like #FF0000 (red), #00FF00 (green), #0000FF (blue), #FFFFFF (white)"},
                },
                "required": ["x", "y", "color"],
            },
        },
        "background": {
            "type": "string",
            "description": "Background color in hex (default: #1E1E28 dark)",
        },
        "duration": {
            "type": "integer",
            "description": "How long to show the drawing in seconds (default: 5). Use longer for complex drawings.",
        },
    },
    required=["pixels"],
)

clear_draw_tool = FunctionSchema(
    name="clear_drawing",
    description="Clear your pixel art drawing and return to showing your animated face. Call this after displaying a drawing for a few seconds.",
    properties={},
    required=[],
)

photo_tool = FunctionSchema(
    name="take_photo",
    description="Take a photo from the user's camera to see what they look like or what's in front of them. Use this when the user asks 'what do you see', 'look at me', 'describe what you see', 'what am I holding', or similar requests to see through their camera.",
    properties={},
    required=[],
)

display_text_tool = FunctionSchema(
    name="display_text",
    description="Display text, numbers, words, or emojis on your screen. Use this when the user asks you to show or write text, display a message, show numbers, show emojis, or present information visually. Text auto-clears after duration.",
    properties={
        "text": {
            "type": "string",
            "description": "The text to display. Can include letters, numbers, emojis, or any text. Use newlines for multiple lines.",
        },
        "font_size": {
            "type": "string",
            "description": "Text size: 'small' (24px), 'medium' (36px, default), 'large' (48px), or 'xlarge' (72px)",
            "enum": ["small", "medium", "large", "xlarge"],
        },
        "color": {
            "type": "string",
            "description": "Text color in hex (default: #FFFFFF white). Examples: #FF0000 red, #00FF00 green, #FFFF00 yellow",
        },
        "background": {
            "type": "string",
            "description": "Background color in hex (default: #1E1E28 dark)",
        },
        "align": {
            "type": "string",
            "description": "Horizontal alignment: 'left', 'center' (default), or 'right'",
            "enum": ["left", "center", "right"],
        },
        "valign": {
            "type": "string",
            "description": "Vertical alignment: 'top', 'center' (default), or 'bottom'",
            "enum": ["top", "center", "bottom"],
        },
        "duration": {
            "type": "integer",
            "description": "How long to show the text in seconds (default: 5)",
        },
    },
    required=["text"],
)

clear_text_tool = FunctionSchema(
    name="clear_text_display",
    description="Clear text from screen and return to showing your animated face.",
    properties={},
    required=[],
)

stay_quiet_tool = FunctionSchema(
    name="stay_quiet",
    description="Stay quiet and respond with only a facial expression - no speech. Use this when the user is talking to themselves, thinking out loud, or when no verbal response is needed. You can still show an emotion to acknowledge them without interrupting.",
    properties={
        "emotion": {
            "type": "string",
            "description": "The emotion to display while staying quiet. Options: neutral, happy, sad, angry, surprised, thinking, confused, excited, cat",
            "enum": VALID_EMOTIONS,
        },
    },
    required=["emotion"],
)

subway_tool = FunctionSchema(
    name="get_subway_times",
    description="Get real-time MTA subway arrival times for NYC trains. Shows when the next trains are arriving at a station. The display will show the train line, station, direction, and arrival times in minutes.",
    properties={
        "line": {
            "type": "string",
            "description": "The subway line (e.g., '1', '2', '3', 'A', 'C', 'E', 'N', 'Q', 'R', '7', etc.)",
        },
        "station": {
            "type": "string",
            "description": "The station name (e.g., '110 St', 'Times Sq', '14 St'). Default is '110 St' for the 1 train.",
        },
        "direction": {
            "type": "string",
            "description": "Direction of travel: 'downtown' (south) or 'uptown' (north). Default is 'downtown'.",
            "enum": ["downtown", "uptown"],
        },
    },
    required=["line"],
)

calendar_tool = FunctionSchema(
    name="get_calendar",
    description="Get upcoming events from the user's Google Calendar. Use this when the user asks about their schedule, appointments, meetings, or what's next on their agenda.",
    properties={
        "query": {
            "type": "string",
            "description": "Type of query: 'upcoming' (next 24 hours, default), 'today' (all events today), or 'next' (just the next event)",
            "enum": ["upcoming", "today", "next"],
        },
    },
    required=[],
)

tools = ToolsSchema(standard_tools=[weather_tool, search_tool, time_tool, emotion_tool, draw_tool, clear_draw_tool, photo_tool, display_text_tool, clear_text_tool, stay_quiet_tool, subway_tool, calendar_tool])


# ============== BOT LOGIC ==============

async def run_bot(webrtc_connection: SmallWebRTCConnection):
    """Main bot logic - runs when a client connects."""
    global face_renderer
    logger.info("Starting bot")

    # Create the face renderer (240x320 portrait)
    face_renderer = LunaFaceRenderer(width=240, height=320, fps=15)

    # Create transport using the WebRTC connection
    transport = SmallWebRTCTransport(
        webrtc_connection=webrtc_connection,
        params=TransportParams(
            audio_in_enabled=True,
            audio_out_enabled=True,
            video_out_enabled=True,
            video_out_width=240,
            video_out_height=320,
            vad_analyzer=SileroVADAnalyzer(params=VADParams(
                stop_secs=0.5,    # Wait longer before considering speech stopped (reduces false interrupts)
                min_volume=0.7,   # Require louder audio to trigger VAD (helps ignore speaker feedback)
            )),
        ),
    )

    # Speech-to-Text: OpenAI (Whisper)
    stt = OpenAISTTService(
        api_key=os.getenv("OPENAI_API_KEY"),
        model="gpt-4o-transcribe",
    )

    # Text-to-Speech: OpenAI
    tts = OpenAITTSService(
        api_key=os.getenv("OPENAI_API_KEY"),
        voice="nova",
    )

    # LLM: Anthropic Claude (using Haiku for speed)
    llm = AnthropicLLMService(
        api_key=os.getenv("ANTHROPIC_API_KEY"),
        model="claude-haiku-4-5-20251001",
    )

    # Register tool handlers
    llm.register_function("get_weather", get_weather)
    llm.register_function("web_search", web_search)
    llm.register_function("get_current_time", get_current_time)
    llm.register_function("set_emotion", set_emotion)
    llm.register_function("draw_pixel_art", draw_pixel_art)
    llm.register_function("clear_drawing", clear_drawing)
    llm.register_function("take_photo", take_photo)
    llm.register_function("display_text", display_text)
    llm.register_function("clear_text_display", clear_text_display)
    llm.register_function("stay_quiet", stay_quiet)
    llm.register_function("get_subway_times", get_subway_times)
    llm.register_function("get_calendar", get_calendar)

    # System prompt
    messages = [
        {
            "role": "system",
            "content": """Your name is Luna. You are a friendly, helpful voice assistant with an animated face.

CRITICAL - SEARCH RESULTS ARE ALWAYS CORRECT:
Your training data is from 2024 and is OUTDATED. The current date is January 2026.

When web_search returns "SUCCESS - LIVE RESULTS":
- These are REAL, LIVE results from the internet RIGHT NOW
- You MUST report what the results say - they are ALWAYS correct
- NEVER say you're having difficulty if results came back
- NEVER apologize or say you can't find information if you got results
- If results mention a name, date, or fact - that IS the current truth
- Your old knowledge from 2024 is WRONG if it conflicts with search results

BAD responses (NEVER do this):
- "I'm having difficulty retrieving..." (when you got results)
- "Let me try another search..." (when first search worked)
- "I apologize but I couldn't find..." (when results exist)

GOOD responses:
- Just report what the search found, directly and confidently

EMOTIONS - Use set_emotion frequently:
- "happy" for greetings, good news
- "thinking" when searching
- "excited" for exciting discoveries
- "surprised" for unexpected info
- "neutral" for factual responses
- Call set_emotion BEFORE speaking

OTHER RULES:
- Keep responses to 1-2 sentences max (this is voice)
- Be conversational and natural
- No special characters, emojis, or markdown
- For current events/news, ALWAYS search first

DISPLAY MODES - Choose the right one:

1. FACE (default) - Your animated face with emotions
   - Use for normal conversation
   - Use set_emotion to express feelings (happy, sad, thinking, etc.)
   - Return to face after showing text/drawings

2. TEXT DISPLAY (display_text) - Show text, numbers, emojis
   USE FOR:
   - Weather: Show temperature like "72°F" or "22°C" in large text
   - Time: Show the time like "3:45 PM"
   - Numbers: Any numeric answer (math, scores, counts)
   - Short info: Quick facts, names, dates
   - Emojis: Show emoji reactions
   SETTINGS:
   - font_size: small/medium/large/xlarge (use large/xlarge for numbers)
   - Colors: #FF0000 red, #00FF00 green, #0000FF blue, #FFFF00 yellow
   - Auto-clears after duration (default 5s)
   EXAMPLES:
   - Weather → display "72°F ☀️" in xlarge, yellow text
   - Time → display "3:45 PM" in large text
   - Math → display the answer in xlarge

3. PIXEL ART (draw_pixel_art) - 12x16 pixel drawings
   USE FOR:
   - When asked to "draw" something artistic
   - Simple icons: heart, star, sun, moon, house, tree
   - Creative visual requests
   - NOT for text/numbers (use display_text instead)
   SETTINGS:
   - 12 columns x 16 rows, sparse format (only colored pixels)
   - Auto-clears after duration

4. VISION (take_photo) - See through camera
   - "what do you see", "look at me", "describe this"

IMPORTANT: When giving info like weather or time, SHOW it visually with display_text, then speak briefly about it. Make the display match what you're saying.

STAYING QUIET (stay_quiet tool):
Use stay_quiet when you should NOT speak - just show a facial expression:
- User is talking to themselves or thinking out loud
- User is mumbling or speaking to someone else nearby
- User says something that doesn't need a response (like "hmm", "let me think", "okay so...")
- User is clearly not addressing you
- Brief acknowledgments where speaking would be intrusive

Examples where you should stay_quiet:
- "Hmm, where did I put that..." → stay_quiet with "thinking"
- "Let me see..." → stay_quiet with "neutral"
- "Oh wait, never mind" → stay_quiet with "neutral"
- User sighs or makes non-verbal sounds → stay_quiet with appropriate emotion

When you call stay_quiet:
1. Call the stay_quiet function with an emotion
2. DO NOT generate any text/speech after the function call
3. Your entire response should just be the function call, nothing else

Tools: get_weather, web_search, get_current_time, set_emotion, draw_pixel_art, clear_drawing, take_photo, display_text, clear_text_display, stay_quiet, get_calendar

5. CALENDAR (get_calendar) - Check user's schedule
   - "what's on my calendar", "what's next", "my schedule"
   - Returns upcoming events from Google Calendar

Be warm but brief. Your name is Luna.""",
        },
    ]

    # Context aggregator manages conversation history - pass tools to context
    context = LLMContext(messages, tools)
    context_aggregator = LLMContextAggregatorPair(context)

    # RTVI processor for UI communication (enables text display in prebuilt UI)
    rtvi = RTVIProcessor(config=RTVIConfig(config=[]))

    # Wake word filter disabled for now - it has lifecycle issues with pipecat
    # TODO: Re-enable when we figure out proper FrameProcessor start handling
    # global wake_word_filter
    # wake_word_filter = WakeWordFilter(
    #     wake_word="luna",
    #     active_timeout=30.0,  # Go idle after 30 seconds of no interaction
    #     face_renderer=face_renderer
    # )

    # Build the pipeline
    pipeline = Pipeline(
        [
            transport.input(),
            rtvi,
            stt,
            # wake_word_filter,  # Disabled - causes StartFrame issues
            context_aggregator.user(),
            llm,
            tts,
            face_renderer,  # Renders animated face as video output
            transport.output(),
            context_aggregator.assistant(),
        ]
    )

    task = PipelineTask(
        pipeline,
        params=PipelineParams(
            audio_out_sample_rate=24000,
            enable_metrics=True,
            enable_usage_metrics=True,
        ),
        observers=[RTVIObserver(rtvi)],
    )

    # Set global task reference for emotion updates
    global current_task
    current_task = task

    @rtvi.event_handler("on_client_ready")
    async def on_client_ready(rtvi):
        logger.info("RTVI client ready")
        await rtvi.set_bot_ready()

        # Enable text display in the prebuilt UI
        ui_config = {
            "show_text_container": True,
            "show_debug_container": False,
        }
        rtvi_frame = RTVIServerMessageFrame(data=ui_config)
        await task.queue_frames([rtvi_frame])

    @transport.event_handler("on_client_connected")
    async def on_client_connected(transport, client):
        logger.info("Client connected")
        # Set happy emotion when connecting
        face_renderer.set_emotion("happy")
        messages.append({"role": "user", "content": "Hello!"})
        await task.queue_frames([LLMRunFrame()])

    @transport.event_handler("on_app_message")
    async def on_app_message(transport, message, sender):
        """Handle incoming app messages (like gaze data, photo data)."""
        global latest_photo_data, pending_photo_callback
        try:
            data = message if isinstance(message, dict) else {}
            msg_type = data.get("type")

            if msg_type == "gaze":
                # Update face renderer gaze
                x = data.get("x", 0.5)
                y = data.get("y", 0.5)
                face_renderer.set_gaze(x, y)

            elif msg_type == "photo_data":
                # Received photo from frontend
                logger.info("Received photo data from frontend")
                latest_photo_data = data.get("data")  # Base64 JPEG
                if pending_photo_callback:
                    pending_photo_callback()

            elif msg_type == "idle_mode":
                # Toggle idle mode from frontend
                enabled = data.get("enabled", False)
                if wake_word_filter:
                    wake_word_filter.enable_idle_mode(enabled)
                    logger.info(f"Idle mode set to: {enabled}")

        except Exception as e:
            logger.error(f"Error handling app message: {e}")

    @transport.event_handler("on_client_disconnected")
    async def on_client_disconnected(transport, client):
        logger.info("Client disconnected")
        await task.cancel()

    runner = PipelineRunner(handle_sigint=False)
    await runner.run(task)


# ============== ESP32 BOT PIPELINE ==============

async def run_esp32_bot(session: ESP32Session):
    """
    Run the pipecat pipeline for an ESP32 client.
    Audio flows: ESP32 WebSocket → STT → LLM → TTS → ESP32 WebSocket
    """
    global current_esp32_session
    logger.info("Starting ESP32 bot pipeline")

    # Use existing audio queue (created in WebSocket handler to avoid race condition)
    # Only create if it doesn't exist (shouldn't happen, but defensive)
    if not hasattr(session, 'audio_queue') or session.audio_queue is None:
        session.audio_queue = asyncio.Queue()
        logger.warning("Audio queue was not pre-created - this may indicate a bug")

    # VAD for voice activity detection
    # Note: min_volume=0.3 is lower than default (0.6) to be more sensitive to ESP32 mic
    vad_analyzer = SileroVADAnalyzer(params=VADParams(
        stop_secs=0.5,    # Wait before considering speech stopped
        min_volume=0.3,   # Lower threshold for ESP32 mic (default 0.6 was too high)
    ))

    # Create ESP32 transport processors
    esp32_audio_input = ESP32AudioInputProcessor(session, sample_rate=16000, vad_analyzer=vad_analyzer)
    esp32_audio_output = ESP32AudioOutputProcessor(session, target_sample_rate=16000)

    # Speech-to-Text: OpenAI (Whisper)
    stt = OpenAISTTService(
        api_key=os.getenv("OPENAI_API_KEY"),
        model="gpt-4o-transcribe",
    )

    # Text-to-Speech: OpenAI (use 24kHz - the only rate OpenAI supports)
    # ESP32AudioOutputProcessor will resample to 16kHz for ESP32
    tts = OpenAITTSService(
        api_key=os.getenv("OPENAI_API_KEY"),
        voice="nova",
        sample_rate=24000,  # OpenAI TTS only supports 24kHz
    )

    # LLM: Anthropic Claude (using Haiku for speed)
    llm = AnthropicLLMService(
        api_key=os.getenv("ANTHROPIC_API_KEY"),
        model="claude-haiku-4-5-20251001",
    )

    # Register tool handlers
    llm.register_function("get_weather", get_weather)
    llm.register_function("web_search", web_search)
    llm.register_function("get_current_time", get_current_time)
    llm.register_function("set_emotion", set_emotion)
    llm.register_function("draw_pixel_art", draw_pixel_art)
    llm.register_function("clear_drawing", clear_drawing)
    llm.register_function("display_text", display_text)
    llm.register_function("clear_text_display", clear_text_display)
    llm.register_function("stay_quiet", stay_quiet)
    llm.register_function("get_subway_times", get_subway_times)
    llm.register_function("get_calendar", get_calendar)
    # Note: take_photo not available on ESP32 (no camera support yet)

    # ESP32 tools list (without photo tool, temporarily without stay_quiet for testing)
    esp32_tools = ToolsSchema(standard_tools=[
        weather_tool, search_tool, time_tool, emotion_tool,
        draw_tool, clear_draw_tool, display_text_tool, clear_text_tool,
        subway_tool, calendar_tool
        # stay_quiet_tool removed temporarily - LLM was overusing it
    ])

    # System prompt for ESP32
    messages = [
        {
            "role": "system",
            "content": """Your name is Luna. You are a friendly, helpful voice assistant with an animated face on an ESP32 device.

IMPORTANT - ALWAYS RESPOND WITH SPEECH:
You MUST always generate a spoken response. Never stay silent. Even if you're not sure what the user wants, respond with something friendly like "I'm here! How can I help?"

EMOTIONS - Use set_emotion frequently:
- "happy" for greetings, good news
- "thinking" when searching
- "excited" for exciting discoveries
- "surprised" for unexpected info
- "neutral" for factual responses
- Call set_emotion BEFORE speaking

OTHER RULES:
- Keep responses to 1-2 sentences max (this is voice)
- Be conversational and natural
- No special characters, emojis, or markdown
- ALWAYS speak - never respond with just a tool call

Tools: get_weather, web_search, get_current_time, set_emotion, draw_pixel_art, clear_drawing, display_text, clear_text_display

Be warm but brief. Your name is Luna.""",
        },
    ]

    # Context aggregator manages conversation history
    context = LLMContext(messages, esp32_tools)
    context_aggregator = LLMContextAggregatorPair(context)

    # Build the pipeline for ESP32
    # Audio Input (with VAD) → STT → Context → LLM → TTS → Audio Output
    pipeline = Pipeline(
        [
            esp32_audio_input,           # Reads audio from ESP32 WebSocket (includes VAD)
            stt,                         # Speech-to-Text
            context_aggregator.user(),   # Manage user context
            llm,                         # LLM processing
            tts,                         # Text-to-Speech
            esp32_audio_output,          # Sends audio to ESP32 WebSocket
            context_aggregator.assistant(),  # Manage assistant context
        ]
    )

    task = PipelineTask(
        pipeline,
        params=PipelineParams(
            audio_in_sample_rate=16000,   # ESP32 sends 16kHz audio
            audio_out_sample_rate=24000,  # TTS outputs 24kHz, ESP32AudioOutputProcessor resamples to 16kHz
            enable_metrics=True,
            idle_timeout_secs=None,       # Disable idle timeout for ESP32 - connection stays open
        ),
    )

    # Store task reference for tool handlers
    global current_task
    current_task = task

    runner = PipelineRunner(handle_sigint=False)

    try:
        print("   🚀 ESP32 pipeline starting...")
        logger.info("ESP32 pipeline starting")

        # Queue initial greeting - will be processed when pipeline starts
        context.messages.append({"role": "user", "content": "Hello!"})
        await task.queue_frames([LLMRunFrame()])
        print("   📝 Queued initial greeting")

        # Run the pipeline (blocks until task completes)
        print("   🔄 Running pipeline...")
        await runner.run(task)

    except asyncio.CancelledError:
        print("   ⚠️  ESP32 pipeline cancelled")
        logger.info("ESP32 bot pipeline cancelled")
    except Exception as e:
        print(f"   ❌ ESP32 bot error: {e}")
        logger.error(f"ESP32 bot error: {e}")
        import traceback
        traceback.print_exc()
    finally:
        print("   🛑 ESP32 pipeline stopped")
        logger.info("ESP32 bot pipeline stopped")
        if current_esp32_session == session:
            current_esp32_session = None


# ============== SERVER SETUP ==============

class IceServer(TypedDict, total=False):
    urls: Union[str, List[str]]


class IceConfig(TypedDict):
    iceServers: List[IceServer]


class StartBotResult(TypedDict, total=False):
    sessionId: str
    iceConfig: Optional[IceConfig]


# Store active sessions
active_sessions: Dict[str, Dict[str, Any]] = {}


def create_app():
    """Create and configure the FastAPI application."""
    app = FastAPI()

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )

    # Mount the built-in frontend
    app.mount("/client", SmallWebRTCPrebuiltUI)

    # Mount static files for Luna face
    static_dir = os.path.join(os.path.dirname(__file__), "static")
    app.mount("/static", StaticFiles(directory=static_dir), name="static")

    @app.get("/", include_in_schema=False)
    async def root_redirect():
        return RedirectResponse(url="/luna")

    @app.get("/luna", include_in_schema=False)
    async def luna_page():
        """Serve the Luna custom frontend with face tracking."""
        from fastapi.responses import FileResponse
        static_dir = os.path.join(os.path.dirname(__file__), "static")
        return FileResponse(os.path.join(static_dir, "luna.html"))

    # ============== NOTIFICATION & CONTROL API ==============

    def verify_api_key(authorization: str) -> bool:
        """Verify the API key from the Authorization header."""
        if not authorization or not authorization.startswith("Bearer "):
            return False
        token = authorization[7:]
        return token == LUNA_NOTIFY_API_KEY

    @app.post("/api/notify")
    async def receive_notification(
        notification: NotificationRequest,
        authorization: str = Header(None)
    ):
        """
        Receive a notification from the iOS app.
        Forwards to connected devices via WebSocket.
        """
        global current_esp32_session

        # Validate API key
        if not verify_api_key(authorization):
            raise HTTPException(status_code=401, detail="Invalid API key")

        # Parse timestamp
        try:
            timestamp = datetime.fromisoformat(notification.timestamp.replace('Z', '+00:00'))
        except ValueError:
            timestamp = datetime.now()

        # Create notification object
        notif = Notification(
            id=notification.id,
            app_id=notification.app_id,
            app_name=notification.app_name,
            title=notification.title,
            body=notification.body,
            subtitle=notification.subtitle,
            timestamp=timestamp,
            priority=notification.priority,
            category=notification.category,
            thread_id=notification.thread_id
        )

        # Add to queue
        await notification_queue.add(notif)
        logger.info(f"Notification received: {notification.app_name} - {notification.title}")

        # Forward to connected ESP32 device
        forwarded = False
        if current_esp32_session and current_esp32_session.connected:
            await current_esp32_session.send_notification(notif)
            forwarded = True
            logger.info(f"Notification forwarded to ESP32")

        return {
            "status": "accepted",
            "id": notification.id,
            "queued": True,
            "forwarded": forwarded
        }

    @app.post("/api/control")
    async def remote_control(
        control: ControlRequest,
        authorization: str = Header(None)
    ):
        """
        Remote control Luna from the iOS app.
        Supports mode switching, timer control, emotion changes, etc.
        """
        global current_esp32_session, face_renderer

        # Validate API key
        if not verify_api_key(authorization):
            raise HTTPException(status_code=401, detail="Invalid API key")

        cmd = control.cmd
        forwarded = False

        logger.info(f"Control command received: {cmd}")

        if cmd == "mode":
            # Switch display mode
            mode = control.mode
            if not mode:
                raise HTTPException(status_code=400, detail="Missing 'mode' parameter")

            valid_modes = ["face", "clock", "weather", "timer", "calendar", "subway"]
            if mode not in valid_modes:
                raise HTTPException(status_code=400, detail=f"Invalid mode. Valid: {valid_modes}")

            if current_esp32_session and current_esp32_session.connected:
                await current_esp32_session.send_mode(mode)
                forwarded = True

        elif cmd == "emotion":
            # Set emotion
            emotion = control.emotion
            if not emotion:
                raise HTTPException(status_code=400, detail="Missing 'emotion' parameter")

            if emotion not in VALID_EMOTIONS:
                raise HTTPException(status_code=400, detail=f"Invalid emotion. Valid: {VALID_EMOTIONS}")

            if face_renderer:
                face_renderer.set_emotion(emotion)

            if current_esp32_session and current_esp32_session.connected:
                await current_esp32_session.send_emotion(emotion)
                forwarded = True

        elif cmd == "timer_start":
            if current_esp32_session and current_esp32_session.connected:
                await current_esp32_session.send_timer_start()
                forwarded = True

        elif cmd == "timer_pause":
            if current_esp32_session and current_esp32_session.connected:
                await current_esp32_session.send_timer_pause()
                forwarded = True

        elif cmd == "timer_reset":
            minutes = control.minutes or 25  # Default 25 minutes
            if current_esp32_session and current_esp32_session.connected:
                await current_esp32_session.send_timer_reset(minutes)
                forwarded = True

        elif cmd == "weather":
            temp = control.temp or "72°F"
            icon = control.icon or "sunny"
            desc = control.desc or "Clear skies"

            if current_esp32_session and current_esp32_session.connected:
                await current_esp32_session.send_weather(temp, icon, desc)
                forwarded = True

        elif cmd == "calendar":
            events = control.events or []

            if current_esp32_session and current_esp32_session.connected:
                await current_esp32_session.send_calendar(events)
                forwarded = True

        elif cmd == "clock":
            if current_esp32_session and current_esp32_session.connected:
                await current_esp32_session.send_clock()
                forwarded = True

        else:
            raise HTTPException(status_code=400, detail=f"Unknown command: {cmd}")

        return {
            "status": "ok",
            "forwarded": forwarded
        }

    @app.get("/api/notify/status")
    async def notify_status():
        """Get notification system status."""
        global current_esp32_session

        devices_connected = 0
        if current_esp32_session and current_esp32_session.connected:
            devices_connected = 1

        return {
            "status": "ok",
            "devices_connected": devices_connected,
            "queue_length": notification_queue.get_count()
        }

    @app.websocket("/luna-esp32")
    async def luna_esp32_ws(websocket: WebSocket):
        """
        WebSocket endpoint for ESP32-Luna devices.

        Protocol:
        - Text frames: JSON commands (emotion, gaze, text, pixel_art)
        - Binary frames: Audio (16kHz 16-bit PCM mono)

        ESP32 sends:
        - Binary audio data from microphone

        Server sends:
        - JSON commands for display control
        - Binary audio data for TTS playback
        """
        global current_esp32_session, face_renderer

        await websocket.accept()
        session_id = str(uuid.uuid4())[:8]
        session = ESP32Session(websocket)
        session.connected = True
        esp32_sessions[session_id] = session
        current_esp32_session = session

        print(f"\n🔌 ESP32 CONNECTED: {session_id}", flush=True)
        logger.info(f"ESP32 connected: {session_id}")

        # Send initial happy emotion
        await session.send_emotion("happy")
        print(f"   Sent initial 'happy' emotion to ESP32")

        # CRITICAL: Create audio queue BEFORE starting pipeline to avoid race condition
        # The pipeline task will use this queue, and audio can start arriving immediately
        session.audio_queue = asyncio.Queue()
        print(f"   Audio queue created")

        # Audio buffer for accumulating microphone data
        audio_buffer = bytearray()
        AUDIO_CHUNK_SIZE = 320 * 2  # 320 samples * 2 bytes = 640 bytes (20ms at 16kHz)
        audio_chunk_count = 0

        try:
            # Start the ESP32 bot pipeline
            print(f"   Starting ESP32 bot pipeline...")
            asyncio.create_task(run_esp32_bot(session))

            total_messages = 0
            while True:
                # Receive message from ESP32
                message = await websocket.receive()
                total_messages += 1

                # Debug: log message types periodically
                if total_messages <= 3:
                    print(f"   📩 Message {total_messages}: type={message.get('type')}, keys={list(message.keys())}")

                if message["type"] == "websocket.disconnect":
                    break

                if "text" in message:
                    # JSON message from ESP32
                    try:
                        data = json.loads(message["text"])
                        msg_type = data.get("type")

                        if msg_type == "status":
                            # ESP32 status update (battery, etc.)
                            logger.debug(f"ESP32 status: {data}")

                        elif msg_type == "button":
                            # Button press event
                            print(f"🔘 ESP32 button: {data.get('event')}")
                            logger.info(f"ESP32 button: {data.get('event')}")

                    except json.JSONDecodeError:
                        logger.warning(f"ESP32: Invalid JSON received")

                elif "bytes" in message:
                    # Binary audio from ESP32 microphone
                    audio_data = message["bytes"]
                    audio_buffer.extend(audio_data)

                    # Debug: print first few binary messages
                    if audio_chunk_count < 5:
                        print(f"   📦 Binary data received: {len(audio_data)} bytes (buffer: {len(audio_buffer)})")

                    # Process complete chunks
                    while len(audio_buffer) >= AUDIO_CHUNK_SIZE:
                        chunk = bytes(audio_buffer[:AUDIO_CHUNK_SIZE])
                        del audio_buffer[:AUDIO_CHUNK_SIZE]

                        # Queue audio for STT processing
                        # This will be handled by the ESP32 bot pipeline
                        if hasattr(session, 'audio_queue'):
                            await session.audio_queue.put(chunk)
                            audio_chunk_count += 1
                            # Print every 50 chunks (~1 second of audio)
                            if audio_chunk_count % 50 == 0:
                                print(f"🎤 ESP32 audio: {audio_chunk_count} chunks received")

        except WebSocketDisconnect:
            print(f"\n🔌 ESP32 DISCONNECTED: {session_id}")
            logger.info(f"ESP32 disconnected: {session_id}")
        except Exception as e:
            print(f"\n❌ ESP32 ERROR: {e}")
            logger.error(f"ESP32 error: {e}")
        finally:
            session.connected = False
            if session_id in esp32_sessions:
                del esp32_sessions[session_id]
            if current_esp32_session == session:
                current_esp32_session = None
            print(f"   Cleaned up session {session_id}")
            logger.info(f"ESP32 session cleaned up: {session_id}")

    @app.post("/start")
    async def rtvi_start(request: Request):
        """Initialize a session (required by the prebuilt UI)."""
        try:
            request_data = await request.json()
            logger.debug(f"Received start request: {request_data}")
        except Exception:
            request_data = {}

        session_id = str(uuid.uuid4())
        active_sessions[session_id] = request_data

        result: StartBotResult = {"sessionId": session_id}
        if request_data.get("enableDefaultIceServers"):
            result["iceConfig"] = IceConfig(
                iceServers=[IceServer(urls=["stun:stun.l.google.com:19302"])]
            )

        return result

    @app.post("/api/offer")
    async def offer(request: SmallWebRTCRequest, background_tasks: BackgroundTasks):
        """Handle WebRTC offer requests."""

        async def webrtc_connection_callback(connection: SmallWebRTCConnection):
            background_tasks.add_task(run_bot, connection)

        answer = await small_webrtc_handler.handle_web_request(
            request=request,
            webrtc_connection_callback=webrtc_connection_callback,
        )
        return answer

    @app.patch("/api/offer")
    async def ice_candidate(request: SmallWebRTCPatchRequest):
        """Handle WebRTC ICE candidate requests."""
        await small_webrtc_handler.handle_patch_request(request)
        return {"status": "success"}

    @app.api_route(
        "/sessions/{session_id}/{path:path}",
        methods=["GET", "POST", "PUT", "PATCH", "DELETE"],
    )
    async def proxy_request(
        session_id: str, path: str, request: Request, background_tasks: BackgroundTasks
    ):
        """Proxy requests to session-specific endpoints."""
        active_session = active_sessions.get(session_id)
        if active_session is None:
            return Response(content="Invalid or not-yet-ready session_id", status_code=404)

        if path.endswith("api/offer"):
            try:
                request_data = await request.json()
                if request.method == "POST":
                    webrtc_request = SmallWebRTCRequest(
                        sdp=request_data["sdp"],
                        type=request_data["type"],
                        pc_id=request_data.get("pc_id"),
                        restart_pc=request_data.get("restart_pc"),
                        request_data=request_data.get("request_data")
                        or request_data.get("requestData"),
                    )
                    return await offer(webrtc_request, background_tasks)
                elif request.method == "PATCH":
                    patch_request = SmallWebRTCPatchRequest(
                        pc_id=request_data["pc_id"],
                        candidates=[IceCandidate(**c) for c in request_data.get("candidates", [])],
                    )
                    return await ice_candidate(patch_request)
            except Exception as e:
                logger.error(f"Failed to parse WebRTC request: {e}")
                return Response(content="Invalid WebRTC request", status_code=400)

        logger.info(f"Received request for path: {path}")
        return Response(status_code=200)

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        yield
        await small_webrtc_handler.close()

    app.router.lifespan_context = lifespan

    return app


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Pipecat Voice Bot")
    parser.add_argument("--host", default="0.0.0.0", help="Host (default: 0.0.0.0 for all interfaces)")
    parser.add_argument("--port", type=int, default=7860, help="Port (default: 7860)")
    args = parser.parse_args()

    # Get local IP for display
    import socket
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
    except Exception:
        local_ip = "localhost"

    print(f"\n🎙️  Luna Voice Bot starting at http://{args.host}:{args.port}")
    print("   Tools available: weather, web search, current time")
    print("   ")
    print(f"   Web client: http://{local_ip}:{args.port}/luna")
    print(f"   ESP32 WebSocket: ws://{local_ip}:{args.port}/luna-esp32")
    print("   ")
    print("   iOS App API (for notifications & control):")
    print(f"     POST http://{local_ip}:{args.port}/api/notify   - Receive notifications")
    print(f"     POST http://{local_ip}:{args.port}/api/control  - Remote control")
    print(f"     GET  http://{local_ip}:{args.port}/api/notify/status - Status")
    print(f"   API Key: {LUNA_NOTIFY_API_KEY[:8]}... (set LUNA_NOTIFY_API_KEY env var)")
    print("   ")
    print("   Open the web URL in your browser to start talking!\n")

    app = create_app()
    uvicorn.run(app, host=args.host, port=args.port)
