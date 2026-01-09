#!/usr/bin/env python3
"""
Luna Pi Standalone - Full voice assistant running on Raspberry Pi

Runs the complete Luna pipeline locally on the Pi:
- LLM: Anthropic Claude
- TTS: OpenAI
- STT: OpenAI Whisper
- Face: Rendered directly to ILI9341 framebuffer
- Audio: USB mic input, speaker output
- Camera: Optional Pi Camera for vision

No server required - everything runs on the Pi itself.

Requirements:
- ANTHROPIC_API_KEY and OPENAI_API_KEY in .env
- ILI9341 display connected via SPI
- USB microphone
- Speaker (3.5mm or USB)
"""

import argparse
import asyncio
import os
import sys
from pathlib import Path

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

import numpy as np
from dotenv import load_dotenv
from loguru import logger
from PIL import Image

from pipecat.audio.vad.silero import SileroVADAnalyzer
from pipecat.audio.vad.vad_analyzer import VADParams
from pipecat.frames.frames import Frame, OutputImageRawFrame, AudioRawFrame, StartFrame, EndFrame
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.runner import PipelineRunner
from pipecat.pipeline.task import PipelineParams, PipelineTask
from pipecat.processors.aggregators.llm_response_universal import LLMContextAggregatorPair
from pipecat.processors.frame_processor import FrameProcessor, FrameDirection
from pipecat.services.anthropic.llm import AnthropicLLMService
from pipecat.services.openai.stt import OpenAISTTService
from pipecat.services.openai.tts import OpenAITTSService
from pipecat.adapters.schemas.function_schema import FunctionSchema
from pipecat.adapters.schemas.tools_schema import ToolsSchema

from luna_face_renderer import LunaFaceRenderer, EmotionFrame

load_dotenv(override=True)


# ============== FRAMEBUFFER DISPLAY ==============

class FramebufferOutput(FrameProcessor):
    """Renders OutputImageRawFrame to Linux framebuffer (ILI9341 display)."""

    def __init__(self, device: str = "/dev/fb0", width: int = 240, height: int = 320):
        super().__init__()
        self.device = device
        self.width = width
        self.height = height
        self.fb = None
        self._frame_count = 0

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)

        if isinstance(frame, StartFrame):
            self._open_framebuffer()
            await self.push_frame(frame, direction)

        elif isinstance(frame, OutputImageRawFrame):
            self._render_frame(frame)
            # Don't push video frames downstream (they go to display only)

        elif isinstance(frame, EndFrame):
            self._close_framebuffer()
            await self.push_frame(frame, direction)

        else:
            await self.push_frame(frame, direction)

    def _open_framebuffer(self):
        """Open the framebuffer device."""
        try:
            self.fb = open(self.device, 'wb')
            logger.info(f"Opened framebuffer: {self.device}")
        except FileNotFoundError:
            logger.warning(f"Framebuffer {self.device} not found - video output disabled")
            self.fb = None
        except PermissionError:
            logger.error(f"Permission denied for {self.device}. Run with sudo or add user to video group.")
            self.fb = None

    def _render_frame(self, frame: OutputImageRawFrame):
        """Render a video frame to the framebuffer."""
        if self.fb is None:
            return

        try:
            # Convert raw frame to PIL Image
            img = Image.frombytes('RGB', (frame.size[0], frame.size[1]), frame.image)

            # Resize if needed
            if img.size != (self.width, self.height):
                img = img.resize((self.width, self.height), Image.Resampling.NEAREST)

            # Convert to RGB565 (16-bit color for ILI9341)
            pixels = np.array(img)
            r = (pixels[:, :, 0] >> 3).astype(np.uint16)
            g = (pixels[:, :, 1] >> 2).astype(np.uint16)
            b = (pixels[:, :, 2] >> 3).astype(np.uint16)
            rgb565 = (r << 11) | (g << 5) | b

            # Write to framebuffer
            self.fb.seek(0)
            self.fb.write(rgb565.tobytes())
            self.fb.flush()

            self._frame_count += 1
            if self._frame_count % 150 == 0:  # Log every ~10 seconds at 15fps
                logger.debug(f"Rendered {self._frame_count} frames to display")

        except Exception as e:
            logger.warning(f"Frame render error: {e}")

    def _close_framebuffer(self):
        """Close the framebuffer device."""
        if self.fb:
            # Clear to black
            black = np.zeros((self.height, self.width), dtype=np.uint16)
            self.fb.seek(0)
            self.fb.write(black.tobytes())
            self.fb.close()
            self.fb = None
            logger.info("Closed framebuffer")


# ============== AUDIO I/O ==============

