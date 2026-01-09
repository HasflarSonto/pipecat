#!/usr/bin/env python3
"""
Luna Pi Debug - Web-based debug interface for Luna on Raspberry Pi

Runs Luna with a web interface at http://PI_IP:7860 showing:
- Real-time status
- Audio levels
- Transcriptions
- Pipeline state
- Live camera with face/hand detection
"""

import argparse
import asyncio
import os
import sys
import time
from pathlib import Path
from collections import deque
from datetime import datetime

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

import numpy as np
from dotenv import load_dotenv
from loguru import logger
from PIL import Image, ImageDraw
import io
import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, StreamingResponse
import threading

# Optional: OpenCV for camera
try:
    import cv2
    CV2_AVAILABLE = True
except ImportError:
    CV2_AVAILABLE = False
    logger.warning("OpenCV not available - camera disabled")

# Optional: MediaPipe for detection (may not be available on Pi)
try:
    import mediapipe as mp
    MP_AVAILABLE = True
except ImportError:
    MP_AVAILABLE = False
    logger.info("MediaPipe not available - using OpenCV cascade detection")

# OpenCV Haar cascade for face detection (fallback when no MediaPipe)
HAAR_FACE_CASCADE = None
if CV2_AVAILABLE and not MP_AVAILABLE:
    try:
        cascade_path = cv2.data.haarcascades + 'haarcascade_frontalface_default.xml'
        HAAR_FACE_CASCADE = cv2.CascadeClassifier(cascade_path)
        logger.info("Using OpenCV Haar cascade for face detection")
    except Exception as e:
        logger.warning(f"Could not load Haar cascade: {e}")

from pipecat.audio.vad.silero import SileroVADAnalyzer
from pipecat.audio.vad.vad_analyzer import VADParams
from pipecat.frames.frames import (
    Frame, OutputImageRawFrame, AudioRawFrame, InputAudioRawFrame,
    OutputAudioRawFrame, StartFrame, EndFrame, TranscriptionFrame,
    TextFrame, VADUserStartedSpeakingFrame, VADUserStoppedSpeakingFrame
)
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.runner import PipelineRunner
from pipecat.pipeline.task import PipelineParams, PipelineTask
from pipecat.processors.aggregators.llm_context import LLMContext
from pipecat.processors.aggregators.llm_response_universal import LLMContextAggregatorPair
from pipecat.processors.frame_processor import FrameProcessor, FrameDirection
from pipecat.services.anthropic.llm import AnthropicLLMService
from pipecat.services.openai.stt import OpenAISTTService
from pipecat.services.openai.tts import OpenAITTSService
from pipecat.adapters.schemas.function_schema import FunctionSchema
from pipecat.adapters.schemas.tools_schema import ToolsSchema

from luna_face_renderer import LunaFaceRenderer

load_dotenv(override=True)

# Global state for web interface
debug_state = {
    "status": "initializing",
    "mic_level": 0,
    "speaker_active": False,
    "vad_speaking": False,  # VAD detected speech
    "last_transcription": "",
    "last_response": "",
    "frame_count": 0,
    "errors": deque(maxlen=10),
    "logs": deque(maxlen=50),
    "started_at": None,
    "last_frame_jpeg": None,  # Latest face frame as JPEG bytes
    "last_camera_jpeg": None,  # Latest camera frame with detection as JPEG
    "camera_enabled": False,
    "face_detected": False,
    "hand_detected": False,
}


def log(msg):
    """Log message to both console and debug state."""
    timestamp = datetime.now().strftime("%H:%M:%S")
    debug_state["logs"].append(f"[{timestamp}] {msg}")
    logger.info(msg)


# ============== CAMERA WITH DETECTION ==============

