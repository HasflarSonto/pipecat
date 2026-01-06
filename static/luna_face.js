/**
 * Luna Face - Animated eyes with emotion support and face tracking
 * Inspired by tablet-robot-face
 */

class LunaFace {
    constructor(canvasId) {
        this.canvas = document.getElementById(canvasId);
        this.ctx = this.canvas.getContext('2d');

        // Canvas dimensions (240x320 portrait)
        this.width = 240;
        this.height = 320;

        // Current state
        this.emotion = 'neutral';
        this.targetEmotion = 'neutral';
        this.emotionTransition = 1.0;

        // Eye gaze (0-1, where 0.5 is center)
        this.gazeX = 0.5;
        this.gazeY = 0.3; // Default slightly up
        this.targetGazeX = 0.5;
        this.targetGazeY = 0.3;

        // Blink state
        this.blinkProgress = 0;
        this.isBlinking = false;
        this.lastBlinkTime = Date.now();
        this.blinkInterval = 3000 + Math.random() * 2000;

        // Animation
        this.animationFrame = null;
        this.lastFrameTime = Date.now();

        // Eye positions (centered, slightly up)
        this.leftEyeX = 80;
        this.rightEyeX = 160;
        this.eyeY = 120;

        // Emotion configurations
        this.emotions = {
            neutral: {
                eyeHeight: 40,
                eyeWidth: 35,
                pupilSize: 12,
                eyebrowAngle: 0,
                eyebrowHeight: 0,
                eyeOpenness: 1.0
            },
            happy: {
                eyeHeight: 30,
                eyeWidth: 38,
                pupilSize: 14,
                eyebrowAngle: -5,
                eyebrowHeight: -5,
                eyeOpenness: 0.8,
                squint: true
            },
            sad: {
                eyeHeight: 35,
                eyeWidth: 32,
                pupilSize: 10,
                eyebrowAngle: 15,
                eyebrowHeight: 10,
                eyeOpenness: 0.7
            },
            angry: {
                eyeHeight: 30,
                eyeWidth: 38,
                pupilSize: 10,
                eyebrowAngle: -20,
                eyebrowHeight: 5,
                eyeOpenness: 0.6
            },
            surprised: {
                eyeHeight: 50,
                eyeWidth: 40,
                pupilSize: 8,
                eyebrowAngle: 0,
                eyebrowHeight: -15,
                eyeOpenness: 1.2
            },
            thinking: {
                eyeHeight: 35,
                eyeWidth: 35,
                pupilSize: 11,
                eyebrowAngle: 10,
                eyebrowHeight: -5,
                eyeOpenness: 0.9,
                lookUp: true
            },
            confused: {
                eyeHeight: 38,
                eyeWidth: 35,
                pupilSize: 11,
                eyebrowAngle: 12,
                eyebrowHeight: 0,
                eyeOpenness: 0.95,
                asymmetric: true
            },
            excited: {
                eyeHeight: 45,
                eyeWidth: 40,
                pupilSize: 16,
                eyebrowAngle: -10,
                eyebrowHeight: -10,
                eyeOpenness: 1.1,
                sparkle: true
            }
        };

        // Start animation loop
        this.animate();

        // Start random blinking
        this.scheduleNextBlink();
    }

    setEmotion(emotion) {
        if (this.emotions[emotion]) {
            this.targetEmotion = emotion;
            this.emotionTransition = 0;
        }
    }

    setGaze(x, y) {
        // x, y are 0-1 where 0.5 is center
        // Clamp values
        this.targetGazeX = Math.max(0, Math.min(1, x));
        this.targetGazeY = Math.max(0, Math.min(1, y));
    }

    scheduleNextBlink() {
        setTimeout(() => {
            if (!this.isBlinking) {
                this.isBlinking = true;
                this.blinkProgress = 0;
            }
            this.scheduleNextBlink();
        }, this.blinkInterval);
        this.blinkInterval = 2000 + Math.random() * 3000;
    }

