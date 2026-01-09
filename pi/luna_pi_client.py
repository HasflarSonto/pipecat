#!/usr/bin/env python3
"""
Luna Pi Client - Headless WebRTC client for Raspberry Pi

Connects to Luna server, renders video to ILI9341 framebuffer,
captures audio from USB mic, plays audio through speaker.

Requirements:
- aiortc (WebRTC)
- aiohttp (HTTP client)
- pyaudio (audio capture/playback)
- Pillow (image processing)
- numpy

Hardware:
- ILI9341 2.4" SPI display (240x320)
- USB microphone
- Speaker (3.5mm or USB)
- Optional: Pi Camera for vision
"""

import argparse
import asyncio
import json
import logging
import os
import struct
import sys
import time
from typing import Optional

import aiohttp
import numpy as np
from aiortc import RTCPeerConnection, RTCSessionDescription, RTCIceCandidate
from aiortc.contrib.media import MediaPlayer, MediaRecorder
from av import VideoFrame
from PIL import Image

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)


class FramebufferDisplay:
    """Renders frames to Linux framebuffer (ILI9341 display)."""

    def __init__(self, device: str = "/dev/fb0", width: int = 240, height: int = 320):
        self.device = device
        self.width = width
        self.height = height
        self.fb = None
        self._open()

    def _open(self):
        """Open the framebuffer device."""
        try:
            self.fb = open(self.device, 'wb')
            logger.info(f"Opened framebuffer: {self.device}")
        except FileNotFoundError:
            logger.warning(f"Framebuffer {self.device} not found, using dummy display")
            self.fb = None
        except PermissionError:
            logger.error(f"Permission denied for {self.device}. Run with sudo or add user to video group.")
            self.fb = None

    def render(self, frame: Image.Image):
        """Render a PIL Image to the framebuffer."""
        if self.fb is None:
            return

        # Resize if needed
        if frame.size != (self.width, self.height):
            frame = frame.resize((self.width, self.height), Image.Resampling.LANCZOS)

        # Convert to RGB565 (16-bit color for ILI9341)
        rgb = frame.convert('RGB')
        pixels = np.array(rgb)

        # RGB888 to RGB565: RRRRRGGG GGGBBBBB
        r = (pixels[:, :, 0] >> 3).astype(np.uint16)
        g = (pixels[:, :, 1] >> 2).astype(np.uint16)
        b = (pixels[:, :, 2] >> 3).astype(np.uint16)
        rgb565 = (r << 11) | (g << 5) | b

        # Write to framebuffer
        self.fb.seek(0)
        self.fb.write(rgb565.tobytes())
        self.fb.flush()

    def clear(self, color: tuple = (0, 0, 0)):
        """Clear the display with a solid color."""
        img = Image.new('RGB', (self.width, self.height), color)
        self.render(img)

    def close(self):
        """Close the framebuffer device."""
        if self.fb:
            self.fb.close()
            self.fb = None


class AudioHandler:
    """Handles audio input (microphone) and output (speaker) using PyAudio."""

    def __init__(self, sample_rate: int = 48000, channels: int = 1, chunk_size: int = 960):
        self.sample_rate = sample_rate
        self.channels = channels
        self.chunk_size = chunk_size
        self.pyaudio = None
        self.input_stream = None
        self.output_stream = None
        self._init_audio()

    def _init_audio(self):
        """Initialize PyAudio streams."""
        try:
            import pyaudio
            self.pyaudio = pyaudio.PyAudio()

            # List available devices
            logger.info("Available audio devices:")
            for i in range(self.pyaudio.get_device_count()):
                dev = self.pyaudio.get_device_info_by_index(i)
                logger.info(f"  [{i}] {dev['name']} (in:{dev['maxInputChannels']}, out:{dev['maxOutputChannels']})")

            # Open input stream (microphone)
            try:
                self.input_stream = self.pyaudio.open(
                    format=pyaudio.paInt16,
                    channels=self.channels,
                    rate=self.sample_rate,
                    input=True,
                    frames_per_buffer=self.chunk_size,
                )
                logger.info("Microphone initialized")
            except Exception as e:
                logger.warning(f"Could not open microphone: {e}")

            # Open output stream (speaker)
            try:
                self.output_stream = self.pyaudio.open(
                    format=pyaudio.paInt16,
                    channels=self.channels,
                    rate=self.sample_rate,
                    output=True,
                    frames_per_buffer=self.chunk_size,
                )
                logger.info("Speaker initialized")
            except Exception as e:
                logger.warning(f"Could not open speaker: {e}")

        except ImportError:
            logger.error("PyAudio not installed. Run: pip install pyaudio")
        except Exception as e:
            logger.error(f"Audio initialization failed: {e}")

    def read_audio(self) -> Optional[bytes]:
        """Read audio from microphone."""
        if self.input_stream:
            try:
                return self.input_stream.read(self.chunk_size, exception_on_overflow=False)
            except Exception as e:
                logger.warning(f"Audio read error: {e}")
        return None

    def play_audio(self, data: bytes):
        """Play audio through speaker."""
        if self.output_stream:
            try:
                self.output_stream.write(data)
            except Exception as e:
                logger.warning(f"Audio play error: {e}")

    def close(self):
        """Close audio streams."""
        if self.input_stream:
            self.input_stream.stop_stream()
            self.input_stream.close()
        if self.output_stream:
            self.output_stream.stop_stream()
            self.output_stream.close()
        if self.pyaudio:
            self.pyaudio.terminate()


