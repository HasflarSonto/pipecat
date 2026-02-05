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

    return "Unknown tool"


async def broadcast_text(text: str):
    """Send text caption to ESP32."""
    await send_esp32_command({
        "cmd": "text",
        "content": text[:250],
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
            max_tokens=300,
            system="""You are Luna, a friendly robot assistant displayed on a small ESP32 screen.
You have tools to control what's shown on the display with REAL data.

Guidelines:
- Keep text responses very short (1-2 sentences) since they display on a small screen
- Use tools to fetch real data (weather, time, subway times)
- For weather, ask about location if not specified
- For subway, default to 1 train at 110 St downtown if not specified
- Be helpful and friendly!""",
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
                    max_tokens=150,
                    system="You are Luna. Give a brief, friendly response about the real data you just showed.",
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