    lerp(a, b, t) {
        return a + (b - a) * t;
    }

    animate() {
        const now = Date.now();
        const deltaTime = (now - this.lastFrameTime) / 1000;
        this.lastFrameTime = now;

        // Update emotion transition
        if (this.emotionTransition < 1) {
            this.emotionTransition = Math.min(1, this.emotionTransition + deltaTime * 3);
            if (this.emotionTransition >= 1) {
                this.emotion = this.targetEmotion;
            }
        }

        // Update gaze (smooth follow)
        this.gazeX = this.lerp(this.gazeX, this.targetGazeX, deltaTime * 5);
        this.gazeY = this.lerp(this.gazeY, this.targetGazeY, deltaTime * 5);

        // Update blink
        if (this.isBlinking) {
            this.blinkProgress += deltaTime * 8;
            if (this.blinkProgress >= 1) {
                this.isBlinking = false;
                this.blinkProgress = 0;
            }
        }

        // Draw
        this.draw();

        // Next frame
        this.animationFrame = requestAnimationFrame(() => this.animate());
    }

    getEmotionParams() {
        const current = this.emotions[this.emotion];
        const target = this.emotions[this.targetEmotion];
        const t = this.emotionTransition;

        return {
            eyeHeight: this.lerp(current.eyeHeight, target.eyeHeight, t),
            eyeWidth: this.lerp(current.eyeWidth, target.eyeWidth, t),
            pupilSize: this.lerp(current.pupilSize, target.pupilSize, t),
            eyebrowAngle: this.lerp(current.eyebrowAngle, target.eyebrowAngle, t),
            eyebrowHeight: this.lerp(current.eyebrowHeight, target.eyebrowHeight, t),
            eyeOpenness: this.lerp(current.eyeOpenness, target.eyeOpenness, t),
            squint: target.squint,
            lookUp: target.lookUp,
            asymmetric: target.asymmetric,
            sparkle: target.sparkle
        };
    }

    draw() {
        const ctx = this.ctx;
        const params = this.getEmotionParams();

        // Clear canvas with background
        ctx.fillStyle = '#1a1a2e';
        ctx.fillRect(0, 0, this.width, this.height);

        // Calculate blink factor (0 = open, 1 = closed)
        let blinkFactor = 0;
        if (this.isBlinking) {
            // Quick close, slower open
            if (this.blinkProgress < 0.4) {
                blinkFactor = this.blinkProgress / 0.4;
            } else {
                blinkFactor = 1 - (this.blinkProgress - 0.4) / 0.6;
            }
        }

        // Draw both eyes
        this.drawEye(this.leftEyeX, this.eyeY, params, blinkFactor, false);
        this.drawEye(this.rightEyeX, this.eyeY, params, blinkFactor, true);

        // Draw eyebrows
        this.drawEyebrow(this.leftEyeX, this.eyeY - params.eyeHeight/2 - 15, params, false);
        this.drawEyebrow(this.rightEyeX, this.eyeY - params.eyeHeight/2 - 15, params, true);

        // Draw sparkles if excited
        if (params.sparkle && this.emotionTransition > 0.5) {
            this.drawSparkles();
        }
    }