class PyAudioInput(FrameProcessor):
    """Captures audio from microphone using PyAudio."""

    def __init__(self, sample_rate: int = 16000, channels: int = 1, chunk_ms: int = 20):
        super().__init__()
        self.sample_rate = sample_rate
        self.channels = channels
        self.chunk_size = int(sample_rate * chunk_ms / 1000)
        self.pyaudio = None
        self.stream = None
        self._running = False
        self._task = None

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)

        if isinstance(frame, StartFrame):
            await self._start_capture()
            await self.push_frame(frame, direction)

        elif isinstance(frame, EndFrame):
            await self._stop_capture()
            await self.push_frame(frame, direction)

        else:
            await self.push_frame(frame, direction)

    async def _start_capture(self):
        """Start audio capture."""
        try:
            import pyaudio
            self.pyaudio = pyaudio.PyAudio()

            # Find USB microphone
            device_index = None
            for i in range(self.pyaudio.get_device_count()):
                dev = self.pyaudio.get_device_info_by_index(i)
                if dev['maxInputChannels'] > 0:
                    logger.info(f"Found input device [{i}]: {dev['name']}")
                    if 'usb' in dev['name'].lower() or device_index is None:
                        device_index = i

            self.stream = self.pyaudio.open(
                format=pyaudio.paInt16,
                channels=self.channels,
                rate=self.sample_rate,
                input=True,
                input_device_index=device_index,
                frames_per_buffer=self.chunk_size,
            )
            logger.info(f"Microphone started (device {device_index})")

            self._running = True
            self._task = asyncio.create_task(self._capture_loop())

        except Exception as e:
            logger.error(f"Failed to start microphone: {e}")

    async def _stop_capture(self):
        """Stop audio capture."""
        self._running = False
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass

        if self.stream:
            self.stream.stop_stream()
            self.stream.close()
        if self.pyaudio:
            self.pyaudio.terminate()

        logger.info("Microphone stopped")

    async def _capture_loop(self):
        """Continuously capture audio and push frames."""
        while self._running:
            try:
                data = self.stream.read(self.chunk_size, exception_on_overflow=False)
                audio_frame = AudioRawFrame(
                    audio=data,
                    sample_rate=self.sample_rate,
                    num_channels=self.channels,
                )
                await self.push_frame(audio_frame)
                await asyncio.sleep(0.001)  # Yield to event loop

            except Exception as e:
                if self._running:
                    logger.warning(f"Audio capture error: {e}")
                await asyncio.sleep(0.1)


class PyAudioOutput(FrameProcessor):
    """Plays audio through speaker using PyAudio."""

    def __init__(self, sample_rate: int = 24000, channels: int = 1):
        super().__init__()
        self.sample_rate = sample_rate
        self.channels = channels
        self.pyaudio = None
        self.stream = None

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)

        if isinstance(frame, StartFrame):
            self._start_playback()
            await self.push_frame(frame, direction)

        elif isinstance(frame, AudioRawFrame):
            self._play_audio(frame)
            await self.push_frame(frame, direction)

        elif isinstance(frame, EndFrame):
            self._stop_playback()
            await self.push_frame(frame, direction)

        else:
            await self.push_frame(frame, direction)

    def _start_playback(self):
        """Start audio playback."""
        try:
            import pyaudio
            self.pyaudio = pyaudio.PyAudio()

            self.stream = self.pyaudio.open(
                format=pyaudio.paInt16,
                channels=self.channels,
                rate=self.sample_rate,
                output=True,
            )
            logger.info("Speaker started")

        except Exception as e:
            logger.error(f"Failed to start speaker: {e}")

    def _play_audio(self, frame: AudioRawFrame):
        """Play an audio frame."""
        if self.stream:
            try:
                self.stream.write(frame.audio)
            except Exception as e:
                logger.warning(f"Audio playback error: {e}")

    def _stop_playback(self):
        """Stop audio playback."""
        if self.stream:
            self.stream.stop_stream()
            self.stream.close()
        if self.pyaudio:
            self.pyaudio.terminate()
        logger.info("Speaker stopped")


# ============== TOOLS ==============

VALID_EMOTIONS = ["neutral", "happy", "sad", "angry", "surprised", "thinking", "confused", "excited", "cat"]

# Global reference to face renderer for tools
face_renderer = None


async def set_emotion(emotion: str):
    """Set Luna's facial emotion."""
    global face_renderer
    if emotion not in VALID_EMOTIONS:
        return {"status": "error", "message": f"Invalid emotion. Valid: {VALID_EMOTIONS}"}
    if face_renderer:
        face_renderer.set_emotion(emotion)
    return {"status": "success", "emotion": emotion}


async def get_current_time(timezone: str = None):
    """Get the current time."""
    from datetime import datetime
    import pytz

    try:
        if timezone:
            tz = pytz.timezone(timezone)
            now = datetime.now(tz)
        else:
            now = datetime.now()
        return {
            "time": now.strftime("%I:%M %p"),
            "date": now.strftime("%A, %B %d, %Y"),
            "timezone": timezone or "local"
        }
    except Exception as e:
        return {"error": str(e)}