class CameraCapture:
    """Captures camera frames and runs face/hand detection."""

    def __init__(self, camera_index: int = 0, width: int = 320, height: int = 240):
        self.camera_index = camera_index
        self.width = width
        self.height = height
        self.cap = None
        self.running = False
        self.thread = None

        # MediaPipe detectors
        self.face_detection = None
        self.hand_detection = None

        if MP_AVAILABLE:
            self.mp_face = mp.solutions.face_detection
            self.mp_hands = mp.solutions.hands
            self.mp_draw = mp.solutions.drawing_utils

    def start(self):
        """Start camera capture in background thread."""
        if not CV2_AVAILABLE:
            log("Camera disabled - OpenCV not installed")
            return False

        try:
            self.cap = cv2.VideoCapture(self.camera_index)
            self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
            self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)
            self.cap.set(cv2.CAP_PROP_FPS, 15)

            if not self.cap.isOpened():
                log(f"Failed to open camera {self.camera_index}")
                return False

            if MP_AVAILABLE:
                self.face_detection = self.mp_face.FaceDetection(
                    model_selection=0, min_detection_confidence=0.5
                )
                self.hand_detection = self.mp_hands.Hands(
                    static_image_mode=False,
                    max_num_hands=2,
                    min_detection_confidence=0.5,
                    min_tracking_confidence=0.5
                )

            self.running = True
            self.thread = threading.Thread(target=self._capture_loop, daemon=True)
            self.thread.start()
            debug_state["camera_enabled"] = True
            log(f"Camera started (index {self.camera_index})")
            return True

        except Exception as e:
            log(f"Camera error: {e}")
            debug_state["errors"].append(f"Camera: {e}")
            return False

    def _capture_loop(self):
        """Background thread for camera capture."""
        while self.running:
            try:
                ret, frame = self.cap.read()
                if not ret:
                    time.sleep(0.1)
                    continue

                # Convert BGR to RGB for detection
                rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

                face_detected = False
                hand_detected = False

                if MP_AVAILABLE and self.face_detection and self.hand_detection:
                    # Use MediaPipe for detection
                    face_results = self.face_detection.process(rgb_frame)
                    if face_results.detections:
                        face_detected = True
                        for detection in face_results.detections:
                            self.mp_draw.draw_detection(frame, detection)

                    hand_results = self.hand_detection.process(rgb_frame)
                    if hand_results.multi_hand_landmarks:
                        hand_detected = True
                        for hand_landmarks in hand_results.multi_hand_landmarks:
                            self.mp_draw.draw_landmarks(
                                frame, hand_landmarks, self.mp_hands.HAND_CONNECTIONS
                            )

                elif HAAR_FACE_CASCADE is not None:
                    # Fallback: Use OpenCV Haar cascade for face detection
                    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                    faces = HAAR_FACE_CASCADE.detectMultiScale(
                        gray, scaleFactor=1.1, minNeighbors=5, minSize=(30, 30)
                    )
                    if len(faces) > 0:
                        face_detected = True
                        for (x, y, w, h) in faces:
                            # Draw green rectangle around face
                            cv2.rectangle(frame, (x, y), (x+w, y+h), (0, 255, 0), 2)
                            # Draw label
                            cv2.putText(frame, "Face", (x, y-10),
                                       cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)

                debug_state["face_detected"] = face_detected
                debug_state["hand_detected"] = hand_detected

                # Encode as JPEG
                _, jpeg = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 80])
                debug_state["last_camera_jpeg"] = jpeg.tobytes()

                time.sleep(0.067)  # ~15 FPS

            except Exception as e:
                debug_state["errors"].append(f"Camera capture: {e}")
                time.sleep(0.5)

    def stop(self):
        """Stop camera capture."""
        self.running = False
        if self.thread:
            self.thread.join(timeout=1)
        if self.cap:
            self.cap.release()
        if self.face_detection:
            self.face_detection.close()
        if self.hand_detection:
            self.hand_detection.close()
        debug_state["camera_enabled"] = False
        log("Camera stopped")


# Global camera instance
camera = None


# ============== WEB SERVER ==============

app = FastAPI()


async def generate_face_mjpeg():
    """Generate MJPEG stream from Luna's face frames."""
    while True:
        if debug_state["last_frame_jpeg"]:
            yield (
                b"--frame\r\n"
                b"Content-Type: image/jpeg\r\n\r\n" +
                debug_state["last_frame_jpeg"] +
                b"\r\n"
            )
        await asyncio.sleep(0.067)  # ~15 FPS


