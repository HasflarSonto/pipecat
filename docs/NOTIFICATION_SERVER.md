# Luna Notification Server Specification

## Overview

The notification server receives push notifications from the iOS companion app and forwards them to connected Luna devices (ESP32 or simulator) via WebSocket.

## Architecture

```
iOS App ──POST──► /api/notify ──► Notification Queue ──► WebSocket ──► Device
                       │
                       ▼
                  Filtering/Priority
```

## HTTP API

### POST `/api/control`

Remote control Luna display modes from iOS app.

**Headers:**
```
Content-Type: application/json
Authorization: Bearer <API_KEY>
```

**Request Body:**
```json
{
  "cmd": "mode",
  "mode": "clock"
}
```

**Available Commands:**

| cmd | Parameters | Description |
|-----|------------|-------------|
| `mode` | `mode`: face, clock, weather, timer, calendar, subway | Switch display mode |
| `emotion` | `emotion`: happy, sad, neutral, etc. | Set face emotion |
| `timer_start` | - | Start the timer |
| `timer_pause` | - | Pause the timer |
| `timer_reset` | `minutes`: int | Reset timer to minutes |
| `weather` | `temp`, `icon`, `desc` | Update weather display |
| `calendar` | `events`: array | Update calendar events |

**Example - Switch to Clock:**
```json
{"cmd": "mode", "mode": "clock"}
```

**Example - Set Timer:**
```json
{"cmd": "timer_reset", "minutes": 25}
```

**Example - Update Weather:**
```json
{
  "cmd": "weather",
  "temp": "72°F",
  "icon": "sunny",
  "desc": "Clear skies"
}
```

**Response:**
```json
{
  "status": "ok",
  "forwarded": true
}
```

---

### POST `/api/notify`

Receives notifications from the iOS app.

**Headers:**
```
Content-Type: application/json
Authorization: Bearer <API_KEY>
```

**Request Body:**
```json
{
  "id": "uuid-string",
  "app_id": "com.apple.mobilemail",
  "app_name": "Mail",
  "title": "John Doe",
  "body": "Hey, are you free for lunch?",
  "subtitle": "Re: Meeting tomorrow",
  "timestamp": "2024-01-22T10:30:00Z",
  "priority": "normal",
  "category": "message",
  "thread_id": "thread-123",
  "attachments": {
    "has_image": false,
    "has_action": true
  }
}
```

**Response:**
```json
{
  "status": "accepted",
  "id": "uuid-string",
  "queued": true
}
```

**Error Response:**
```json
{
  "status": "error",
  "code": "UNAUTHORIZED",
  "message": "Invalid API key"
}
```

### GET `/api/notify/status`

Check server status and connection info.

**Response:**
```json
{
  "status": "ok",
  "devices_connected": 1,
  "queue_length": 3,
  "dnd_active": false
}
```

## Data Model

### Notification

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Unique notification ID (UUID) |
| `app_id` | string | Yes | iOS bundle identifier |
| `app_name` | string | Yes | Human-readable app name |
| `title` | string | Yes | Notification title |
| `body` | string | No | Notification body text |
| `subtitle` | string | No | Subtitle (if present) |
| `timestamp` | ISO8601 | Yes | When notification was received |
| `priority` | enum | No | `low`, `normal`, `high`, `critical` |
| `category` | string | No | Notification category |
| `thread_id` | string | No | Thread/conversation ID |
| `attachments` | object | No | Attachment metadata |

### Priority Levels

| Priority | Behavior |
|----------|----------|
| `low` | Silent, queue only |
| `normal` | Visual notification on device |
| `high` | Visual + sound, interrupts current mode |
| `critical` | Always display, even in DND |

## WebSocket Protocol

### Server to Device

**New Notification:**
```json
{
  "cmd": "notification",
  "id": "uuid-string",
  "app": "Mail",
  "app_icon": "mail",
  "title": "John Doe",
  "body": "Hey, are you free for lunch?",
  "timestamp": "10:30 AM",
  "priority": "normal"
}
```

**Notification Dismissed (from another device):**
```json
{
  "cmd": "notification_dismiss",
  "id": "uuid-string"
}
```

**Notification Queue Update:**
```json
{
  "cmd": "notification_queue",
  "count": 5,
  "notifications": [
    {"id": "...", "app": "Mail", "title": "John Doe", "preview": "Hey..."},
    {"id": "...", "app": "Messages", "title": "Mom", "preview": "Call me..."}
  ]
}
```

### Device to Server

**Dismiss Notification:**
```json
{
  "cmd": "notification_dismiss",
  "id": "uuid-string"
}
```

**Clear All Notifications:**
```json
{
  "cmd": "notification_clear_all"
}
```

## Implementation in `my_bot.py`

### New Components

