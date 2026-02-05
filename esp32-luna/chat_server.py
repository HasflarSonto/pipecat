#!/usr/bin/env python3
"""
Luna Chat Server for ESP32-Luna

WebSocket server that:
1. Accepts terminal input
2. Sends to Claude API with tools
3. Fetches REAL data (weather, time, subway)
4. Broadcasts to connected ESP32

Usage:
    python chat_server.py [--port 7860]

Requires:
    pip install websockets anthropic aiohttp pytz gtfs-realtime-bindings
    pip install google-auth-oauthlib google-api-python-client
"""

import asyncio
import json
import argparse
import sys
import os
import time as time_module
from typing import Set, Dict, Any, Optional
from pathlib import Path
from datetime import datetime

# Load .env file manually (no dependencies)
def load_env_file(path):
    if not path.exists():
        return False
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#') and '=' in line:
                key, value = line.split('=', 1)
                os.environ[key.strip()] = value.strip()
    return True

env_path = Path("/Users/antonioli/Desktop/pipecat/.env")
if load_env_file(env_path):
    print(f"Loaded env from: {env_path}")

try:
    import websockets
    from websockets.server import serve
except ImportError:
    print("Error: websockets not installed. Run: pip install websockets")
    sys.exit(1)

try:
    import anthropic
except ImportError:
    print("Error: anthropic not installed. Run: pip install anthropic")
    sys.exit(1)

try:
    import aiohttp
except ImportError:
    print("Error: aiohttp not installed. Run: pip install aiohttp")
    sys.exit(1)

try:
    import pytz
except ImportError:
    print("Error: pytz not installed. Run: pip install pytz")
    sys.exit(1)

# Google API (optional - will warn if not installed)
gmail_service = None
calendar_service = None
GOOGLE_API_AVAILABLE = False
try:
    from google.oauth2.credentials import Credentials
    from google_auth_oauthlib.flow import InstalledAppFlow
    from google.auth.transport.requests import Request
    from googleapiclient.discovery import build
    GOOGLE_API_AVAILABLE = True
except ImportError:
    print("Warning: Google API dependencies not installed. Run: pip install google-auth-oauthlib google-api-python-client")

# OAuth scopes (Gmail + Calendar)
GOOGLE_SCOPES = [
    'https://www.googleapis.com/auth/gmail.readonly',
    'https://www.googleapis.com/auth/calendar.readonly'
]
GOOGLE_TOKEN_PATH = Path(__file__).parent / "google_token.json"


def init_google_services():
    """Initialize Gmail and Calendar API services with OAuth."""
    global gmail_service, calendar_service

    if not GOOGLE_API_AVAILABLE:
        return False

    client_id = os.environ.get('GMAIL_CLIENT_ID')
    client_secret = os.environ.get('GMAIL_CLIENT_SECRET')

    if not client_id or not client_secret:
        print("Warning: GMAIL_CLIENT_ID or GMAIL_CLIENT_SECRET not set in .env")
        return False

    creds = None

    # Load existing token if available
    if GOOGLE_TOKEN_PATH.exists():
        try:
            creds = Credentials.from_authorized_user_file(str(GOOGLE_TOKEN_PATH), GOOGLE_SCOPES)
        except Exception as e:
            print(f"Error loading Google token: {e}")

    # Refresh or get new credentials
    if not creds or not creds.valid:
        if creds and creds.expired and creds.refresh_token:
            try:
                creds.refresh(Request())
            except Exception as e:
                print(f"Error refreshing Google token: {e}")
                creds = None

        if not creds:
            # Need to do OAuth flow
            flow = InstalledAppFlow.from_client_config(
                {
                    "installed": {
                        "client_id": client_id,
                        "client_secret": client_secret,
                        "auth_uri": "https://accounts.google.com/o/oauth2/auth",
                        "token_uri": "https://oauth2.googleapis.com/token",
                        "redirect_uris": ["http://localhost"]
                    }
                },
                GOOGLE_SCOPES
            )
            print("\n" + "="*50)
            print("Google Authorization Required (Gmail + Calendar)")
            print("A browser window will open for you to sign in...")
            print("="*50 + "\n")
            creds = flow.run_local_server(port=0)

        # Save credentials for next time
        with open(GOOGLE_TOKEN_PATH, 'w') as f:
            f.write(creds.to_json())
        print(f"Google token saved to: {GOOGLE_TOKEN_PATH}")

    try:
        gmail_service = build('gmail', 'v1', credentials=creds)
        calendar_service = build('calendar', 'v3', credentials=creds)
        print("Gmail and Calendar APIs initialized successfully")
        return True
    except Exception as e:
        print(f"Error building Google services: {e}")
        return False