async def generate_camera_mjpeg():
    """Generate MJPEG stream from camera with detection overlay."""
    while True:
        if debug_state["last_camera_jpeg"]:
            yield (
                b"--frame\r\n"
                b"Content-Type: image/jpeg\r\n\r\n" +
                debug_state["last_camera_jpeg"] +
                b"\r\n"
            )
        await asyncio.sleep(0.067)  # ~15 FPS


@app.get("/video")
async def video_feed():
    """MJPEG video stream for Luna's face."""
    return StreamingResponse(
        generate_face_mjpeg(),
        media_type="multipart/x-mixed-replace; boundary=frame"
    )


@app.get("/camera")
async def camera_feed():
    """MJPEG video stream for camera with face/hand detection."""
    return StreamingResponse(
        generate_camera_mjpeg(),
        media_type="multipart/x-mixed-replace; boundary=frame"
    )


@app.get("/", response_class=HTMLResponse)
async def index():
    logs_html = "<br>".join(list(debug_state["logs"])[-30:])
    errors_html = "<br>".join([f"<span style='color:red'>{e}</span>" for e in debug_state["errors"]])

    uptime = ""
    if debug_state["started_at"]:
        uptime = f"{int(time.time() - debug_state['started_at'])}s"

    camera_html = ""
    if debug_state["camera_enabled"]:
        camera_html = """
            <div class="video-container">
                <h2>Camera (Detection)</h2>
                <img src="/camera" width="320" height="240" alt="Camera Feed">
                <p><span id="detection-status" class="value"></span></p>
            </div>
        """
    else:
        camera_html = """
            <div class="video-container" style="opacity: 0.5;">
                <h2>Camera (Disabled)</h2>
                <div style="width: 320px; height: 240px; background: #333; border-radius: 8px; display: flex; align-items: center; justify-content: center;">
                    <span style="color: #888;">No camera</span>
                </div>
            </div>
        """

    return f"""
    <!DOCTYPE html>
    <html>
    <head>
        <title>Luna Pi Debug</title>
        <style>
            body {{ font-family: monospace; background: #1a1a2e; color: #eee; padding: 20px; }}
            .status {{ font-size: 24px; margin-bottom: 20px; }}
            .status.ok {{ color: #4ade80; }}
            .status.error {{ color: #f87171; }}
            .box {{ background: #16213e; padding: 15px; margin: 10px 0; border-radius: 8px; }}
            .label {{ color: #888; }}
            .value {{ color: #4ade80; font-size: 18px; }}
            .logs {{ font-size: 12px; max-height: 200px; overflow-y: auto; }}
            h2 {{ color: #818cf8; margin-top: 0; }}
            .meter {{ width: 200px; height: 20px; background: #333; border-radius: 4px; overflow: hidden; }}
            .meter-fill {{ height: 100%; background: linear-gradient(90deg, #4ade80, #fbbf24); transition: width 0.2s; }}
            .video-row {{ display: flex; gap: 20px; flex-wrap: wrap; }}
            .video-container {{ text-align: center; }}
            .video-container img {{ border-radius: 8px; border: 2px solid #818cf8; }}
            .info-row {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 10px; }}
        </style>
        <script>
            setInterval(async () => {{
                const resp = await fetch('/status');
                const data = await resp.json();
                document.getElementById('status-text').textContent = data.status + (data.uptime ? ` (uptime: ${{data.uptime}}s)` : '');
                document.getElementById('status-text').className = 'status ' + (data.status === 'running' ? 'ok' : 'error');
                document.getElementById('mic-meter').style.width = Math.min(data.mic_level * 100, 100) + '%';
                document.getElementById('vad-status').innerHTML = data.vad_speaking ? '🎤 SPEAKING' : '⏸️ Waiting...';
                document.getElementById('vad-status').style.color = data.vad_speaking ? '#4ade80' : '#888';
                document.getElementById('speaker-status').innerHTML = data.speaker_active ? '🔊 Playing' : '🔇 Silent';
                document.getElementById('last-heard').textContent = data.last_transcription || '(nothing yet)';
                document.getElementById('last-response').textContent = data.last_response || '(nothing yet)';
                document.getElementById('frame-count').textContent = data.frame_count;
                document.getElementById('logs').innerHTML = data.logs.join('<br>');
                // Detection status
                const det = document.getElementById('detection-status');
                if (det) {{
                    let status = [];
                    if (data.face_detected) status.push('👤 Face');
                    if (data.hand_detected) status.push('✋ Hand');
                    det.textContent = status.length ? status.join(' | ') : 'No detection';
                    det.style.color = status.length ? '#4ade80' : '#888';
                }}
            }}, 500);
        </script>
    </head>
    <body>
        <h1>🤖 Luna Pi Debug</h1>

        <div id="status-text" class="status {'ok' if debug_state['status'] == 'running' else 'error'}">
            {debug_state['status']} {f"(uptime: {uptime})" if uptime else ''}
        </div>

        <div class="video-row">
            <div class="video-container">
                <h2>Luna's Face</h2>
                <img src="/video" width="240" height="320" alt="Luna Face">
            </div>
            {camera_html}
        </div>

        <div class="info-row">
            <div class="box">
                <h2>Audio</h2>
                <p><span class="label">Mic Level:</span></p>
                <div class="meter"><div id="mic-meter" class="meter-fill" style="width: {min(debug_state['mic_level'] * 100, 100)}%"></div></div>
                <p><span class="label">VAD:</span> <span id="vad-status" class="value" style="color: {'#4ade80' if debug_state['vad_speaking'] else '#888'}">{'🎤 SPEAKING' if debug_state['vad_speaking'] else '⏸️ Waiting...'}</span></p>
                <p><span class="label">Speaker:</span> <span id="speaker-status" class="value">{'🔊 Playing' if debug_state['speaker_active'] else '🔇 Silent'}</span></p>
            </div>

            <div class="box">
                <h2>Conversation</h2>
                <p><span class="label">Last heard:</span> <span id="last-heard" class="value">{debug_state['last_transcription'] or '(nothing yet)'}</span></p>
                <p><span class="label">Last response:</span> <span id="last-response" class="value">{debug_state['last_response'] or '(nothing yet)'}</span></p>
            </div>

            <div class="box">
                <h2>Pipeline</h2>
                <p><span class="label">Video frames:</span> <span id="frame-count" class="value">{debug_state['frame_count']}</span></p>
            </div>
        </div>

        {f'<div class="box"><h2>Errors</h2>{errors_html}</div>' if debug_state['errors'] else ''}

        <div class="box">
            <h2>Logs</h2>
            <div id="logs" class="logs">{logs_html}</div>
        </div>
    </body>
    </html>
    """


