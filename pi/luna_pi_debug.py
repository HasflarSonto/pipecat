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

# Optional: OpenCV for camera and image processing
try:
    import cv2
    CV2_AVAILABLE = True
except ImportError:
    CV2_AVAILABLE = False
    logger.warning("OpenCV not available - face detection disabled")

# Optional: Picamera2 for Raspberry Pi Camera
try:
    from picamera2 import Picamera2
    PICAMERA2_AVAILABLE = True
except ImportError:
    PICAMERA2_AVAILABLE = False
    logger.info("Picamera2 not available - will try OpenCV for USB cameras")

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
    "gaze_x": 0.5,  # Current gaze position (0-1)
    "gaze_y": 0.5,
}


def log(msg):
    """Log message to both console and debug state."""
    timestamp = datetime.now().strftime("%H:%M:%S")
    debug_state["logs"].append(f"[{timestamp}] {msg}")
    logger.info(msg)


# ============== CAMERA WITH DETECTION ==============

class CameraCapture:
    """Captures camera frames and runs face/hand detection.

    Supports:
    - Pi Camera via picamera2 (libcamera stack)
    - USB cameras via OpenCV VideoCapture
    - Gaze tracking: sends face position to Luna's face renderer
    """

    def __init__(self, camera_index: int = 0, width: int = 320, height: int = 240,
                 use_picamera: bool = True, face_renderer: "LunaFaceRenderer" = None):
        self.camera_index = camera_index
        self.width = width
        self.height = height
        self.use_picamera = use_picamera and PICAMERA2_AVAILABLE
        self.cap = None  # OpenCV VideoCapture
        self.picam = None  # Picamera2 instance
        self.running = False
        self.thread = None
        self.face_renderer = face_renderer  # Reference to Luna's face for gaze tracking

        # MediaPipe detectors
        self.face_detection = None
        self.hand_detection = None

        if MP_AVAILABLE:
            self.mp_face = mp.solutions.face_detection
            self.mp_hands = mp.solutions.hands
            self.mp_draw = mp.solutions.drawing_utils

    def start(self):
        """Start camera capture in background thread."""
        # Try Pi Camera first if available and requested
        if self.use_picamera:
            try:
                self.picam = Picamera2()
                # Configure for still capture with preview
                config = self.picam.create_preview_configuration(
                    main={"size": (self.width, self.height), "format": "RGB888"}
                )
                self.picam.configure(config)
                self.picam.start()
                log(f"Pi Camera started via picamera2 ({self.width}x{self.height})")

                self._start_detection_and_loop()
                return True

            except Exception as e:
                log(f"Pi Camera error: {e}, trying OpenCV...")
                self.picam = None
                # Fall through to try OpenCV

        # Fall back to OpenCV for USB cameras
        if not CV2_AVAILABLE:
            log("Camera disabled - neither picamera2 nor OpenCV available")
            return False

        try:
            self.cap = cv2.VideoCapture(self.camera_index)
            self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
            self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)
            self.cap.set(cv2.CAP_PROP_FPS, 15)

            if not self.cap.isOpened():
                log(f"Failed to open camera {self.camera_index}")
                return False

            log(f"USB camera started via OpenCV (index {self.camera_index})")
            self._start_detection_and_loop()
            return True

        except Exception as e:
            log(f"Camera error: {e}")
            debug_state["errors"].append(f"Camera: {e}")
            return False

    def _start_detection_and_loop(self):
        """Initialize detection and start capture thread."""
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

    def _capture_frame(self):
        """Capture a single frame from camera (Pi Camera or USB)."""
        if self.picam is not None:
            # Pi Camera via picamera2 - returns RGB
            frame_rgb = self.picam.capture_array()
            # Rotate 180 degrees (Pi Camera is often mounted upside down)
            frame_rgb = cv2.rotate(frame_rgb, cv2.ROTATE_180) if CV2_AVAILABLE else frame_rgb[::-1, ::-1]
            # Convert RGB to BGR for OpenCV drawing functions
            frame_bgr = cv2.cvtColor(frame_rgb, cv2.COLOR_RGB2BGR) if CV2_AVAILABLE else None
            return frame_rgb, frame_bgr
        elif self.cap is not None:
            # USB camera via OpenCV - returns BGR
            ret, frame_bgr = self.cap.read()
            if not ret:
                return None, None
            # Rotate 180 degrees
            frame_bgr = cv2.rotate(frame_bgr, cv2.ROTATE_180)
            frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
            return frame_rgb, frame_bgr
        return None, None

    def _capture_loop(self):
        """Background thread for camera capture.

        Runs detection every DETECTION_INTERVAL seconds to save CPU,
        but streams frames more frequently for smooth video.
        """
        DETECTION_INTERVAL = 1.0  # Run face detection every 1 second
        FRAME_INTERVAL = 0.2  # Stream frames every 200ms (~5 FPS for video)
        last_detection_time = 0

        # Cache last detection results for drawing between detections
        cached_faces = []

        while self.running:
            try:
                frame_rgb, frame_bgr = self._capture_frame()
                if frame_rgb is None:
                    time.sleep(0.1)
                    continue

                current_time = time.time()
                run_detection = (current_time - last_detection_time) >= DETECTION_INTERVAL

                face_detected = False
                hand_detected = False

                # For detection drawing, we need BGR frame (OpenCV format)
                # If no OpenCV, we'll convert RGB to JPEG directly via PIL
                draw_frame = frame_bgr if frame_bgr is not None else frame_rgb

                if MP_AVAILABLE and self.face_detection and self.hand_detection and frame_bgr is not None and run_detection:
                    # Use MediaPipe for detection (needs RGB input)
                    last_detection_time = current_time
                    face_results = self.face_detection.process(frame_rgb)
                    if face_results.detections:
                        face_detected = True
                        for detection in face_results.detections:
                            self.mp_draw.draw_detection(frame_bgr, detection)

                    hand_results = self.hand_detection.process(frame_rgb)
                    if hand_results.multi_hand_landmarks:
                        hand_detected = True
                        for hand_landmarks in hand_results.multi_hand_landmarks:
                            self.mp_draw.draw_landmarks(
                                frame_bgr, hand_landmarks, self.mp_hands.HAND_CONNECTIONS
                            )
                    draw_frame = frame_bgr

                elif HAAR_FACE_CASCADE is not None and frame_bgr is not None and run_detection:
                    # Fallback: Use OpenCV Haar cascade for face detection
                    last_detection_time = current_time
                    gray = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2GRAY)
                    faces = HAAR_FACE_CASCADE.detectMultiScale(
                        gray, scaleFactor=1.1, minNeighbors=5, minSize=(30, 30)
                    )
                    # Cache faces for drawing between detections
                    cached_faces = list(faces) if len(faces) > 0 else []

                    if len(faces) > 0:
                        face_detected = True
                        # Use the largest face for gaze tracking
                        largest_face = max(faces, key=lambda f: f[2] * f[3])
                        x, y, w, h = largest_face

                        # Calculate face center as normalized position (0-1)
                        # Camera sees mirror image, so invert X
                        face_center_x = 1.0 - ((x + w / 2) / self.width)
                        face_center_y = (y + h / 2) / self.height

                        # Update gaze - Luna looks AT the user's face
                        debug_state["gaze_x"] = face_center_x
                        debug_state["gaze_y"] = face_center_y

                        # Send to face renderer if available
                        if self.face_renderer:
                            self.face_renderer.set_gaze(face_center_x, face_center_y)
                    else:
                        # No face detected - reset gaze to center
                        debug_state["gaze_x"] = 0.5
                        debug_state["gaze_y"] = 0.5
                        if self.face_renderer:
                            self.face_renderer.set_gaze(0.5, 0.5)

                # Draw cached faces on frame (even between detections)
                if HAAR_FACE_CASCADE is not None and frame_bgr is not None and cached_faces:
                    face_detected = True
                    for (fx, fy, fw, fh) in cached_faces:
                        cv2.rectangle(frame_bgr, (fx, fy), (fx+fw, fy+fh), (0, 255, 0), 2)
                        cv2.putText(frame_bgr, "Face", (fx, fy-10),
                                   cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)
                    draw_frame = frame_bgr

                debug_state["face_detected"] = face_detected
                debug_state["hand_detected"] = hand_detected

                # Encode as JPEG
                if CV2_AVAILABLE and draw_frame is not None and len(draw_frame.shape) == 3:
                    _, jpeg = cv2.imencode('.jpg', draw_frame, [cv2.IMWRITE_JPEG_QUALITY, 80])
                    debug_state["last_camera_jpeg"] = jpeg.tobytes()
                else:
                    # Fallback: use PIL to encode RGB frame
                    img = Image.fromarray(frame_rgb)
                    jpeg_buffer = io.BytesIO()
                    img.save(jpeg_buffer, format='JPEG', quality=80)
                    debug_state["last_camera_jpeg"] = jpeg_buffer.getvalue()

                time.sleep(FRAME_INTERVAL)  # Stream at ~5 FPS, detect every 1s

            except Exception as e:
                debug_state["errors"].append(f"Camera capture: {e}")
                log(f"Camera capture error: {e}")
                time.sleep(0.5)

    def stop(self):
        """Stop camera capture."""
        self.running = False
        if self.thread:
            self.thread.join(timeout=1)
        if self.cap:
            self.cap.release()
        if self.picam:
            self.picam.stop()
            self.picam.close()
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
                <h2>Camera (Detection + Gaze)</h2>
                <img src="/camera" width="320" height="240" alt="Camera Feed">
                <p><span id="detection-status" class="value"></span></p>
                <p><span class="label">Gaze:</span> <span id="gaze-status" class="value"></span></p>
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
                // Gaze status
                const gaze = document.getElementById('gaze-status');
                if (gaze) {{
                    const x = (data.gaze_x * 100).toFixed(0);
                    const y = (data.gaze_y * 100).toFixed(0);
                    gaze.textContent = `X: ${{x}}% Y: ${{y}}%`;
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
        "gaze_x": debug_state["gaze_x"],
        "gaze_y": debug_state["gaze_y"],
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

async def set_emotion(params):
    """Set Luna's facial expression.

    Args:
        params: FunctionCallParams with arguments={'emotion': str}
    """
    global face_renderer
    # Extract emotion from params (pipecat passes FunctionCallParams object)
    emotion = params.arguments.get("emotion") if hasattr(params, 'arguments') else params

    log(f"set_emotion called with: {emotion}, face_renderer={face_renderer is not None}")
    if emotion not in VALID_EMOTIONS:
        log(f"Invalid emotion: {emotion}")
        return {"status": "error", "message": f"Invalid emotion: {emotion}"}
    if face_renderer:
        face_renderer.set_emotion(emotion)
        log(f"Emotion set to: {emotion}")
    else:
        log("WARNING: face_renderer is None!")
    return {"status": "success", "emotion": emotion}

async def get_current_time(params):
    """Get the current time."""
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

async def run_luna(display_device: str = "/dev/fb0", camera_index: int = -1):
    global face_renderer, camera

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

    # Test speaker at startup
    log("Testing speaker...")
    try:
        import pyaudio
        import struct
        import math

        pa = pyaudio.PyAudio()

        # Find USB speaker
        speaker_index = None
        for i in range(pa.get_device_count()):
            dev = pa.get_device_info_by_index(i)
            if dev['maxOutputChannels'] > 0:
                name = dev['name'].lower()
                if 'usb' in name or 'uac' in name:
                    speaker_index = i
                    break
                elif speaker_index is None:
                    speaker_index = i

        # Generate a short beep (440Hz for 0.3 seconds)
        sample_rate = 48000
        duration = 0.3
        frequency = 440
        samples = int(sample_rate * duration)
        beep_data = b''
        for i in range(samples):
            # Stereo: duplicate each sample
            value = int(16000 * math.sin(2 * math.pi * frequency * i / sample_rate))
            beep_data += struct.pack('<hh', value, value)  # Left and right channel

        stream = pa.open(
            format=pyaudio.paInt16,
            channels=2,
            rate=sample_rate,
            output=True,
            output_device_index=speaker_index,
        )
        stream.write(beep_data)
        stream.stop_stream()
        stream.close()
        pa.terminate()
        log("Speaker test: OK (beep played)")
    except Exception as e:
        log(f"Speaker test FAILED: {e}")
        debug_state["errors"].append(f"Speaker test: {e}")

    # Components
    audio_input = PyAudioInput(sample_rate=44100, output_sample_rate=16000)
    audio_output = PyAudioOutput(sample_rate=48000)
    framebuffer = FramebufferOutput(device=display_device)
    debug_monitor = DebugMonitor()

    face_renderer = LunaFaceRenderer(width=240, height=320, fps=15)
    log("Face renderer created")

    # Start camera with gaze tracking (after face_renderer is created)
    if camera_index >= 0:
        camera = CameraCapture(camera_index=camera_index, face_renderer=face_renderer)
        camera.start()
        log(f"Camera started with gaze tracking")

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

    # Start web server task
    web_task = asyncio.create_task(run_web_server())
    log("Web server starting on http://0.0.0.0:7860")

    # Give web server a moment to start
    await asyncio.sleep(1)

    # Run Luna (camera will be started inside after face_renderer is created)
    try:
        await run_luna(display_device=display_device, camera_index=camera_index)
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
