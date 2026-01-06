# Luna Face Tracking Pipeline

This document explains the complete pipeline for face tracking in Luna, from camera input to animated eye movement.

## Overview

Luna uses MediaPipe Face Detector (Tasks Vision API) to detect your face position in real-time and smoothly moves Luna's eyes to follow your head movements. Unlike the 8x8 LED matrix project, Luna uses a 240x320 pixel display allowing for smooth, continuous eye movement.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              FRONTEND (luna.html)                           │
├─────────────────────────────────────────────────────────────────────────────┤
│  1. Camera Stream (640x480)                                                 │
│  2. MediaPipe Face Detector (Tasks Vision API)                              │
│  3. Face Center Calculation                                                 │
│  4. WebRTC Data Channel → Send gaze {x, y} to backend                       │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                              BACKEND (my_bot.py)                            │
├─────────────────────────────────────────────────────────────────────────────┤
│  5. on_app_message handler receives gaze data                               │
│  6. Calls face_renderer.set_gaze(x, y)                                      │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         RENDERER (luna_face_renderer.py)                    │
├─────────────────────────────────────────────────────────────────────────────┤
│  7. Smooth interpolation of gaze position                                   │
│  8. Eye offset calculation based on gaze                                    │
│  9. Render frame with eye positions                                         │
│  10. Output video frame via WebRTC                                          │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Stage 1: MediaPipe Initialization (Frontend)

**File**: `static/luna.html`

### CDN Import
```html
<script src="https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@latest/vision_bundle.js" crossorigin="anonymous"></script>
```

### Face Detector Setup
```javascript
// Initialize WASM runtime
const vision = await FilesetResolver.forVisionTasks(
    "https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@latest/wasm"
);

// Create Face Detector
faceDetector = await FaceDetector.createFromOptions(vision, {
    baseOptions: {
        modelAssetPath: "https://storage.googleapis.com/mediapipe-models/face_detector/blaze_face_short_range/float16/1/blaze_face_short_range.tflite",
        delegate: "GPU"  // Use GPU acceleration
    },
    runningMode: "VIDEO",
    minDetectionConfidence: 0.5
});
```

## Stage 2: Camera Access (Frontend)

```javascript
cameraStream = await navigator.mediaDevices.getUserMedia({
    video: { width: 640, height: 480, facingMode: 'user' },
    audio: false
});
cameraVideo.srcObject = cameraStream;
```

## Stage 3: Frame Processing Loop (Frontend)

```javascript
function detectFace() {
    if (!isTrackingFace || !faceDetector) {
        requestAnimationFrame(detectFace);
        return;
    }

    // Only process new video frames
    if (cameraVideo.readyState >= 2 && cameraVideo.currentTime !== lastVideoTime) {
        lastVideoTime = cameraVideo.currentTime;

        const startTimeMs = performance.now();
        const detections = faceDetector.detectForVideo(cameraVideo, startTimeMs);
        processResults(detections);
    }

    requestAnimationFrame(detectFace);
}
```

## Stage 4: Face Detection Results (Frontend)

### Detection Output Structure
```javascript
result.detections = [
    {
        boundingBox: {
            originX: 150,      // Top-left X in pixels
            originY: 80,       // Top-left Y in pixels
            width: 200,        // Box width in pixels
            height: 250,       // Box height in pixels
        },
        keypoints: [
            { x: 0.35, y: 0.40 },  // Left eye (normalized 0-1)
            { x: 0.65, y: 0.40 },  // Right eye
            { x: 0.50, y: 0.55 },  // Nose tip
            { x: 0.50, y: 0.70 },  // Mouth center
            { x: 0.20, y: 0.45 },  // Left ear tragion
            { x: 0.80, y: 0.45 },  // Right ear tragion
        ],
        categories: [{ score: 0.95 }]  // Confidence
    }
]
```

### Face Center Calculation
```javascript
function processResults(result) {
    if (result.detections && result.detections.length > 0) {
        // Find largest face
        let largestFace = result.detections[0];
        let largestArea = 0;
        for (const detection of result.detections) {
            const bbox = detection.boundingBox;
            const area = bbox.width * bbox.height;
            if (area > largestArea) {
                largestArea = area;
                largestFace = detection;
            }
        }

        const bbox = largestFace.boundingBox;

        // Calculate normalized face center (0-1)
        const normalizedX = (bbox.originX + bbox.width / 2) / cameraVideo.videoWidth;
        const normalizedY = (bbox.originY + bbox.height / 2) / cameraVideo.videoHeight;

        // Invert X because camera is mirrored (creates natural following)
        const gazeX = 1 - normalizedX;
        const gazeY = normalizedY;

        // Send to backend
        sendGaze(gazeX, gazeY);
    }
}
```

## Stage 5: Draw Bounding Box (Frontend)

```javascript
// Scale from video coordinates to canvas coordinates
const scaleX = faceOverlay.width / cameraVideo.videoWidth;
const scaleY = faceOverlay.height / cameraVideo.videoHeight;

const x = bbox.originX * scaleX;
const y = bbox.originY * scaleY;
const w = bbox.width * scaleX;
const h = bbox.height * scaleY;

// Draw bounding box
overlayCtx.strokeStyle = '#7c83fd';
overlayCtx.lineWidth = 3;
overlayCtx.strokeRect(x, y, w, h);

// Draw center dot
const centerX = x + w / 2;
const centerY = y + h / 2;
overlayCtx.fillStyle = '#7c83fd';
overlayCtx.beginPath();
overlayCtx.arc(centerX, centerY, 5, 0, Math.PI * 2);
overlayCtx.fill();
```