@app.get("/status")
async def get_status():
    """JSON status endpoint for AJAX updates."""
    uptime = int(time.time() - debug_state['started_at']) if debug_state['started_at'] else 0
    return {
        "status": debug_state["status"],
        "uptime": uptime,
        "mic_level": debug_state["mic_level"],
        "vad_speaking": debug_state["vad_speaking"],
        "speaker_active": debug_state["speaker_active"],
        "last_transcription": debug_state["last_transcription"],
        "last_response": debug_state["last_response"],
        "frame_count": debug_state["frame_count"],
        "logs": list(debug_state["logs"])[-30:],
        "camera_enabled": debug_state["camera_enabled"],
        "face_detected": debug_state["face_detected"],
        "hand_detected": debug_state["hand_detected"],
    }


# ============== FRAMEBUFFER DISPLAY ==============

class FramebufferOutput(FrameProcessor):
    """Renders OutputImageRawFrame to Linux framebuffer."""

    def __init__(self, device: str = "/dev/fb0", width: int = 240, height: int = 320):
        super().__init__()
        self.device = device
        self.width = width
        self.height = height
        self.fb = None

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)

        if isinstance(frame, StartFrame):
            self._open_framebuffer()
            await self.push_frame(frame, direction)
        elif isinstance(frame, OutputImageRawFrame):
            self._render_frame(frame)
        elif isinstance(frame, EndFrame):
            self._close_framebuffer()
            await self.push_frame(frame, direction)
        else:
            await self.push_frame(frame, direction)

    def _open_framebuffer(self):
        try:
            self.fb = open(self.device, 'wb')
            log(f"Framebuffer opened: {self.device}")
        except Exception as e:
            log(f"Framebuffer error: {e}")
            debug_state["errors"].append(f"Framebuffer: {e}")
            self.fb = None

    def _render_frame(self, frame: OutputImageRawFrame):
        try:
            img = Image.frombytes('RGB', (frame.size[0], frame.size[1]), frame.image)
            if img.size != (self.width, self.height):
                img = img.resize((self.width, self.height), Image.Resampling.NEAREST)

            # Save JPEG for web streaming
            jpeg_buffer = io.BytesIO()
            img.save(jpeg_buffer, format='JPEG', quality=80)
            debug_state["last_frame_jpeg"] = jpeg_buffer.getvalue()

            # Render to framebuffer if available
            if self.fb is not None:
                pixels = np.array(img)
                r = (pixels[:, :, 0] >> 3).astype(np.uint16)
                g = (pixels[:, :, 1] >> 2).astype(np.uint16)
                b = (pixels[:, :, 2] >> 3).astype(np.uint16)
                rgb565 = (r << 11) | (g << 5) | b
                self.fb.seek(0)
                self.fb.write(rgb565.tobytes())
                self.fb.flush()

            debug_state["frame_count"] += 1
        except Exception as e:
            debug_state["errors"].append(f"Render: {e}")

    def _close_framebuffer(self):
        if self.fb:
            self.fb.close()
            log("Framebuffer closed")