class LunaPiClient:
    """Main client that connects to Luna server via WebRTC."""

    def __init__(self, server_url: str, display_device: str = "/dev/fb0"):
        self.server_url = server_url.rstrip('/')
        self.session_id: Optional[str] = None
        self.pc: Optional[RTCPeerConnection] = None
        self.data_channel = None
        self.display = FramebufferDisplay(device=display_device)
        self.audio = AudioHandler()
        self.running = False
        self.connected = False

    async def connect(self):
        """Connect to Luna server via WebRTC."""
        logger.info(f"Connecting to {self.server_url}")

        async with aiohttp.ClientSession() as session:
            # 1. Start session
            async with session.post(f"{self.server_url}/start", json={
                "enableDefaultIceServers": True
            }) as resp:
                if resp.status != 200:
                    raise Exception(f"Failed to start session: {resp.status}")
                data = await resp.json()
                self.session_id = data["sessionId"]
                ice_config = data.get("iceConfig", {})
                logger.info(f"Session started: {self.session_id}")

            # 2. Create peer connection
            ice_servers = ice_config.get("iceServers", [{"urls": "stun:stun.l.google.com:19302"}])
            config = {"iceServers": ice_servers}
            self.pc = RTCPeerConnection(configuration=config)

            # 3. Set up track handlers
            @self.pc.on("track")
            async def on_track(track):
                logger.info(f"Received track: {track.kind}")
                if track.kind == "video":
                    asyncio.create_task(self._handle_video_track(track))
                elif track.kind == "audio":
                    asyncio.create_task(self._handle_audio_track(track))

            @self.pc.on("connectionstatechange")
            async def on_connectionstatechange():
                logger.info(f"Connection state: {self.pc.connectionState}")
                if self.pc.connectionState == "connected":
                    self.connected = True
                elif self.pc.connectionState in ("failed", "closed"):
                    self.connected = False
                    self.running = False

            @self.pc.on("datachannel")
            async def on_datachannel(channel):
                logger.info(f"Data channel received: {channel.label}")
                self.data_channel = channel

                @channel.on("message")
                def on_message(message):
                    self._handle_rtvi_message(json.loads(message))

            # 4. Add audio track (microphone)
            # Note: aiortc MediaPlayer can use ALSA devices
            try:
                audio_player = MediaPlayer("default", format="alsa", options={
                    "sample_rate": "48000",
                    "channels": "1",
                })
                if audio_player.audio:
                    self.pc.addTrack(audio_player.audio)
                    logger.info("Added microphone track")
            except Exception as e:
                logger.warning(f"Could not add microphone track: {e}")

            # 5. Add video transceiver (to receive video)
            self.pc.addTransceiver("video", direction="recvonly")

            # 6. Create and send offer
            offer = await self.pc.createOffer()
            await self.pc.setLocalDescription(offer)

            async with session.post(
                f"{self.server_url}/sessions/{self.session_id}/api/offer",
                json={"sdp": self.pc.localDescription.sdp, "type": self.pc.localDescription.type}
            ) as resp:
                if resp.status != 200:
                    raise Exception(f"Failed to send offer: {resp.status}")
                answer_data = await resp.json()

            # 7. Set remote description
            answer = RTCSessionDescription(sdp=answer_data["sdp"], type="answer")
            await self.pc.setRemoteDescription(answer)

            # 8. Trickle ICE candidates
            @self.pc.on("icecandidate")
            async def on_icecandidate(candidate):
                if candidate:
                    try:
                        async with aiohttp.ClientSession() as ice_session:
                            await ice_session.patch(
                                f"{self.server_url}/sessions/{self.session_id}/api/offer",
                                json={
                                    "candidate": candidate.candidate,
                                    "sdpMid": candidate.sdpMid,
                                    "sdpMLineIndex": candidate.sdpMLineIndex,
                                }
                            )
                    except Exception as e:
                        logger.warning(f"Failed to send ICE candidate: {e}")

            logger.info("WebRTC connection initiated")
            self.running = True

    async def _handle_video_track(self, track):
        """Process incoming video frames and render to display."""
        logger.info("Starting video handler")
        frame_count = 0
        last_log = time.time()

        while self.running:
            try:
                frame = await asyncio.wait_for(track.recv(), timeout=1.0)

                # Convert to PIL Image
                img = frame.to_image()

                # Render to framebuffer
                self.display.render(img)

                frame_count += 1
                now = time.time()
                if now - last_log > 5:
                    fps = frame_count / (now - last_log)
                    logger.info(f"Video: {fps:.1f} FPS")
                    frame_count = 0
                    last_log = now

            except asyncio.TimeoutError:
                continue
            except Exception as e:
                if self.running:
                    logger.warning(f"Video error: {e}")
                break

        logger.info("Video handler stopped")

    async def _handle_audio_track(self, track):
        """Process incoming audio and play through speaker."""
        logger.info("Starting audio handler")

        while self.running:
            try:
                frame = await asyncio.wait_for(track.recv(), timeout=1.0)

                # Convert to bytes and play
                audio_data = frame.to_ndarray().tobytes()
                self.audio.play_audio(audio_data)

            except asyncio.TimeoutError:
                continue
            except Exception as e:
                if self.running:
                    logger.warning(f"Audio error: {e}")
                break

        logger.info("Audio handler stopped")

    def _handle_rtvi_message(self, message):
        """Handle RTVI protocol messages."""
        msg_type = message.get("type")
        if msg_type in ("bot-ready", "error", "user-transcription"):
            logger.info(f"RTVI: {msg_type} - {message.get('data', '')}")

    async def send_client_ready(self):
        """Send client-ready message via data channel."""
        if self.data_channel and self.data_channel.readyState == "open":
            self.data_channel.send(json.dumps({
                "type": "client-ready",
                "data": {"version": "1.0.0"}
            }))
            logger.info("Sent client-ready")

    async def run(self):
        """Main run loop."""
        try:
            await self.connect()

            # Wait for connection
            for _ in range(50):  # 5 second timeout
                if self.connected:
                    break
                await asyncio.sleep(0.1)

            if not self.connected:
                logger.error("Connection timeout")
                return

            logger.info("Connected to Luna!")

            # Send client-ready after a short delay
            await asyncio.sleep(0.5)
            await self.send_client_ready()

            # Keep running until disconnected
            while self.running:
                await asyncio.sleep(1)

        except KeyboardInterrupt:
            logger.info("Interrupted by user")
        except Exception as e:
            logger.error(f"Error: {e}")
        finally:
            await self.cleanup()

    async def cleanup(self):
        """Clean up resources."""
        self.running = False

        if self.pc:
            await self.pc.close()

        self.display.clear()
        self.display.close()
        self.audio.close()

        logger.info("Cleanup complete")


async def main():
    parser = argparse.ArgumentParser(description="Luna Pi Client")
    parser.add_argument("--server", "-s", default="http://localhost:7860",
                        help="Luna server URL (default: http://localhost:7860)")
    parser.add_argument("--display", "-d", default="/dev/fb0",
                        help="Framebuffer device (default: /dev/fb0)")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Enable verbose logging")
    args = parser.parse_args()

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    client = LunaPiClient(server_url=args.server, display_device=args.display)
    await client.run()


if __name__ == "__main__":
    asyncio.run(main())