async def fetch_gmail_notifications(max_results: int = 5) -> Dict[str, Any]:
    """Fetch unread emails from Gmail."""
    global gmail_service

    if not gmail_service:
        init_google_services()

    if not gmail_service:
        return {
            "count": 0,
            "emails": [],
            "error": "Gmail not configured"
        }

    try:
        # Get unread messages
        results = gmail_service.users().messages().list(
            userId='me',
            q='is:unread',
            maxResults=max_results
        ).execute()

        messages = results.get('messages', [])
        emails = []

        for msg in messages:
            msg_data = gmail_service.users().messages().get(
                userId='me',
                id=msg['id'],
                format='metadata',
                metadataHeaders=['From', 'Subject']
            ).execute()

            headers = {h['name']: h['value'] for h in msg_data.get('payload', {}).get('headers', [])}

            # Parse sender name
            from_header = headers.get('From', 'Unknown')
            if '<' in from_header:
                sender = from_header.split('<')[0].strip().strip('"')
            else:
                sender = from_header.split('@')[0]

            # Truncate for display
            subject = headers.get('Subject', '(No subject)')
            if len(subject) > 40:
                subject = subject[:37] + "..."
            if len(sender) > 20:
                sender = sender[:17] + "..."

            emails.append({
                "sender": sender,
                "subject": subject
            })

        return {
            "count": len(emails),
            "emails": emails,
            "error": None
        }

    except Exception as e:
        print(f"Gmail API error: {e}")
        return {
            "count": 0,
            "emails": [],
            "error": str(e)
        }


async def fetch_calendar_events(max_results: int = 5) -> Dict[str, Any]:
    """Fetch today's calendar events from Google Calendar."""
    global calendar_service

    if not calendar_service:
        init_google_services()

    if not calendar_service:
        return {
            "count": 0,
            "events": [],
            "error": "Calendar not configured"
        }

    try:
        # Get today's date range in local timezone
        tz = pytz.timezone("America/New_York")
        now = datetime.now(tz)
        start_of_day = now.replace(hour=0, minute=0, second=0, microsecond=0)
        end_of_day = now.replace(hour=23, minute=59, second=59, microsecond=999999)

        # Format for Google Calendar API
        time_min = start_of_day.isoformat()
        time_max = end_of_day.isoformat()

        # Fetch events
        events_result = calendar_service.events().list(
            calendarId='primary',
            timeMin=time_min,
            timeMax=time_max,
            maxResults=max_results,
            singleEvents=True,
            orderBy='startTime'
        ).execute()

        items = events_result.get('items', [])
        events = []

        for event in items:
            # Get start time
            start = event.get('start', {})
            if 'dateTime' in start:
                event_time = datetime.fromisoformat(start['dateTime'].replace('Z', '+00:00'))
                event_time = event_time.astimezone(tz)
                time_str = event_time.strftime("%I:%M %p").lstrip('0')
            else:
                # All-day event
                time_str = "All day"

            # Get title and location
            title = event.get('summary', '(No title)')
            location = event.get('location', '')

            # Truncate for display
            if len(title) > 40:
                title = title[:37] + "..."
            if len(location) > 30:
                location = location[:27] + "..."

            events.append({
                "time_str": time_str,
                "title": title,
                "location": location
            })

        return {
            "count": len(events),
            "events": events,
            "error": None
        }

    except Exception as e:
        print(f"Calendar API error: {e}")
        return {
            "count": 0,
            "events": [],
            "error": str(e)
        }