# ============== AUDIO I/O ==============

class PyAudioInput(FrameProcessor):
    """Captures audio from microphone."""

    def __init__(self, sample_rate: int = 44100, output_sample_rate: int = 16000, channels: int = 1, device_index: int = None):
        super().__init__()
        self.sample_rate = sample_rate
        self.output_sample_rate = output_sample_rate
        self.channels = channels
        self.chunk_size = int(sample_rate * 20 / 1000)  # 20ms chunks
        self.device_index = device_index
        self.pyaudio = None
        self.stream = None
        self._running = False
        self._task = None
        self._need_resample = (sample_rate != output_sample_rate)

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
        try:
            import pyaudio
            self.pyaudio = pyaudio.PyAudio()

            # Find USB mic
            device_index = self.device_index
            if device_index is None:
                for i in range(self.pyaudio.get_device_count()):
                    dev = self.pyaudio.get_device_info_by_index(i)
                    if dev['maxInputChannels'] > 0:
                        name = dev['name'].lower()
                        log(f"Input device [{i}]: {dev['name']}")
                        if 'usb' in name or 'pnp' in name:
                            device_index = i
                            break
                        elif device_index is None:
                            device_index = i

            self.stream = self.pyaudio.open(
                format=pyaudio.paInt16,
                channels=self.channels,
                rate=self.sample_rate,
                input=True,
                input_device_index=device_index,
                frames_per_buffer=self.chunk_size,
            )
            log(f"Mic started: device {device_index}, {self.sample_rate}Hz")
            debug_state["status"] = "running"

            self._running = True
            self._task = asyncio.create_task(self._capture_loop())

        except Exception as e:
            log(f"Mic error: {e}")
            debug_state["errors"].append(f"Mic: {e}")
            debug_state["status"] = "mic_error"

    async def _stop_capture(self):
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
        log("Mic stopped")

    def _resample(self, data: bytes, from_rate: int, to_rate: int) -> bytes:
        audio = np.frombuffer(data, dtype=np.int16)
        duration = len(audio) / from_rate
        new_length = int(duration * to_rate)
        indices = np.linspace(0, len(audio) - 1, new_length)
        resampled = np.interp(indices, np.arange(len(audio)), audio).astype(np.int16)
        return resampled.tobytes()

    async def _capture_loop(self):
        while self._running:
            try:
                data = self.stream.read(self.chunk_size, exception_on_overflow=False)

                # Update mic level for debug display
                audio = np.frombuffer(data, dtype=np.int16)
                debug_state["mic_level"] = np.abs(audio).mean() / 32768

                if self._need_resample:
                    data = self._resample(data, self.sample_rate, self.output_sample_rate)

                audio_frame = InputAudioRawFrame(
                    audio=data,
                    sample_rate=self.output_sample_rate,
                    num_channels=self.channels,
                )
                await self.push_frame(audio_frame)
                await asyncio.sleep(0.001)

            except Exception as e:
                if self._running:
                    debug_state["errors"].append(f"Capture: {e}")
                await asyncio.sleep(0.1)


