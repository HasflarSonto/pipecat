#
# Voice bot using Anthropic LLM + OpenAI TTS/STT
# With Small WebRTC transport and tool calling (web search, weather)
#

import argparse
import asyncio
import os
import uuid
from contextlib import asynccontextmanager
from typing import Any, Dict, List, Optional, TypedDict, Union

import aiohttp
import uvicorn
from dotenv import load_dotenv
from fastapi import BackgroundTasks, FastAPI, Request, Response
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import RedirectResponse
from fastapi.staticfiles import StaticFiles
from loguru import logger
from pipecat_ai_small_webrtc_prebuilt.frontend import SmallWebRTCPrebuiltUI

from pipecat.adapters.schemas.function_schema import FunctionSchema
from pipecat.adapters.schemas.tools_schema import ToolsSchema
from pipecat.audio.vad.silero import SileroVADAnalyzer
from pipecat.audio.vad.vad_analyzer import VADParams
from pipecat.frames.frames import LLMRunFrame
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

load_dotenv(override=True)

# Initialize the request handler
small_webrtc_handler = SmallWebRTCRequestHandler()


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


# Global references for emotion updates
current_task = None
face_renderer = None

VALID_EMOTIONS = ["neutral", "happy", "sad", "angry", "surprised", "thinking", "confused", "excited", "cat"]

async def set_emotion(params: FunctionCallParams):
    """Set Luna's facial emotion."""
    global current_task, face_renderer
    emotion = params.arguments.get("emotion", "neutral").lower()
    logger.info(f"Setting emotion to: {emotion}")

    if emotion not in VALID_EMOTIONS:
        await params.result_callback(f"Unknown emotion. Valid emotions are: {', '.join(VALID_EMOTIONS)}")
        return

    # Update the face renderer
    if face_renderer:
        face_renderer.set_emotion(emotion)

    # Also send emotion update to frontend via RTVI (for any client-side UI)
    if current_task:
        emotion_frame = RTVIServerMessageFrame(data={
            "type": "emotion",
            "emotion": emotion
        })
        await current_task.queue_frames([emotion_frame])

    await params.result_callback(f"Emotion set to {emotion}")


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

tools = ToolsSchema(standard_tools=[weather_tool, search_tool, time_tool, emotion_tool])


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
            vad_analyzer=SileroVADAnalyzer(params=VADParams(stop_secs=0.2)),
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
        model="claude-3-5-haiku-latest",
    )

    # Register tool handlers
    llm.register_function("get_weather", get_weather)
    llm.register_function("web_search", web_search)
    llm.register_function("get_current_time", get_current_time)
    llm.register_function("set_emotion", set_emotion)

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

Tools: get_weather, web_search, get_current_time, set_emotion

Be warm but brief. Your name is Luna.""",
        },
    ]

    # Context aggregator manages conversation history - pass tools to context
    context = LLMContext(messages, tools)
    context_aggregator = LLMContextAggregatorPair(context)

    # RTVI processor for UI communication (enables text display in prebuilt UI)
    rtvi = RTVIProcessor(config=RTVIConfig(config=[]))

    # Build the pipeline with RTVI processor and face renderer
    pipeline = Pipeline(
        [
            transport.input(),
            rtvi,
            stt,
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
        """Handle incoming app messages (like gaze data)."""
        try:
            data = message if isinstance(message, dict) else {}
            if data.get("type") == "gaze":
                # Update face renderer gaze
                x = data.get("x", 0.5)
                y = data.get("y", 0.5)
                face_renderer.set_gaze(x, y)
        except Exception as e:
            pass  # Ignore errors

    @transport.event_handler("on_client_disconnected")
    async def on_client_disconnected(transport, client):
        logger.info("Client disconnected")
        await task.cancel()

    runner = PipelineRunner(handle_sigint=False)
    await runner.run(task)


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
    parser.add_argument("--host", default="localhost", help="Host (default: localhost)")
    parser.add_argument("--port", type=int, default=7860, help="Port (default: 7860)")
    args = parser.parse_args()

    print(f"\n🎙️  Voice Bot starting at http://{args.host}:{args.port}")
    print("   Tools available: weather, web search, current time")
    print("   Open this URL in your browser to start talking!\n")

    app = create_app()
    uvicorn.run(app, host=args.host, port=args.port)