# Connected ESP32 clients
connected_clients: Set[websockets.WebSocketServerProtocol] = set()

# Anthropic client (initialized in main)
claude_client = None


# ============== REAL DATA FETCHERS ==============

# Weather code to icon mapping for ESP32
WEATHER_CODE_TO_ICON = {
    0: "sunny",           # Clear sky
    1: "sunny",           # Mainly clear
    2: "partly_cloudy",   # Partly cloudy
    3: "cloudy",          # Overcast
    45: "foggy", 48: "foggy",
    51: "rainy", 53: "rainy", 55: "rainy",  # Drizzle
    61: "rainy", 63: "rainy", 65: "rainy",  # Rain
    71: "snowy", 73: "snowy", 75: "snowy",  # Snow
    80: "rainy", 81: "rainy", 82: "rainy",  # Rain showers
    95: "stormy", 96: "stormy", 99: "stormy",  # Thunderstorm
}

WEATHER_CODE_TO_DESC = {
    0: "Clear sky",
    1: "Mainly clear", 2: "Partly cloudy", 3: "Overcast",
    45: "Foggy", 48: "Rime fog",
    51: "Light drizzle", 53: "Moderate drizzle", 55: "Dense drizzle",
    61: "Slight rain", 63: "Moderate rain", 65: "Heavy rain",
    71: "Slight snow", 73: "Moderate snow", 75: "Heavy snow",
    80: "Rain showers", 81: "Moderate showers", 82: "Heavy showers",
    95: "Thunderstorm", 96: "Thunderstorm with hail", 99: "Severe thunderstorm",
}


async def fetch_real_weather(location: str = "New York") -> Dict[str, Any]:
    """Fetch real weather from Open-Meteo API (free, no API key)."""
    try:
        async with aiohttp.ClientSession() as session:
            # Geocode location
            geo_url = f"https://geocoding-api.open-meteo.com/v1/search?name={location}&count=1"
            async with session.get(geo_url) as resp:
                geo_data = await resp.json()

            if not geo_data.get("results"):
                return {"temp": "??°F", "icon": "cloudy", "desc": f"Unknown: {location}"}

            lat = geo_data["results"][0]["latitude"]
            lon = geo_data["results"][0]["longitude"]
            city = geo_data["results"][0]["name"]

            # Get weather
            weather_url = f"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current=temperature_2m,weather_code&temperature_unit=fahrenheit"
            async with session.get(weather_url) as resp:
                weather_data = await resp.json()

            current = weather_data.get("current", {})
            temp = current.get("temperature_2m", 0)
            code = current.get("weather_code", 0)

            return {
                "temp": f"{int(temp)}°F",
                "icon": WEATHER_CODE_TO_ICON.get(code, "cloudy"),
                "desc": WEATHER_CODE_TO_DESC.get(code, "Unknown"),
                "city": city
            }
    except Exception as e:
        print(f"Weather API error: {e}")
        return {"temp": "??°F", "icon": "cloudy", "desc": "Error fetching weather"}


def get_real_time(timezone: str = "America/New_York") -> Dict[str, Any]:
    """Get real current time for a timezone."""
    try:
        tz = pytz.timezone(timezone)
        now = datetime.now(tz)
        return {
            "hours": now.hour,
            "minutes": now.minute,
            "is_24h": False,
            "formatted": now.strftime("%I:%M %p")
        }
    except Exception as e:
        print(f"Time error: {e}")
        now = datetime.now()
        return {"hours": now.hour, "minutes": now.minute, "is_24h": False}


# MTA Line colors
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