class PyAudioOutput(FrameProcessor):
    """Plays audio through speaker."""

    def __init__(self, sample_rate: int = 48000, channels: int = 2, device_index: int = None):
        super().__init__()
        self.sample_rate = sample_rate
        self.channels = channels
        self.device_index = device_index
        self.pyaudio = None
        self.stream = None

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)
        if isinstance(frame, StartFrame):
            self._start_playback()
            await self.push_frame(frame, direction)
        elif isinstance(frame, OutputAudioRawFrame):
            self._play_audio(frame)
            await self.push_frame(frame, direction)
        elif isinstance(frame, EndFrame):
            self._stop_playback()
            await self.push_frame(frame, direction)
        else:
            await self.push_frame(frame, direction)

    def _start_playback(self):
        try:
            import pyaudio
            self.pyaudio = pyaudio.PyAudio()

            device_index = self.device_index
            if device_index is None:
                for i in range(self.pyaudio.get_device_count()):
                    dev = self.pyaudio.get_device_info_by_index(i)
                    if dev['maxOutputChannels'] > 0:
                        name = dev['name'].lower()
                        log(f"Output device [{i}]: {dev['name']}")
                        if 'usb' in name or 'uac' in name:
                            device_index = i
                            break
                        elif device_index is None:
                            device_index = i

            self.stream = self.pyaudio.open(
                format=pyaudio.paInt16,
                channels=self.channels,
                rate=self.sample_rate,
                output=True,
                output_device_index=device_index,
            )
            log(f"Speaker started: device {device_index}, {self.sample_rate}Hz, {self.channels}ch")

        except Exception as e:
            log(f"Speaker error: {e}")
            debug_state["errors"].append(f"Speaker: {e}")

    def _resample(self, data: bytes, from_rate: int, to_rate: int) -> bytes:
        audio = np.frombuffer(data, dtype=np.int16)
        duration = len(audio) / from_rate
        new_length = int(duration * to_rate)
        indices = np.linspace(0, len(audio) - 1, new_length)
        resampled = np.interp(indices, np.arange(len(audio)), audio).astype(np.int16)
        return resampled.tobytes()

    def _play_audio(self, frame: OutputAudioRawFrame):
        if self.stream:
            try:
                debug_state["speaker_active"] = True
                audio_data = frame.audio
                input_rate = frame.sample_rate

                if input_rate != self.sample_rate:
                    audio_data = self._resample(audio_data, input_rate, self.sample_rate)

                if frame.num_channels == 1 and self.channels == 2:
                    mono = np.frombuffer(audio_data, dtype=np.int16)
                    stereo = np.column_stack((mono, mono)).flatten()
                    audio_data = stereo.tobytes()

                self.stream.write(audio_data)
            except Exception as e:
                debug_state["errors"].append(f"Playback: {e}")
            finally:
                debug_state["speaker_active"] = False

    def _stop_playback(self):
        if self.stream:
            self.stream.stop_stream()
            self.stream.close()
        if self.pyaudio:
            self.pyaudio.terminate()
        log("Speaker stopped")


# ============== VAD PROCESSOR ==============