```python
# notification_handler.py

from dataclasses import dataclass
from datetime import datetime
from typing import Optional, List
from collections import deque
import asyncio

@dataclass
class Notification:
    id: str
    app_id: str
    app_name: str
    title: str
    body: Optional[str]
    timestamp: datetime
    priority: str = "normal"
    category: Optional[str] = None

class NotificationQueue:
    def __init__(self, max_size: int = 50):
        self._queue: deque[Notification] = deque(maxlen=max_size)
        self._listeners: List[asyncio.Queue] = []

    async def add(self, notification: Notification):
        self._queue.append(notification)
        # Notify all listeners
        for listener in self._listeners:
            await listener.put(notification)

    def dismiss(self, notification_id: str):
        self._queue = deque(
            [n for n in self._queue if n.id != notification_id],
            maxlen=self._queue.maxlen
        )

    def get_all(self) -> List[Notification]:
        return list(self._queue)

    def subscribe(self) -> asyncio.Queue:
        q = asyncio.Queue()
        self._listeners.append(q)
        return q
```

### HTTP Endpoint (FastAPI)

```python
from fastapi import FastAPI, HTTPException, Header
from pydantic import BaseModel

app = FastAPI()
notification_queue = NotificationQueue()

API_KEY = os.environ.get("LUNA_NOTIFY_API_KEY", "dev-key-change-me")

class NotificationRequest(BaseModel):
    id: str
    app_id: str
    app_name: str
    title: str
    body: Optional[str] = None
    subtitle: Optional[str] = None
    timestamp: str
    priority: str = "normal"
    category: Optional[str] = None

@app.post("/api/notify")
async def receive_notification(
    notification: NotificationRequest,
    authorization: str = Header(...)
):
    # Validate API key
    if not authorization.startswith("Bearer "):
        raise HTTPException(401, "Invalid authorization header")

    token = authorization[7:]
    if token != API_KEY:
        raise HTTPException(401, "Invalid API key")

    # Add to queue
    notif = Notification(
        id=notification.id,
        app_id=notification.app_id,
        app_name=notification.app_name,
        title=notification.title,
        body=notification.body,
        timestamp=datetime.fromisoformat(notification.timestamp.replace('Z', '+00:00')),
        priority=notification.priority,
        category=notification.category
    )

    await notification_queue.add(notif)

    return {"status": "accepted", "id": notification.id, "queued": True}
```

### Integration with WebSocket Handler

```python
# In the ESP32 WebSocket handler

async def forward_notification_to_device(ws, notification: Notification):
    """Send notification to connected ESP32/simulator"""
    await ws.send_json({
        "cmd": "notification",
        "id": notification.id,
        "app": notification.app_name,
        "app_icon": get_app_icon(notification.app_id),
        "title": notification.title,
        "body": notification.body or "",
        "timestamp": notification.timestamp.strftime("%I:%M %p"),
        "priority": notification.priority
    })

def get_app_icon(app_id: str) -> str:
    """Map app bundle ID to icon name"""
    icons = {
        "com.apple.mobilemail": "mail",
        "com.apple.MobileSMS": "message",
        "com.apple.mobilecal": "calendar",
        "com.slack.Slack": "slack",
        # Add more mappings
    }
    return icons.get(app_id, "app")
```

## Filtering & Do Not Disturb

### App Filtering

```python
# Configuration
BLOCKED_APPS = {"com.facebook.Facebook", "com.instagram.Instagram"}
PRIORITY_APPS = {"com.apple.mobilemail", "com.apple.MobileSMS"}

def should_forward(notification: Notification) -> bool:
    if notification.app_id in BLOCKED_APPS:
        return False
    return True

def get_effective_priority(notification: Notification) -> str:
    if notification.app_id in PRIORITY_APPS:
        return "high" if notification.priority == "normal" else notification.priority
    return notification.priority
```

### Do Not Disturb

```python
class DNDManager:
    def __init__(self):
        self.enabled = False
        self.allow_critical = True
        self.scheduled_start: Optional[time] = None
        self.scheduled_end: Optional[time] = None

    def is_active(self) -> bool:
        if not self.enabled:
            return False
        # Check schedule if configured
        if self.scheduled_start and self.scheduled_end:
            now = datetime.now().time()
            return self.scheduled_start <= now <= self.scheduled_end
        return self.enabled

    def should_deliver(self, notification: Notification) -> bool:
        if not self.is_active():
            return True
        if self.allow_critical and notification.priority == "critical":
            return True
        return False
```

## Security

### API Key Management

- API key should be generated per-device during iOS app setup
- Store in environment variable `LUNA_NOTIFY_API_KEY`
- Support key rotation via `/api/notify/rotate-key` endpoint

### Rate Limiting

```python
from slowapi import Limiter
from slowapi.util import get_remote_address

limiter = Limiter(key_func=get_remote_address)

@app.post("/api/notify")
@limiter.limit("60/minute")
async def receive_notification(...):
    ...
```

## Configuration

Environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `LUNA_NOTIFY_API_KEY` | - | API key for iOS app authentication |
| `LUNA_NOTIFY_PORT` | 8080 | HTTP API port |
| `LUNA_NOTIFY_MAX_QUEUE` | 50 | Max notifications in queue |
| `LUNA_DND_ENABLED` | false | Enable Do Not Disturb |

## Testing

### curl Example

```bash
curl -X POST http://localhost:8080/api/notify \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer dev-key-change-me" \
  -d '{
    "id": "test-123",
    "app_id": "com.apple.mobilemail",
    "app_name": "Mail",
    "title": "Test Notification",
    "body": "This is a test",
    "timestamp": "2024-01-22T10:30:00Z",
    "priority": "normal"
  }'
```