# Stop IDs for common stations
MTA_STOP_IDS = {
    "110 St": {"1": {"N": "117N", "S": "117S"}},
    "116 St": {"1": {"N": "116N", "S": "116S"}},
    "125 St": {"1": {"N": "115N", "S": "115S"}},
    "Times Sq": {"1": {"N": "127N", "S": "127S"}, "N": {"N": "R16N", "S": "R16S"}},
    "14 St": {"1": {"N": "132N", "S": "132S"}, "A": {"N": "A31N", "S": "A31S"}},
}


async def fetch_real_subway(line: str = "1", station: str = "110 St", direction: str = "downtown") -> Dict[str, Any]:
    """Fetch real MTA subway arrival times using GTFS-Realtime."""
    line = line.upper()
    dir_suffix = "S" if direction.lower() in ["downtown", "south", "southbound", "s"] else "N"
    dir_name = "Downtown" if dir_suffix == "S" else "Uptown"
    line_color = MTA_LINE_COLORS.get(line, "#FFFFFF")

    # Determine feed URL based on line
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

    # Get stop ID
    stop_id = None
    if station in MTA_STOP_IDS:
        station_stops = MTA_STOP_IDS[station]
        if line in station_stops:
            stop_id = station_stops[line].get(dir_suffix)
        elif "1" in station_stops and line in ["1", "2", "3"]:
            stop_id = station_stops["1"].get(dir_suffix)

    if not stop_id:
        stop_id = "117S" if dir_suffix == "S" else "117N"
        station = "110 St"

    try:
        from google.transit import gtfs_realtime_pb2

        async with aiohttp.ClientSession() as session:
            async with session.get(feed_url) as resp:
                if resp.status != 200:
                    raise Exception(f"MTA API returned {resp.status}")
                data = await resp.read()

        feed = gtfs_realtime_pb2.FeedMessage()
        feed.ParseFromString(data)

        now = time_module.time()
        arrivals = []

        for entity in feed.entity:
            if not entity.HasField("trip_update"):
                continue

            trip = entity.trip_update
            if trip.trip.route_id != line:
                continue

            for stop_time in trip.stop_time_update:
                if stop_time.stop_id == stop_id:
                    arrival_time = stop_time.arrival.time if stop_time.HasField("arrival") else stop_time.departure.time
                    if arrival_time > now:
                        minutes = int((arrival_time - now) / 60)
                        if 0 <= minutes <= 60:
                            arrivals.append(minutes)

        arrivals.sort()
        arrivals = arrivals[:3]

        if not arrivals:
            arrivals = [5, 12, 20]  # Fallback

        return {
            "line": line,
            "color": line_color,
            "station": station,
            "direction": dir_name,
            "times": arrivals
        }

    except ImportError:
        print("gtfs-realtime-bindings not installed, using demo data")
        return {
            "line": line,
            "color": line_color,
            "station": station,
            "direction": dir_name,
            "times": [3, 8, 15]  # Demo data
        }
    except Exception as e:
        print(f"MTA API error: {e}")
        return {
            "line": line,
            "color": line_color,
            "station": station,
            "direction": dir_name,
            "times": [5, 10, 18]  # Fallback data
        }


# ============== TOOLS DEFINITION ==============