    drawEye(x, y, params, blinkFactor, isRight) {
        const ctx = this.ctx;

        // Adjust for asymmetric (confused) emotion
        let eyeHeight = params.eyeHeight;
        let eyeWidth = params.eyeWidth;
        if (params.asymmetric && isRight) {
            eyeHeight *= 0.8;
        }

        // Apply blink
        const effectiveOpenness = params.eyeOpenness * (1 - blinkFactor * 0.9);
        eyeHeight *= effectiveOpenness;

        // Apply squint for happy
        if (params.squint) {
            eyeHeight *= 0.7;
        }

        // Eye white (rounded rectangle)
        ctx.fillStyle = '#ffffff';
        ctx.beginPath();
        this.roundRect(x - eyeWidth/2, y - eyeHeight/2, eyeWidth, eyeHeight, 15);
        ctx.fill();

        // Pupil position based on gaze
        let gazeOffsetX = (this.gazeX - 0.5) * eyeWidth * 0.4;
        let gazeOffsetY = (this.gazeY - 0.5) * eyeHeight * 0.4;

        // Looking up when thinking
        if (params.lookUp) {
            gazeOffsetY = -eyeHeight * 0.2;
            gazeOffsetX = (isRight ? 1 : -1) * eyeWidth * 0.1;
        }

        // Pupil
        const pupilX = x + gazeOffsetX;
        const pupilY = y + gazeOffsetY;

        // Outer pupil (dark)
        ctx.fillStyle = '#1a1a2e';
        ctx.beginPath();
        ctx.ellipse(pupilX, pupilY, params.pupilSize, params.pupilSize * 1.2, 0, 0, Math.PI * 2);
        ctx.fill();

        // Inner pupil highlight
        ctx.fillStyle = '#ffffff';
        ctx.beginPath();
        ctx.ellipse(pupilX - params.pupilSize * 0.3, pupilY - params.pupilSize * 0.3,
                    params.pupilSize * 0.25, params.pupilSize * 0.25, 0, 0, Math.PI * 2);
        ctx.fill();

        // Eyelid shadow (top)
        const gradient = ctx.createLinearGradient(x, y - eyeHeight/2, x, y - eyeHeight/4);
        gradient.addColorStop(0, 'rgba(0, 0, 0, 0.2)');
        gradient.addColorStop(1, 'rgba(0, 0, 0, 0)');
        ctx.fillStyle = gradient;
        ctx.beginPath();
        this.roundRect(x - eyeWidth/2, y - eyeHeight/2, eyeWidth, eyeHeight/2, 15);
        ctx.fill();
    }

    drawEyebrow(x, y, params, isRight) {
        const ctx = this.ctx;

        const browWidth = 40;
        const browThickness = 6;

        // Adjust angle - mirror for right eyebrow
        let angle = params.eyebrowAngle * (Math.PI / 180);
        if (isRight) angle = -angle;

        // Asymmetric adjustment for confused
        if (params.asymmetric && isRight) {
            angle += 15 * (Math.PI / 180);
        }

        ctx.save();
        ctx.translate(x, y + params.eyebrowHeight);
        ctx.rotate(angle);

        ctx.fillStyle = '#4a4a6a';
        ctx.beginPath();
        this.roundRect(-browWidth/2, -browThickness/2, browWidth, browThickness, 3);
        ctx.fill();

        ctx.restore();
    }

    drawSparkles() {
        const ctx = this.ctx;
        const time = Date.now() / 200;

        const sparklePositions = [
            { x: 50, y: 80 },
            { x: 190, y: 90 },
            { x: 70, y: 170 },
            { x: 170, y: 160 }
        ];

        sparklePositions.forEach((pos, i) => {
            const scale = 0.5 + Math.sin(time + i) * 0.3;
            this.drawSparkle(pos.x, pos.y, 8 * scale);
        });
    }

    drawSparkle(x, y, size) {
        const ctx = this.ctx;
        ctx.fillStyle = '#ffdd44';

        // 4-point star
        ctx.beginPath();
        for (let i = 0; i < 4; i++) {
            const angle = (i / 4) * Math.PI * 2 - Math.PI / 2;
            const nextAngle = ((i + 0.5) / 4) * Math.PI * 2 - Math.PI / 2;

            ctx.lineTo(x + Math.cos(angle) * size, y + Math.sin(angle) * size);
            ctx.lineTo(x + Math.cos(nextAngle) * size * 0.3, y + Math.sin(nextAngle) * size * 0.3);
        }
        ctx.closePath();
        ctx.fill();
    }