## Stage 6: Send Gaze via WebRTC (Frontend)

```javascript
// Throttle to 10 updates/sec (100ms)
const now = Date.now();
if (dataChannel && dataChannel.readyState === 'open' && now - lastGazeUpdate > 100) {
    dataChannel.send(JSON.stringify({
        type: 'gaze',
        x: gazeX,  // 0-1, 0=left, 0.5=center, 1=right
        y: gazeY   // 0-1, 0=top, 0.5=center, 1=bottom
    }));
    lastGazeUpdate = now;
}
```

## Stage 7: Receive Gaze (Backend)

**File**: `my_bot.py`

```python
@transport.event_handler("on_app_message")
async def on_app_message(transport, message, sender):
    """Handle incoming app messages (like gaze data)."""
    try:
        data = message if isinstance(message, dict) else {}
        if data.get("type") == "gaze":
            x = data.get("x", 0.5)
            y = data.get("y", 0.5)
            face_renderer.set_gaze(x, y)
    except Exception as e:
        pass
```

## Stage 8: Smooth Gaze Interpolation (Backend)

**File**: `luna_face_renderer.py`

```python
def set_gaze(self, x: float, y: float):
    """Set target gaze direction (0-1, where 0.5 is center)."""
    self.target_gaze_x = max(0.0, min(1.0, x))
    self.target_gaze_y = max(0.0, min(1.0, y))

def _update_animation(self, delta_time: float):
    # Smooth follow using linear interpolation
    gaze_speed = 8.0  # Higher = faster response
    self.gaze_x = self._lerp(self.gaze_x, self.target_gaze_x, delta_time * gaze_speed)
    self.gaze_y = self._lerp(self.gaze_y, self.target_gaze_y, delta_time * gaze_speed)

def _lerp(self, a: float, b: float, t: float) -> float:
    """Linear interpolation."""
    return a + (b - a) * t
```

## Stage 9: Eye Position Calculation (Backend)

```python
def _draw_robot_eye(self, draw: ImageDraw.Draw, x: int, y: int, is_right: bool):
    # Gaze offset - move eyes based on where we're looking
    # gaze_x: 0=look left, 0.5=center, 1=look right
    # gaze_y: 0=look up, 0.5=center, 1=look down
    gaze_range_x = 15  # Max pixels to shift horizontally
    gaze_range_y = 10  # Max pixels to shift vertically

    gaze_x_offset = int((self.gaze_x - 0.5) * 2 * gaze_range_x)
    gaze_y_offset = int((self.gaze_y - 0.5) * 2 * gaze_range_y)

    # Draw eye at offset position
    eye_bbox = [
        x + gaze_x_offset - eye_width // 2,
        y + gaze_y_offset - int(eye_height) // 2,
        x + gaze_x_offset + eye_width // 2,
        y + gaze_y_offset + int(eye_height) // 2,
    ]
    self._draw_rounded_rect(draw, eye_bbox, radius, self.face_color)
```

## Coordinate Systems

### Video Frame (Frontend)
- Raw pixel coordinates from camera (640x480)
- `x=0` = left edge, `x=640` = right edge
- `y=0` = top edge, `y=480` = bottom edge

### Normalized Coordinates (Data Channel)
- Values between 0.0 and 1.0
- `x=0.0` = user on right side of screen (after mirror inversion)
- `x=1.0` = user on left side of screen
- `y=0.0` = user at top of screen
- `y=1.0` = user at bottom of screen

### Gaze Offset (Backend)
- Pixel offset from eye center
- `gaze_x=0.0` → offset = -15px (eyes look left)
- `gaze_x=0.5` → offset = 0px (eyes look center)
- `gaze_x=1.0` → offset = +15px (eyes look right)

## Key Differences from 8x8 LED Matrix

| Aspect | 8x8 LED Matrix | Luna (240x320) |
|--------|---------------|----------------|
| Resolution | 8x8 = 64 pixels | 240x320 = 76,800 pixels |
| Positions | 4x4 or 3x3 discrete grid | Continuous pixel positions |
| Movement | Jump between positions | Smooth interpolated movement |
| Update Rate | ~10 FPS | ~30 FPS |
| Gaze Range | Pattern placement (0-3 rows/cols) | Eye offset (15px horizontal, 10px vertical) |

## Files Involved

| File | Purpose |
|------|---------|
| `static/luna.html` | Frontend: Camera, MediaPipe, WebRTC client, face detection overlay |
| `my_bot.py` | Backend: WebRTC server, event handlers, pipeline setup |
| `luna_face_renderer.py` | Backend: Face rendering, gaze tracking, animation |

## Troubleshooting

### Face Detection Not Working
1. Check browser console for MediaPipe errors
2. Verify camera permissions granted
3. Check if `Face tracking: Active` status shows
4. Look for `Gaze: X=0.XX, Y=0.XX` debug output

### Eyes Not Moving
1. Check backend logs for gaze messages
2. Verify data channel is open
3. Check `on_app_message` handler is registered
4. Verify `set_gaze()` is being called

### Laggy Movement
1. Increase `gaze_speed` in `_update_animation()`
2. Reduce throttle interval (currently 100ms)
3. Check network latency
4. Increase renderer FPS

## Testing Checklist

- [ ] Camera preview shows in frontend
- [ ] Blue bounding box appears around face
- [ ] Center dot follows face movement
- [ ] Gaze debug shows updating X/Y values
- [ ] Backend logs show gaze messages received
- [ ] Luna's eyes move smoothly following your face
- [ ] Works with all emotions (happy, sad, cat, etc.)