TOOLS = [
    {
        "name": "show_weather",
        "description": "Show weather information on the ESP32 display. Use this when the user asks about weather. You can specify a location.",
        "input_schema": {
            "type": "object",
            "properties": {
                "location": {
                    "type": "string",
                    "description": "City name, e.g., 'New York', 'Boston', 'Los Angeles'"
                }
            },
            "required": ["location"]
        }
    },
    {
        "name": "show_clock",
        "description": "Show the current time on the ESP32 display. Use this when the user asks what time it is.",
        "input_schema": {
            "type": "object",
            "properties": {
                "timezone": {
                    "type": "string",
                    "description": "Timezone, e.g., 'America/New_York', 'America/Los_Angeles', 'Europe/London'"
                }
            }
        }
    },
    {
        "name": "show_timer",
        "description": "Show a countdown timer. Use this when the user wants to set a timer.",
        "input_schema": {
            "type": "object",
            "properties": {
                "minutes": {
                    "type": "integer",
                    "description": "Minutes for the timer"
                },
                "label": {
                    "type": "string",
                    "description": "Timer label, e.g., 'Focus', 'Break', 'Cooking'"
                }
            },
            "required": ["minutes"]
        }
    },
    {
        "name": "show_subway",
        "description": "Show NYC subway arrival times. Use for MTA transit information.",
        "input_schema": {
            "type": "object",
            "properties": {
                "line": {
                    "type": "string",
                    "description": "Train line: 1, 2, 3, 4, 5, 6, 7, A, C, E, B, D, F, M, G, J, Z, L, N, Q, R, W"
                },
                "station": {
                    "type": "string",
                    "description": "Station name: '110 St', '116 St', '125 St', 'Times Sq', '14 St'"
                },
                "direction": {
                    "type": "string",
                    "description": "Direction: 'downtown' or 'uptown'"
                }
            },
            "required": ["line"]
        }
    },
    {
        "name": "show_face",
        "description": "Return to the animated face display. Use when done showing other screens.",
        "input_schema": {
            "type": "object",
            "properties": {}
        }
    },
    {
        "name": "set_emotion",
        "description": "Set Luna's facial expression/emotion.",
        "input_schema": {
            "type": "object",
            "properties": {
                "emotion": {
                    "type": "string",
                    "enum": ["neutral", "happy", "sad", "angry", "surprised", "thinking", "confused", "excited", "cat"],
                    "description": "The emotion to display"
                }
            },
            "required": ["emotion"]
        }
    },
    {
        "name": "show_notifications",
        "description": "Show Gmail notifications/unread emails on the ESP32 display. Use when user asks about emails, notifications, or messages.",
        "input_schema": {
            "type": "object",
            "properties": {
                "max_results": {
                    "type": "integer",
                    "description": "Maximum number of emails to show (1-5, default 3)"
                }
            }
        }
    },
    {
        "name": "show_calendar",
        "description": "Show today's calendar events/schedule from Google Calendar. Use when user asks about their calendar, schedule, meetings, or events for today.",
        "input_schema": {
            "type": "object",
            "properties": {
                "max_results": {
                    "type": "integer",
                    "description": "Maximum number of events to show (1-5, default 5)"
                }
            }
        }
    }
]


# ============== ESP32 COMMUNICATION ==============

async def send_esp32_command(cmd: dict):
    """Send a command to all connected ESP32 devices."""
    if not connected_clients:
        print("[No ESP32 connected]")
        return

    message = json.dumps(cmd)
    disconnected = set()
    for client in connected_clients:
        try:
            await client.send(message)
        except websockets.exceptions.ConnectionClosed:
            disconnected.add(client)

    connected_clients.difference_update(disconnected)
    print(f"[Sent: {cmd.get('cmd', 'unknown')}]")


