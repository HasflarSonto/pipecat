"""
Luna Face Renderer - Server-side animated face for pipecat
Renders a friendly robot face with emotions and blinking as video frames
Supports both robot mode and cat mode!
"""

import asyncio
import math
import random
import time
from dataclasses import dataclass
from typing import Optional

from PIL import Image, ImageDraw
from loguru import logger

from pipecat.frames.frames import CancelFrame, Frame, OutputImageRawFrame, StartFrame, SystemFrame
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor


@dataclass
class EmotionFrame(SystemFrame):
    """Frame to set the face emotion."""
    emotion: str


@dataclass
class GazeFrame(SystemFrame):
    """Frame to set the gaze direction (where Luna looks)."""
    x: float  # 0-1, where 0.5 is center
    y: float  # 0-1, where 0.5 is center


class LunaFaceRenderer(FrameProcessor):
    """
    Renders a friendly robot face with emotions.
    Supports robot mode (default) and cat mode.
    Outputs video frames at a specified framerate.
    """

    # Emotion configurations for the friendly robot style
    # eye_height/width control the rounded rectangle size
    # mouth_curve: positive = smile, negative = frown, 0 = neutral line
    # mouth_open: 0-1 how open the mouth is (for surprised "O" shape)
    EMOTIONS = {
        "neutral": {
            "eye_height": 60,  # Taller eyes
            "eye_width": 40,
            "eye_openness": 1.0,
            "mouth_curve": 0.0,
            "mouth_open": 0.0,
            "mouth_width": 25,  # Smaller mouth
        },
        "happy": {
            "eye_height": 55,  # Taller eyes
            "eye_width": 40,
            "eye_openness": 0.85,
            "mouth_curve": 0.5,  # Smaller smile
            "mouth_open": 0.0,
            "mouth_width": 22,  # Smaller mouth
        },
        "sad": {
            "eye_height": 55,  # Taller eyes
            "eye_width": 38,
            "eye_openness": 0.8,
            "mouth_curve": -0.4,  # Smaller frown
            "mouth_open": 0.0,
            "mouth_width": 20,  # Smaller mouth
        },
        "angry": {
            "eye_height": 45,  # Taller eyes
            "eye_width": 45,
            "eye_openness": 0.5,  # Narrowed eyes
            "mouth_curve": -0.3,
            "mouth_open": 0.0,
            "mouth_width": 25,
            "angry_brows": True,
        },
        "surprised": {
            "eye_height": 65,  # Taller eyes
            "eye_width": 45,
            "eye_openness": 1.15,
            "mouth_curve": 0.0,
            "mouth_open": 0.4,  # Smaller O mouth
            "mouth_width": 20,
        },
        "thinking": {
            "eye_height": 55,  # Taller eyes
            "eye_width": 40,
            "eye_openness": 0.9,
            "mouth_curve": 0.2,
            "mouth_open": 0.0,
            "mouth_width": 18,
            "look_side": True,  # Eyes look to the side
        },
        "confused": {
            "eye_height": 60,  # Taller eyes
            "eye_width": 40,
            "eye_openness": 1.0,
            "mouth_curve": -0.2,
            "mouth_open": 0.0,
            "mouth_width": 20,
            "tilt_eyes": True,  # One eye higher
        },
        "excited": {
            "eye_height": 65,  # Taller eyes
            "eye_width": 48,
            "eye_openness": 1.2,
            "mouth_curve": 0.8,
            "mouth_open": 0.2,  # Open smile
            "mouth_width": 30,
            "sparkle": True,
        },
        "cat": {
            "eye_height": 60,  # Taller eyes
            "eye_width": 40,
            "eye_openness": 1.0,
            "mouth_curve": 0.5,
            "mouth_open": 0.0,
            "mouth_width": 40,
            "cat_face": True,  # Special flag for cat mouth
        },
    }

    def __init__(
        self,
        width: int = 240,
        height: int = 320,
        fps: int = 30,
        cat_mode: bool = False,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.width = width
        self.height = height
        self.fps = fps
        self.frame_interval = 1.0 / fps
        self.cat_mode = cat_mode

        # Colors
        self.bg_color = (30, 30, 40)  # Dark background
        self.face_color = (255, 255, 255)  # White for eyes/features
        self.blush_color = (255, 180, 180)  # Pink blush for cat mode

        # Face center position
        self.center_x = width // 2
        self.center_y = height // 2 - 50  # Shifted up for better framing

        # Eye positions (relative to center)
        self.eye_spacing = 50
        self.left_eye_x = self.center_x - self.eye_spacing
        self.right_eye_x = self.center_x + self.eye_spacing
        self.eye_y = self.center_y - 10

        # Mouth position
        self.mouth_y = self.center_y + 50

        # Animation state
        self.current_emotion = "neutral"
        self.target_emotion = "neutral"
        self.emotion_transition = 1.0

        # Blink state
        self.blink_progress = 0.0
        self.is_blinking = False
        self.last_blink_time = time.time()
        self.blink_interval = 3.0 + random.random() * 2.0

        # Running state
        self._running = False
        self._render_task: Optional[asyncio.Task] = None

        # Gaze state (0-1, where 0.5 is center)
        self.gaze_x = 0.5
        self.gaze_y = 0.5
        self.target_gaze_x = 0.5
        self.target_gaze_y = 0.5

        # Face offset for edge tracking (shifts entire face when looking at edges)
        self.face_offset_x = 0.0
        self.face_offset_y = 0.0

        # Current parameters (interpolated)
        self._current_params = dict(self.EMOTIONS["neutral"])

    def set_cat_mode(self, enabled: bool):
        """Toggle cat mode."""
        self.cat_mode = enabled
        logger.info(f"Cat mode: {'enabled' if enabled else 'disabled'}")

    def set_gaze(self, x: float, y: float):
        """Set target gaze direction (0-1, where 0.5 is center)."""
        self.target_gaze_x = max(0.0, min(1.0, x))
        self.target_gaze_y = max(0.0, min(1.0, y))

    def _lerp(self, a: float, b: float, t: float) -> float:
        """Linear interpolation."""
        return a + (b - a) * t

    def _update_animation(self, delta_time: float):
        """Update animation state."""
        # Update emotion transition
        if self.emotion_transition < 1.0:
            self.emotion_transition = min(1.0, self.emotion_transition + delta_time * 4)
            if self.emotion_transition >= 1.0:
                self.current_emotion = self.target_emotion

        # Interpolate emotion parameters
        current = self.EMOTIONS.get(self.current_emotion, self.EMOTIONS["neutral"])
        target = self.EMOTIONS.get(self.target_emotion, self.EMOTIONS["neutral"])
        t = self.emotion_transition

        for key in ["eye_height", "eye_width", "eye_openness", "mouth_curve", "mouth_open", "mouth_width"]:
            self._current_params[key] = self._lerp(current.get(key, 0), target.get(key, 0), t)

        # Copy boolean flags from target
        for key in ["angry_brows", "look_side", "tilt_eyes", "sparkle", "cat_face"]:
            self._current_params[key] = target.get(key, False)

        # Update gaze (smooth follow)
        gaze_speed = 10.0  # How fast eyes follow (increased for snappier response)
        self.gaze_x = self._lerp(self.gaze_x, self.target_gaze_x, delta_time * gaze_speed)
        self.gaze_y = self._lerp(self.gaze_y, self.target_gaze_y, delta_time * gaze_speed)

        # Calculate face offset for edge tracking
        # When gaze is at extreme edges, shift the entire face
        target_offset_x = 0.0
        target_offset_y = 0.0
        edge_threshold = 0.25  # Start shifting when within 25% of edge
        max_face_shift = 25  # Maximum pixels to shift face

        # Horizontal face shift
        if self.gaze_x < edge_threshold:
            # Looking far left - shift face left
            edge_factor = (edge_threshold - self.gaze_x) / edge_threshold
            target_offset_x = -max_face_shift * edge_factor
        elif self.gaze_x > (1 - edge_threshold):
            # Looking far right - shift face right
            edge_factor = (self.gaze_x - (1 - edge_threshold)) / edge_threshold
            target_offset_x = max_face_shift * edge_factor

        # Vertical face shift
        if self.gaze_y < edge_threshold:
            # Looking far up - shift face up
            edge_factor = (edge_threshold - self.gaze_y) / edge_threshold
            target_offset_y = -max_face_shift * 0.6 * edge_factor  # Less vertical shift
        elif self.gaze_y > (1 - edge_threshold):
            # Looking far down - shift face down
            edge_factor = (self.gaze_y - (1 - edge_threshold)) / edge_threshold
            target_offset_y = max_face_shift * 0.6 * edge_factor

        # Smooth the face offset
        face_shift_speed = 6.0
        self.face_offset_x = self._lerp(self.face_offset_x, target_offset_x, delta_time * face_shift_speed)
        self.face_offset_y = self._lerp(self.face_offset_y, target_offset_y, delta_time * face_shift_speed)

        # Update blink
        current_time = time.time()
        if not self.is_blinking and current_time - self.last_blink_time > self.blink_interval:
            self.is_blinking = True
            self.blink_progress = 0.0
            self.blink_interval = 2.0 + random.random() * 3.0
            self.last_blink_time = current_time

        if self.is_blinking:
            self.blink_progress += delta_time * 10
            if self.blink_progress >= 1.0:
                self.is_blinking = False
                self.blink_progress = 0.0

    def _get_blink_factor(self) -> float:
        """Get current blink factor (0 = open, 1 = closed)."""
        if not self.is_blinking:
            return 0.0
        # Quick close, slower open
        if self.blink_progress < 0.3:
            return self.blink_progress / 0.3
        else:
            return 1.0 - (self.blink_progress - 0.3) / 0.7

    def _draw_rounded_rect(self, draw: ImageDraw.Draw, bbox: list, radius: int, fill):
        """Draw a rounded rectangle."""
        x1, y1, x2, y2 = bbox
        # Ensure minimum size
        if x2 - x1 < 2 or y2 - y1 < 2:
            return
        radius = min(radius, (x2 - x1) // 2, (y2 - y1) // 2)
        draw.rounded_rectangle(bbox, radius=radius, fill=fill)

    def _draw_robot_eye(self, draw: ImageDraw.Draw, x: int, y: int, is_right: bool):
        """Draw a friendly robot eye - solid white rounded rectangle."""
        params = self._current_params
        blink_factor = self._get_blink_factor()

        eye_height = params["eye_height"]
        eye_width = params["eye_width"]

        # Apply openness
        eye_height *= params["eye_openness"]

        # Apply blink - squish vertically
        eye_height *= (1 - blink_factor * 0.95)

        # Gaze offset - move eyes based on where we're looking
        # gaze_x: 0=look left, 0.5=center, 1=look right
        # gaze_y: 0=look up, 0.5=center, 1=look down
        gaze_range_x = 28  # Max pixels to shift horizontally
        gaze_range_y = 18  # Max pixels to shift vertically
        gaze_x_offset = int((self.gaze_x - 0.5) * 2 * gaze_range_x)
        gaze_y_offset = int((self.gaze_y - 0.5) * 2 * gaze_range_y)

        # Tilt adjustment for confused
        y_offset = gaze_y_offset
        if params.get("tilt_eyes"):
            y_offset += 8 if is_right else -8

        # Look to the side for thinking (overrides gaze)
        x_offset = gaze_x_offset
        if params.get("look_side"):
            x_offset = 12  # Both eyes look right

        # Ensure minimum height during blink
        eye_height = max(3, eye_height)

        # Draw the eye as a rounded rectangle
        eye_bbox = [
            x + x_offset - eye_width // 2,
            y + y_offset - int(eye_height) // 2,
            x + x_offset + eye_width // 2,
            y + y_offset + int(eye_height) // 2,
        ]
        radius = min(15, int(eye_height) // 2, eye_width // 2)
        self._draw_rounded_rect(draw, eye_bbox, radius, self.face_color)

    def _draw_cat_eye(self, draw: ImageDraw.Draw, x: int, y: int, is_right: bool):
        """Draw a cat eye - vertical slit style."""
        params = self._current_params
        blink_factor = self._get_blink_factor()

        eye_height = params["eye_height"] * 1.2  # Cat eyes are taller
        eye_width = params["eye_width"] * 0.7  # And narrower

        # Apply openness
        eye_height *= params["eye_openness"]

        # Apply blink
        eye_height *= (1 - blink_factor * 0.95)

        # Tilt adjustment for confused
        y_offset = 0
        if params.get("tilt_eyes"):
            y_offset = 8 if is_right else -8

        # Look to the side for thinking
        x_offset = 0
        if params.get("look_side"):
            x_offset = 10

        eye_height = max(3, eye_height)

        # Draw almond-shaped cat eye
        # Use a vertical ellipse
        eye_bbox = [
            x + x_offset - eye_width // 2,
            y + y_offset - int(eye_height) // 2,
            x + x_offset + eye_width // 2,
            y + y_offset + int(eye_height) // 2,
        ]
        draw.ellipse(eye_bbox, fill=self.face_color)

    def _draw_robot_mouth(self, draw: ImageDraw.Draw, offset_x: int = 0, offset_y: int = 0):
        """Draw the robot mouth - simple curved line or O shape."""
        params = self._current_params
        mouth_curve = params["mouth_curve"]
        mouth_open = params["mouth_open"]
        mouth_width = int(params["mouth_width"])

        x = self.center_x + offset_x
        y = self.mouth_y + offset_y

        if mouth_open > 0.3:
            # Draw O-shaped mouth for surprised
            o_width = int(20 + mouth_open * 15)
            o_height = int(15 + mouth_open * 20)
            mouth_bbox = [
                x - o_width,
                y - o_height,
                x + o_width,
                y + o_height,
            ]
            draw.ellipse(mouth_bbox, fill=self.face_color)
            # Draw inner dark circle to make it look like open mouth
            inner_bbox = [
                x - o_width + 4,
                y - o_height + 4,
                x + o_width - 4,
                y + o_height - 4,
            ]
            draw.ellipse(inner_bbox, fill=self.bg_color)
        else:
            # Draw curved line mouth
            # Calculate curve points
            curve_amount = int(mouth_curve * 15)

            if abs(mouth_curve) < 0.1:
                # Straight line for neutral
                draw.line(
                    [(x - mouth_width, y), (x + mouth_width, y)],
                    fill=self.face_color,
                    width=4,
                )
            else:
                # Curved smile or frown using arc
                arc_bbox = [
                    x - mouth_width,
                    y - abs(curve_amount) * 2,
                    x + mouth_width,
                    y + abs(curve_amount) * 2,
                ]
                if mouth_curve > 0:
                    # Smile - arc on bottom half
                    draw.arc(arc_bbox, start=0, end=180, fill=self.face_color, width=4)
                else:
                    # Frown - arc on top half
                    draw.arc(arc_bbox, start=180, end=360, fill=self.face_color, width=4)

    def _draw_cat_mouth(self, draw: ImageDraw.Draw, offset_x: int = 0, offset_y: int = 0):
        """Draw cat mouth - horizontal 3 shape (like ω) with whiskers."""
        x = self.center_x + offset_x
        # Move mouth closer to eyes
        y = self.eye_y + 50 + offset_y

        # Draw horizontal "3" - like ω shape
        # Two bumps going downward, meeting in the middle
        bump_size = 10
        bump_width = 18

        # Left bump (curves down)
        draw.arc([
            x - bump_width * 2, y - bump_size,
            x, y + bump_size
        ], start=0, end=180, fill=self.face_color, width=4)

        # Right bump (curves down)
        draw.arc([
            x, y - bump_size,
            x + bump_width * 2, y + bump_size
        ], start=0, end=180, fill=self.face_color, width=4)

        # Whiskers - 3 on each side, positioned lower near the mouth
        whisker_length = 40
        whisker_start_x = 55  # Start from outside the eyes
        whisker_y = self.eye_y + 45 + offset_y  # Closer to mouth level

        for i, angle in enumerate([-15, 0, 15]):
            angle_rad = math.radians(angle)
            end_y_offset = int(math.sin(angle_rad) * whisker_length)
            y_offset = (i - 1) * 8  # Space whiskers vertically

            # Left whiskers (go left)
            draw.line([
                (x - whisker_start_x, whisker_y + y_offset),
                (x - whisker_start_x - whisker_length, whisker_y + y_offset + end_y_offset),
            ], fill=self.face_color, width=3)

            # Right whiskers (go right)
            draw.line([
                (x + whisker_start_x, whisker_y + y_offset),
                (x + whisker_start_x + whisker_length, whisker_y + y_offset + end_y_offset),
            ], fill=self.face_color, width=3)

    def _draw_cat_ears(self, draw: ImageDraw.Draw):
        """Draw cat ears at the top."""
        ear_size = 35
        ear_y = self.eye_y - 60

        # Left ear
        draw.polygon([
            (self.left_eye_x - 15, ear_y + ear_size),
            (self.left_eye_x - 5, ear_y - ear_size),
            (self.left_eye_x + 25, ear_y + ear_size),
        ], fill=self.face_color)

        # Right ear
        draw.polygon([
            (self.right_eye_x - 25, ear_y + ear_size),
            (self.right_eye_x + 5, ear_y - ear_size),
            (self.right_eye_x + 15, ear_y + ear_size),
        ], fill=self.face_color)

        # Inner ear (darker)
        inner_size = ear_size * 0.5
        draw.polygon([
            (self.left_eye_x - 10, ear_y + inner_size + 5),
            (self.left_eye_x - 3, ear_y - inner_size + 10),
            (self.left_eye_x + 15, ear_y + inner_size + 5),
        ], fill=self.blush_color)

        draw.polygon([
            (self.right_eye_x - 15, ear_y + inner_size + 5),
            (self.right_eye_x + 3, ear_y - inner_size + 10),
            (self.right_eye_x + 10, ear_y + inner_size + 5),
        ], fill=self.blush_color)

    def _draw_angry_brows(self, draw: ImageDraw.Draw, offset_x: int = 0, offset_y: int = 0):
        """Draw angry eyebrows (V shape)."""
        brow_y = self.eye_y - 35 + offset_y
        brow_length = 30
        left_eye_x = self.left_eye_x + offset_x
        right_eye_x = self.right_eye_x + offset_x

        # Left brow - angled down toward center
        draw.line([
            (left_eye_x - brow_length, brow_y - 10),
            (left_eye_x + 10, brow_y + 5),
        ], fill=self.face_color, width=5)

        # Right brow - angled down toward center
        draw.line([
            (right_eye_x - 10, brow_y + 5),
            (right_eye_x + brow_length, brow_y - 10),
        ], fill=self.face_color, width=5)

    def _draw_sparkles(self, draw: ImageDraw.Draw):
        """Draw sparkles for excited emotion."""
        sparkle_color = (255, 255, 150)  # Light yellow
        current_time = time.time()

        sparkle_positions = [
            (45, 70),
            (195, 80),
            (55, 180),
            (185, 175),
        ]

        for i, (sx, sy) in enumerate(sparkle_positions):
            # Pulsing size
            scale = 0.6 + math.sin(current_time * 6 + i * 1.5) * 0.4
            size = int(10 * scale)

            if size > 3:
                # Draw 4-point star
                draw.polygon([
                    (sx, sy - size),
                    (sx + size // 3, sy),
                    (sx, sy + size),
                    (sx - size // 3, sy),
                ], fill=sparkle_color)
                draw.polygon([
                    (sx - size, sy),
                    (sx, sy - size // 3),
                    (sx + size, sy),
                    (sx, sy + size // 3),
                ], fill=sparkle_color)

    def render_frame(self) -> Image.Image:
        """Render a single frame of the face."""
        img = Image.new("RGB", (self.width, self.height), self.bg_color)
        draw = ImageDraw.Draw(img)

        params = self._current_params

        # Apply face offset for edge tracking
        offset_x = int(self.face_offset_x)
        offset_y = int(self.face_offset_y)

        # Calculate offset positions for all face elements
        left_eye_x = self.left_eye_x + offset_x
        right_eye_x = self.right_eye_x + offset_x
        eye_y = self.eye_y + offset_y

        # Always use robot eyes (rounded rectangles)
        self._draw_robot_eye(draw, left_eye_x, eye_y, is_right=False)
        self._draw_robot_eye(draw, right_eye_x, eye_y, is_right=True)

        # Draw angry brows if needed
        if params.get("angry_brows"):
            self._draw_angry_brows(draw, offset_x, offset_y)

        # Draw mouth - cat_face emotion uses sideways 3 + whiskers, others use simple curve
        if params.get("cat_face"):
            self._draw_cat_mouth(draw, offset_x, offset_y)
        else:
            self._draw_robot_mouth(draw, offset_x, offset_y)

        # Draw sparkles if excited
        if params.get("sparkle") and self.emotion_transition > 0.5:
            self._draw_sparkles(draw)

        return img

    def set_emotion(self, emotion: str):
        """Set the target emotion."""
        if emotion in self.EMOTIONS:
            self.target_emotion = emotion
            self.emotion_transition = 0.0
            logger.info(f"Luna face emotion set to: {emotion}")
        else:
            logger.warning(f"Unknown emotion: {emotion}")

    async def _render_loop(self):
        """Main render loop that outputs video frames."""
        last_time = time.time()

        while self._running:
            current_time = time.time()
            delta_time = current_time - last_time
            last_time = current_time

            # Update animation
            self._update_animation(delta_time)

            # Render frame
            img = self.render_frame()

            # Convert to bytes (RGB format)
            frame_bytes = img.tobytes()

            # Create and push output frame
            output_frame = OutputImageRawFrame(
                image=frame_bytes,
                size=(self.width, self.height),
                format="RGB",
            )
            await self.push_frame(output_frame)

            # Wait for next frame
            elapsed = time.time() - current_time
            sleep_time = max(0, self.frame_interval - elapsed)
            await asyncio.sleep(sleep_time)

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        """Process incoming frames."""
        await super().process_frame(frame, direction)

        # Handle StartFrame - start the render loop
        if isinstance(frame, StartFrame):
            if not self._running:
                self._running = True
                self._render_task = self.create_task(self._render_loop(), "render_loop")
                logger.info(f"Luna face renderer started (cat_mode={self.cat_mode})")

        # Handle emotion frames
        if isinstance(frame, EmotionFrame):
            self.set_emotion(frame.emotion)
            return

        # Handle gaze frames
        if isinstance(frame, GazeFrame):
            self.set_gaze(frame.x, frame.y)
            return

        # Handle CancelFrame - stop the render loop
        if isinstance(frame, CancelFrame):
            await self._stop_render_loop()

        # Pass through other frames
        await self.push_frame(frame, direction)

    async def _stop_render_loop(self):
        """Stop the render loop."""
        self._running = False
        if self._render_task:
            await self.cancel_task(self._render_task)
            self._render_task = None
        logger.info("Luna face renderer stopped")

    async def cleanup(self):
        """Cleanup resources."""
        await self._stop_render_loop()
        await super().cleanup()