emotion_tool = FunctionSchema(
    name="set_emotion",
    description="Set your facial expression/emotion",
    properties={
        "emotion": {
            "type": "string",
            "description": f"The emotion to display. Options: {', '.join(VALID_EMOTIONS)}",
            "enum": VALID_EMOTIONS,
        }
    },
    required=["emotion"],
)

time_tool = FunctionSchema(
    name="get_current_time",
    description="Get the current time and date",
    properties={
        "timezone": {
            "type": "string",
            "description": "Optional timezone (e.g., 'America/New_York', 'Europe/London')",
        }
    },
    required=[],
)

tools = ToolsSchema(standard_tools=[emotion_tool, time_tool])


# ============== MAIN ==============

async def run_luna(display_device: str = "/dev/fb0", input_device: int = None):
    """Run the Luna voice assistant."""
    global face_renderer

    logger.info("Starting Luna Pi Standalone")

    # Check API keys
    if not os.getenv("ANTHROPIC_API_KEY"):
        logger.error("ANTHROPIC_API_KEY not set in environment")
        return
    if not os.getenv("OPENAI_API_KEY"):
        logger.error("OPENAI_API_KEY not set in environment")
        return

    # Create components
    audio_input = PyAudioInput(sample_rate=16000)
    audio_output = PyAudioOutput(sample_rate=24000)
    framebuffer = FramebufferOutput(device=display_device)

    # Face renderer (240x320 portrait, 15 FPS)
    face_renderer = LunaFaceRenderer(width=240, height=320, fps=15)

    # VAD for detecting speech
    vad = SileroVADAnalyzer(params=VADParams(
        stop_secs=0.5,
        min_volume=0.6,
    ))

    # STT: OpenAI Whisper
    stt = OpenAISTTService(
        api_key=os.getenv("OPENAI_API_KEY"),
        model="whisper-1",
    )

    # TTS: OpenAI
    tts = OpenAITTSService(
        api_key=os.getenv("OPENAI_API_KEY"),
        voice="nova",
    )

    # LLM: Anthropic Claude
    llm = AnthropicLLMService(
        api_key=os.getenv("ANTHROPIC_API_KEY"),
        model="claude-3-5-haiku-latest",
    )

    # Register tools
    llm.register_function("set_emotion", set_emotion)
    llm.register_function("get_current_time", get_current_time)

    # System prompt
    messages = [
        {
            "role": "system",
            "content": """Your name is Luna. You are a friendly, helpful voice assistant with an animated robot face displayed on a small screen.

EMOTIONS - Use set_emotion frequently:
- "happy" for greetings, good news, positive interactions
- "thinking" when processing or considering something
- "excited" for exciting discoveries or news
- "surprised" for unexpected information
- "neutral" for factual responses
- "sad" for unfortunate news
- "confused" when you need clarification
- Call set_emotion BEFORE speaking your response

RULES:
- Keep responses SHORT - 1-2 sentences max (this is voice, not text)
- Be warm, friendly, and conversational
- No special characters, emojis, or markdown
- Speak naturally as if talking to a friend

You have access to: set_emotion (change your face), get_current_time (check time/date)

Be helpful but brief. Your name is Luna."""
        }
    ]

    # Context aggregator for conversation
    context = LLMContextAggregatorPair(
        messages=messages,
        tools=tools,
        vad_analyzer=vad,
    )

    # Build pipeline
    pipeline = Pipeline([
        audio_input,           # Mic → audio frames
        context.user(),        # Aggregate user speech
        stt,                   # Speech → text
        llm,                   # Text → response
        tts,                   # Response → speech
        face_renderer,         # Generate face video
        framebuffer,           # Video → display
        audio_output,          # Audio → speaker
        context.assistant(),   # Track assistant responses
    ])

    # Create and run task
    task = PipelineTask(
        pipeline,
        params=PipelineParams(
            allow_interruptions=True,
            enable_metrics=True,
        ),
    )

    # Set initial emotion
    face_renderer.set_emotion("happy")

    logger.info("Luna is ready! Say something...")

    runner = PipelineRunner()
    await runner.run(task)


def main():
    parser = argparse.ArgumentParser(description="Luna Pi Standalone Voice Assistant")
    parser.add_argument("--display", "-d", default="/dev/fb0",
                        help="Framebuffer device (default: /dev/fb0)")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Enable debug logging")
    args = parser.parse_args()

    if args.verbose:
        logger.remove()
        logger.add(sys.stderr, level="DEBUG")
    else:
        logger.remove()
        logger.add(sys.stderr, level="INFO")

    try:
        asyncio.run(run_luna(display_device=args.display))
    except KeyboardInterrupt:
        logger.info("Shutting down...")


if __name__ == "__main__":
    main()