async def handle_tool_call(tool_name: str, tool_input: dict) -> str:
    """Handle a tool call from Claude - fetch real data and send to ESP32."""

    if tool_name == "show_weather":
        location = tool_input.get("location", "New York")
        print(f"  [Fetching weather for {location}...]")
        weather = await fetch_real_weather(location)
        await send_esp32_command({
            "cmd": "weather",
            "temp": weather["temp"],
            "icon": weather["icon"],
            "desc": weather["desc"]
        })
        return f"Weather in {weather.get('city', location)}: {weather['temp']}, {weather['desc']}"

    elif tool_name == "show_clock":
        timezone = tool_input.get("timezone", "America/New_York")
        print(f"  [Getting time for {timezone}...]")
        time_data = get_real_time(timezone)
        await send_esp32_command({
            "cmd": "clock",
            "hours": time_data["hours"],
            "minutes": time_data["minutes"],
            "is_24h": time_data["is_24h"]
        })
        return f"Current time: {time_data['formatted']}"

    elif tool_name == "show_timer":
        minutes = tool_input.get("minutes", 5)
        label = tool_input.get("label", "Timer")
        await send_esp32_command({
            "cmd": "timer",
            "minutes": minutes,
            "seconds": 0,
            "label": label,
            "running": False
        })
        return f"Timer set: {minutes} minutes ({label})"

    elif tool_name == "show_subway":
        line = tool_input.get("line", "1")
        station = tool_input.get("station", "110 St")
        direction = tool_input.get("direction", "downtown")
        print(f"  [Fetching {line} train times at {station}...]")
        subway = await fetch_real_subway(line, station, direction)
        await send_esp32_command({
            "cmd": "subway",
            "line": subway["line"],
            "color": subway["color"],
            "station": subway["station"],
            "direction": subway["direction"],
            "times": subway["times"]
        })
        times_str = ", ".join(str(t) for t in subway["times"])
        return f"Next {subway['line']} trains at {subway['station']} {subway['direction']}: {times_str} min"

    elif tool_name == "show_face":
        await send_esp32_command({"cmd": "clear_display"})
        return "Returned to face display"

    elif tool_name == "set_emotion":
        emotion = tool_input.get("emotion", "neutral")
        await send_esp32_command({
            "cmd": "emotion",
            "value": emotion
        })
        return f"Set emotion: {emotion}"

    elif tool_name == "show_notifications":
        max_results = min(tool_input.get("max_results", 3), 5)
        print(f"  [Fetching Gmail notifications...]")
        gmail_data = await fetch_gmail_notifications(max_results)

        if gmail_data.get("error"):
            return f"Gmail error: {gmail_data['error']}"

        if gmail_data["count"] == 0:
            await send_esp32_command({
                "cmd": "text",
                "content": "No unread emails!",
                "size": "large",
                "color": "#00FF00",
                "bg": "#1E1E28"
            })
            return "No unread emails"

        # Format for calendar-style display on ESP32
        events = []
        for email in gmail_data["emails"][:3]:  # Max 3 for calendar display
            events.append({
                "time_str": email["sender"],
                "title": email["subject"],
                "location": ""
            })

        await send_esp32_command({
            "cmd": "notifications",
            "events": events
        })
        return f"Showing {gmail_data['count']} unread email(s)"

    elif tool_name == "show_calendar":
        max_results = min(tool_input.get("max_results", 5), 5)
        print(f"  [Fetching calendar events...]")
        calendar_data = await fetch_calendar_events(max_results)

        if calendar_data.get("error"):
            return f"Calendar error: {calendar_data['error']}"

        if calendar_data["count"] == 0:
            await send_esp32_command({
                "cmd": "text",
                "content": "No events today!",
                "size": "large",
                "color": "#00FF00",
                "bg": "#1E1E28"
            })
            return "No calendar events today"

        # Format for calendar display on ESP32
        events = []
        for event in calendar_data["events"][:5]:  # Max 5 for calendar display
            events.append({
                "time_str": event["time_str"],
                "title": event["title"],
                "location": event["location"]
            })

        await send_esp32_command({
            "cmd": "calendar",
            "events": events
        })
        return f"Showing {calendar_data['count']} calendar event(s)"

    return "Unknown tool"


async def broadcast_text(text: str):
    """Send text caption to ESP32 - truncate to fit screen without scrollbar."""
    # ~60 chars fits on screen with medium font without scrolling
    truncated = text[:60]
    if len(text) > 60:
        truncated = text[:57] + "..."
    await send_esp32_command({
        "cmd": "text",
        "content": truncated,
        "size": "medium",
        "color": "#FFFFFF",
        "bg": "#1E1E28"
    })


# ============== CLAUDE API ==============