class VADProcessor(FrameProcessor):
    """Wraps VADAnalyzer to emit VAD frames in the pipeline.

    The SileroVADAnalyzer is not a FrameProcessor - it's normally used by
    the transport. This processor wraps it for standalone use.
    """

    def __init__(self, vad_analyzer: "SileroVADAnalyzer", sample_rate: int = 16000):
        super().__init__()
        self._vad = vad_analyzer
        self._sample_rate = sample_rate
        self._vad_state = None
        self._initialized = False

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)

        if isinstance(frame, StartFrame):
            # Initialize VAD with sample rate
            self._vad.set_sample_rate(self._sample_rate)
            self._initialized = True
            log(f"VAD initialized at {self._sample_rate}Hz")
            await self.push_frame(frame, direction)

        elif isinstance(frame, InputAudioRawFrame):
            if self._initialized:
                # Run VAD analysis
                from pipecat.audio.vad.vad_analyzer import VADState
                new_state = await self._vad.analyze_audio(frame.audio)

                # Emit VAD frames on state transitions
                if new_state != self._vad_state:
                    if new_state == VADState.SPEAKING and self._vad_state != VADState.STARTING:
                        await self.push_frame(VADUserStartedSpeakingFrame())
                    elif new_state == VADState.QUIET and self._vad_state == VADState.STOPPING:
                        await self.push_frame(VADUserStoppedSpeakingFrame())
                    self._vad_state = new_state

            # Always pass audio through
            await self.push_frame(frame, direction)

        else:
            await self.push_frame(frame, direction)


# ============== DEBUG MONITOR ==============

class DebugMonitor(FrameProcessor):
    """Monitors frames for debug display."""

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)

        if isinstance(frame, VADUserStartedSpeakingFrame):
            debug_state["vad_speaking"] = True
            log("VAD: Speech started")

        elif isinstance(frame, VADUserStoppedSpeakingFrame):
            debug_state["vad_speaking"] = False
            log("VAD: Speech stopped")

        elif isinstance(frame, TranscriptionFrame):
            debug_state["last_transcription"] = frame.text
            log(f"Heard: {frame.text}")

        elif isinstance(frame, TextFrame) and not isinstance(frame, TranscriptionFrame):
            # LLM response text
            if frame.text:
                debug_state["last_response"] = frame.text[:100]
                log(f"Response: {frame.text[:50]}...")

        await self.push_frame(frame, direction)


# ============== TOOLS ==============

VALID_EMOTIONS = ["neutral", "happy", "sad", "angry", "surprised", "thinking", "confused", "excited", "cat"]
face_renderer = None

async def set_emotion(emotion: str):
    global face_renderer
    if emotion not in VALID_EMOTIONS:
        return {"status": "error", "message": f"Invalid emotion"}
    if face_renderer:
        face_renderer.set_emotion(emotion)
        log(f"Emotion: {emotion}")
    return {"status": "success", "emotion": emotion}

async def get_current_time(timezone: str = None):
    from datetime import datetime
    now = datetime.now()
    return {"time": now.strftime("%I:%M %p"), "date": now.strftime("%A, %B %d, %Y")}

emotion_tool = FunctionSchema(
    name="set_emotion",
    description="Set your facial expression",
    properties={"emotion": {"type": "string", "enum": VALID_EMOTIONS}},
    required=["emotion"],
)

time_tool = FunctionSchema(
    name="get_current_time",
    description="Get current time",
    properties={},
    required=[],
)

tools = ToolsSchema(standard_tools=[emotion_tool, time_tool])


# ============== MAIN ==============