    roundRect(x, y, width, height, radius) {
        const ctx = this.ctx;
        ctx.moveTo(x + radius, y);
        ctx.lineTo(x + width - radius, y);
        ctx.quadraticCurveTo(x + width, y, x + width, y + radius);
        ctx.lineTo(x + width, y + height - radius);
        ctx.quadraticCurveTo(x + width, y + height, x + width - radius, y + height);
        ctx.lineTo(x + radius, y + height);
        ctx.quadraticCurveTo(x, y + height, x, y + height - radius);
        ctx.lineTo(x, y + radius);
        ctx.quadraticCurveTo(x, y, x + radius, y);
    }
}

// MediaPipe Face Tracking
class FaceTracker {
    constructor(lunaFace) {
        this.lunaFace = lunaFace;
        this.videoElement = document.getElementById('camera-feed');
        this.faceDetection = null;
        this.isRunning = false;
    }

    async start() {
        try {
            // Get camera stream
            const stream = await navigator.mediaDevices.getUserMedia({
                video: { width: 640, height: 480, facingMode: 'user' }
            });
            this.videoElement.srcObject = stream;
            await this.videoElement.play();

            // Initialize MediaPipe Face Detection
            this.faceDetection = new FaceDetection({
                locateFile: (file) => {
                    return `https://cdn.jsdelivr.net/npm/@mediapipe/face_detection/${file}`;
                }
            });

            this.faceDetection.setOptions({
                model: 'short',
                minDetectionConfidence: 0.5
            });

            this.faceDetection.onResults((results) => this.onResults(results));

            // Start processing
            this.isRunning = true;
            this.processFrame();

            console.log('Face tracking started');
        } catch (error) {
            console.error('Failed to start face tracking:', error);
        }
    }

    async processFrame() {
        if (!this.isRunning) return;

        if (this.videoElement.readyState >= 2) {
            await this.faceDetection.send({ image: this.videoElement });
        }

        requestAnimationFrame(() => this.processFrame());
    }

    onResults(results) {
        if (results.detections && results.detections.length > 0) {
            const detection = results.detections[0];
            const bbox = detection.boundingBox;

            // Calculate face center (0-1 normalized)
            const faceCenterX = bbox.xCenter;
            const faceCenterY = bbox.yCenter;

            // Mirror the X axis (camera is mirrored)
            // If user is on left of camera (faceCenterX < 0.5),
            // eyes should look to their right (which is screen's left)
            // So we invert: mirroredX = 1 - faceCenterX
            const mirroredX = 1 - faceCenterX;

            // Set gaze - eyes follow the user
            this.lunaFace.setGaze(mirroredX, faceCenterY);
        }
    }

    stop() {
        this.isRunning = false;
        if (this.videoElement.srcObject) {
            this.videoElement.srcObject.getTracks().forEach(track => track.stop());
        }
    }
}

// Initialize
let lunaFace = null;
let faceTracker = null;

document.addEventListener('DOMContentLoaded', () => {
    lunaFace = new LunaFace('face-canvas');
    faceTracker = new FaceTracker(lunaFace);

    // Start face tracking
    faceTracker.start();

    // Listen for emotion messages from parent window (for iframe embedding)
    window.addEventListener('message', (event) => {
        if (event.data && event.data.type === 'setEmotion') {
            lunaFace.setEmotion(event.data.emotion);
        }
        if (event.data && event.data.type === 'setGaze') {
            lunaFace.setGaze(event.data.x, event.data.y);
        }
    });

    // Expose for direct control
    window.lunaFace = lunaFace;
    window.faceTracker = faceTracker;
});

// Test emotions with keyboard (for development)
document.addEventListener('keydown', (e) => {
    const emotionMap = {
        '1': 'neutral',
        '2': 'happy',
        '3': 'sad',
        '4': 'angry',
        '5': 'surprised',
        '6': 'thinking',
        '7': 'confused',
        '8': 'excited'
    };

    if (emotionMap[e.key] && lunaFace) {
        lunaFace.setEmotion(emotionMap[e.key]);
        console.log('Emotion set to:', emotionMap[e.key]);
    }
});