async def get_claude_response(user_input: str) -> str:
    """Get response from Claude API with tools."""
    try:
        messages = [{"role": "user", "content": user_input}]

        response = claude_client.messages.create(
            model="claude-sonnet-4-20250514",
            max_tokens=60,
            system="""You are Luna, a friendly robot on a tiny ESP32 screen.

RULES:
- ALL responses: ONE short sentence max (under 50 characters)
- Tool actions: Just "Here!" or "There you go!"
- Casual chat: Brief but warm, like "Doing great, thanks!"
- Default: NYC weather, 1 train 110 St downtown
- NEVER use emojis in responses""",
            tools=TOOLS,
            messages=messages
        )

        while response.stop_reason == "tool_use":
            tool_use = None
            for block in response.content:
                if block.type == "tool_use":
                    tool_use = block

            if tool_use:
                tool_result = await handle_tool_call(tool_use.name, tool_use.input)
                print(f"  [Tool: {tool_use.name}] -> {tool_result}")

                messages.append({"role": "assistant", "content": response.content})
                messages.append({
                    "role": "user",
                    "content": [{
                        "type": "tool_result",
                        "tool_use_id": tool_use.id,
                        "content": tool_result
                    }]
                })

                response = claude_client.messages.create(
                    model="claude-sonnet-4-20250514",
                    max_tokens=50,
                    system="The display is showing the data. Just say something brief like 'Here you go!' or 'There it is!' - no need to repeat the info.",
                    tools=TOOLS,
                    messages=messages
                )

        for block in response.content:
            if block.type == "text":
                return block.text

        return "Done!"

    except Exception as e:
        return f"Error: {str(e)[:50]}"


# ============== WEBSOCKET HANDLERS ==============

async def handle_esp32_connection(websocket: websockets.WebSocketServerProtocol):
    """Handle WebSocket connection from ESP32."""
    connected_clients.add(websocket)
    client_ip = websocket.remote_address[0] if websocket.remote_address else "unknown"
    print(f"\n[ESP32 connected from {client_ip}] ({len(connected_clients)} total)")

    try:
        await websocket.send(json.dumps({
            "cmd": "text",
            "content": "Connected!",
            "size": "medium",
            "color": "#FFFFFF",
            "bg": "#1E1E28"
        }))

        async for message in websocket:
            print(f"[ESP32 -> Server]: {message[:100]}")

    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        connected_clients.discard(websocket)
        print(f"[ESP32 disconnected] ({len(connected_clients)} remaining)")


async def terminal_input_loop():
    """Read terminal input and process chat."""
    print("\n" + "="*50)
    print("Luna Chat Server (with REAL Data)")
    print("="*50)
    print("Try: 'What's the weather?', 'What time is it?'")
    print("     'When's the next 1 train?'")
    print("Commands: /quit, /status, /face")
    print("="*50 + "\n")

    loop = asyncio.get_event_loop()

    while True:
        try:
            user_input = await loop.run_in_executor(None, lambda: input("You: "))
            user_input = user_input.strip()

            if not user_input:
                continue

            if user_input.lower() == "/quit":
                print("Shutting down...")
                return
            elif user_input.lower() == "/status":
                print(f"Connected ESP32s: {len(connected_clients)}")
                continue
            elif user_input.lower() == "/face":
                await send_esp32_command({"cmd": "clear_display"})
                continue

            print("Luna: ", end="", flush=True)
            response = await get_claude_response(user_input)
            print(response)

            await broadcast_text(response)

        except EOFError:
            break
        except KeyboardInterrupt:
            print("\nShutting down...")
            break


async def main(port: int):
    """Main entry point."""
    global claude_client

    claude_client = anthropic.Anthropic()

    print(f"Starting WebSocket server on port {port}...")
    async with serve(handle_esp32_connection, "0.0.0.0", port):
        print(f"WebSocket server running at ws://0.0.0.0:{port}")
        await terminal_input_loop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Luna Chat Server")
    parser.add_argument("--port", type=int, default=7860, help="WebSocket port")
    args = parser.parse_args()

    try:
        asyncio.run(main(args.port))
    except KeyboardInterrupt:
        print("\nServer stopped.")