async def run_luna(display_device: str = "/dev/fb0"):
    global face_renderer

    log("Starting Luna Pi Debug")
    debug_state["started_at"] = time.time()

    if not os.getenv("ANTHROPIC_API_KEY"):
        debug_state["status"] = "error: no ANTHROPIC_API_KEY"
        debug_state["errors"].append("Missing ANTHROPIC_API_KEY")
        return
    if not os.getenv("OPENAI_API_KEY"):
        debug_state["status"] = "error: no OPENAI_API_KEY"
        debug_state["errors"].append("Missing OPENAI_API_KEY")
        return

    log("API keys found")

    # Components
    audio_input = PyAudioInput(sample_rate=44100, output_sample_rate=16000)
    audio_output = PyAudioOutput(sample_rate=48000)
    framebuffer = FramebufferOutput(device=display_device)
    debug_monitor = DebugMonitor()

    face_renderer = LunaFaceRenderer(width=240, height=320, fps=15)
    log("Face renderer created")

    vad_analyzer = SileroVADAnalyzer(params=VADParams(stop_secs=0.5, min_volume=0.6))
    vad = VADProcessor(vad_analyzer, sample_rate=16000)  # Wrap VAD for pipeline use
    log("VAD loaded")

    stt = OpenAISTTService(api_key=os.getenv("OPENAI_API_KEY"), model="whisper-1")
    tts = OpenAITTSService(api_key=os.getenv("OPENAI_API_KEY"), voice="nova")
    llm = AnthropicLLMService(api_key=os.getenv("ANTHROPIC_API_KEY"), model="claude-3-5-haiku-latest")
    log("AI services created")

    llm.register_function("set_emotion", set_emotion)
    llm.register_function("get_current_time", get_current_time)

    messages = [{"role": "system", "content": """Your name is Luna. You are a friendly voice assistant with an animated face.

IMPORTANT RULES:
1. ALWAYS speak a response after using set_emotion - never JUST call the tool silently
2. Keep responses SHORT - 1-2 sentences max (this is voice, not text)
3. Be warm and conversational
4. Use set_emotion to change your face before speaking

Example flow:
- User says hello
- You call set_emotion("happy")
- Then you SPEAK: "Hi there! How can I help you today?"

Never respond with ONLY a tool call. Always include spoken text."""}]

    context = LLMContext(messages, tools)
    context_aggregator = LLMContextAggregatorPair(context)

    pipeline = Pipeline([
        audio_input,
        vad,  # VAD must be before STT to generate speaking/stopped frames
        stt,
        debug_monitor,  # Monitor VAD frames + transcriptions (all pass through)
        context_aggregator.user(),
        llm,
        tts,
        face_renderer,
        framebuffer,
        audio_output,
        context_aggregator.assistant(),
    ])

    task = PipelineTask(pipeline, params=PipelineParams(allow_interruptions=True, enable_metrics=True))

    face_renderer.set_emotion("happy")
    log("Pipeline ready - say something!")
    debug_state["status"] = "running"

    runner = PipelineRunner()
    await runner.run(task)


async def run_web_server():
    """Run web server as async task."""
    config = uvicorn.Config(app, host="0.0.0.0", port=7860, log_level="warning")
    server = uvicorn.Server(config)
    await server.serve()


async def main_async(display_device: str, camera_index: int = -1):
    """Run both web server and Luna concurrently."""
    global camera

    # Start camera if enabled
    if camera_index >= 0:
        camera = CameraCapture(camera_index=camera_index)
        camera.start()

    # Start web server task
    web_task = asyncio.create_task(run_web_server())
    log("Web server starting on http://0.0.0.0:7860")

    # Give web server a moment to start
    await asyncio.sleep(1)

    # Run Luna
    try:
        await run_luna(display_device=display_device)
    except Exception as e:
        log(f"Luna error: {e}")
        debug_state["errors"].append(str(e))
    finally:
        if camera:
            camera.stop()
        web_task.cancel()


def main():
    parser = argparse.ArgumentParser(description="Luna Pi Debug")
    parser.add_argument("--display", "-d", default="/dev/fb0", help="Framebuffer device")
    parser.add_argument("--camera", "-c", type=int, default=-1, help="Camera index (e.g., 0 for /dev/video0)")
    parser.add_argument("--no-web", action="store_true", help="Disable web interface")
    args = parser.parse_args()

    import socket
    ip = socket.gethostbyname(socket.gethostname())
    print(f"\n🌐 Debug interface: http://{ip}:7860\n")
    if args.camera >= 0:
        print(f"📷 Camera enabled: /dev/video{args.camera}\n")

    try:
        if args.no_web:
            asyncio.run(run_luna(display_device=args.display))
        else:
            asyncio.run(main_async(display_device=args.display, camera_index=args.camera))
    except KeyboardInterrupt:
        log("Shutting down...")


if __name__ == "__main__":
    main()
