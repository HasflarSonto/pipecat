#!/usr/bin/env python3
"""
Minimal Chat Server for ESP32-Luna

Simple WebSocket server that:
1. Accepts terminal input
2. Sends to Claude API with tools
3. Broadcasts responses to connected ESP32

Usage:
    python chat_server.py [--port 7860]

Requires:
    pip install websockets anthropic
"""

import asyncio
import json
import argparse
import sys
import os
from typing import Set
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


# Connected ESP32 clients
connected_clients: Set[websockets.WebSocketServerProtocol] = set()

# Anthropic client (initialized in main)
claude_client = None

# Tools for Claude to control ESP32 display
TOOLS = [
    {
        "name": "show_weather",
        "description": "Show weather information on the ESP32 display. Use this when the user asks about weather.",
        "input_schema": {
            "type": "object",
            "properties": {
                "temp": {
                    "type": "string",
                    "description": "Temperature string, e.g., '72°F' or '22°C'"
                },
                "icon": {
                    "type": "string",
                    "enum": ["sunny", "cloudy", "rainy", "snowy", "stormy", "foggy", "partly_cloudy"],
                    "description": "Weather icon to display"
                },
                "description": {
                    "type": "string",
                    "description": "Short weather description, e.g., 'Clear skies'"
                }
            },
            "required": ["temp", "icon", "description"]
        }
    },
    {
        "name": "show_clock",
        "description": "Show a clock/time display. Use this when the user asks what time it is.",
        "input_schema": {
            "type": "object",
            "properties": {
                "hours": {
                    "type": "integer",
                    "description": "Hour (0-23)"
                },
                "minutes": {
                    "type": "integer",
                    "description": "Minutes (0-59)"
                },
                "is_24h": {
                    "type": "boolean",
                    "description": "Use 24-hour format (default: false)"
                }
            },
            "required": ["hours", "minutes"]
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
                "seconds": {
                    "type": "integer",
                    "description": "Seconds for the timer (default: 0)"
                },
                "label": {
                    "type": "string",
                    "description": "Timer label, e.g., 'Focus', 'Break'"
                },
                "running": {
                    "type": "boolean",
                    "description": "Start the timer immediately (default: false)"
                }
            },
            "required": ["minutes"]
        }
    },
    {
        "name": "show_subway",
        "description": "Show subway/train arrival times. Use for transit information.",
        "input_schema": {
            "type": "object",
            "properties": {
                "line": {
                    "type": "string",
                    "description": "Train line, e.g., '1', 'A', 'N'"
                },
                "color": {
                    "type": "string",
                    "description": "Line color in hex, e.g., '#EE352E' for red"
                },
                "station": {
                    "type": "string",
                    "description": "Station name, e.g., '110 St'"
                },
                "direction": {
                    "type": "string",
                    "description": "Direction: 'Downtown' or 'Uptown'"
                },
                "times": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "Arrival times in minutes, e.g., [3, 8, 12]"
                }
            },
            "required": ["line", "station", "times"]
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


async def send_esp32_command(cmd: dict):
    """Send a command to all connected ESP32 devices"""
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
    """Handle a tool call from Claude"""

    if tool_name == "show_weather":
        await send_esp32_command({
            "cmd": "weather",
            "temp": tool_input.get("temp", "72°F"),
            "icon": tool_input.get("icon", "sunny"),
            "desc": tool_input.get("description", "")
        })
        return f"Showing weather: {tool_input.get('temp')}"

    elif tool_name == "show_clock":
        hours = tool_input.get("hours", datetime.now().hour)
        minutes = tool_input.get("minutes", datetime.now().minute)
        await send_esp32_command({
            "cmd": "clock",
            "hours": hours,
            "minutes": minutes,
            "is_24h": tool_input.get("is_24h", False)
        })
        return f"Showing clock: {hours:02d}:{minutes:02d}"

    elif tool_name == "show_timer":
        await send_esp32_command({
            "cmd": "timer",
            "minutes": tool_input.get("minutes", 5),
            "seconds": tool_input.get("seconds", 0),
            "label": tool_input.get("label", "Timer"),
            "running": tool_input.get("running", False)
        })
        return f"Showing timer: {tool_input.get('minutes')} minutes"

    elif tool_name == "show_subway":
        await send_esp32_command({
            "cmd": "subway",
            "line": tool_input.get("line", "1"),
            "color": tool_input.get("color", "#EE352E"),
            "station": tool_input.get("station", "Station"),
            "direction": tool_input.get("direction", "Downtown"),
            "times": tool_input.get("times", [5, 10, 15])
        })
        return f"Showing subway: {tool_input.get('line')} train"

    elif tool_name == "show_face":
        await send_esp32_command({"cmd": "clear_display"})
        return "Returned to face display"

    elif tool_name == "set_emotion":
        await send_esp32_command({
            "cmd": "emotion",
            "value": tool_input.get("emotion", "neutral")
        })
        return f"Set emotion: {tool_input.get('emotion')}"

    return "Unknown tool"


async def broadcast_text(text: str):
    """Send text caption to ESP32"""
    await send_esp32_command({
        "cmd": "text",
        "content": text[:250],
        "size": "medium",
        "color": "#FFFFFF",
        "bg": "#1E1E28"
    })


async def get_claude_response(user_input: str) -> str:
    """Get response from Claude API with tools"""
    try:
        messages = [{"role": "user", "content": user_input}]

        # First API call - may return tool use
        response = claude_client.messages.create(
            model="claude-sonnet-4-20250514",
            max_tokens=300,
            system="""You are Luna, a friendly robot assistant displayed on a small ESP32 screen.
You have tools to control what's shown on the display.

Guidelines:
- Keep text responses very short (1-2 sentences) since they display on a small screen
- Use tools when appropriate (weather, time, timer, etc.)
- After using a tool, give a brief confirmation
- Be helpful and friendly!""",
            tools=TOOLS,
            messages=messages
        )

        # Handle tool use if needed
        while response.stop_reason == "tool_use":
            # Find tool use block
            tool_use = None
            text_response = ""
            for block in response.content:
                if block.type == "tool_use":
                    tool_use = block
                elif block.type == "text":
                    text_response = block.text

            if tool_use:
                # Execute the tool
                tool_result = await handle_tool_call(tool_use.name, tool_use.input)
                print(f"  [Tool: {tool_use.name}] -> {tool_result}")

                # Continue conversation with tool result
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
                    system="You are Luna. Give a brief, friendly response about what you just did.",
                    tools=TOOLS,
                    messages=messages
                )

        # Extract final text response
        for block in response.content:
            if block.type == "text":
                return block.text

        return "Done!"

    except Exception as e:
        return f"Error: {str(e)[:50]}"


async def handle_esp32_connection(websocket: websockets.WebSocketServerProtocol):
    """Handle WebSocket connection from ESP32"""
    connected_clients.add(websocket)
    client_ip = websocket.remote_address[0] if websocket.remote_address else "unknown"
    print(f"\n[ESP32 connected from {client_ip}] ({len(connected_clients)} total)")

    try:
        # Send welcome message
        await websocket.send(json.dumps({
            "cmd": "text",
            "content": "Connected!",
            "size": "medium",
            "color": "#FFFFFF",
            "bg": "#1E1E28"
        }))

        # Keep connection alive, handle any incoming messages
        async for message in websocket:
            print(f"[ESP32 -> Server]: {message[:100]}")

    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        connected_clients.discard(websocket)
        print(f"[ESP32 disconnected] ({len(connected_clients)} remaining)")


async def terminal_input_loop():
    """Read terminal input and process chat"""
    print("\n" + "="*50)
    print("Luna Chat Server (with Tools)")
    print("="*50)
    print("Type your message and press Enter to chat with Luna.")
    print("Luna can now control the display (weather, clock, etc.)")
    print("Commands: /quit, /status, /face")
    print("="*50 + "\n")

    loop = asyncio.get_event_loop()

    while True:
        try:
            user_input = await loop.run_in_executor(None, lambda: input("You: "))
            user_input = user_input.strip()

            if not user_input:
                continue

            # Handle commands
            if user_input.lower() == "/quit":
                print("Shutting down...")
                return
            elif user_input.lower() == "/status":
                print(f"Connected ESP32s: {len(connected_clients)}")
                continue
            elif user_input.lower() == "/face":
                await send_esp32_command({"cmd": "clear_display"})
                continue

            # Get Claude response (may use tools)
            print("Luna: ", end="", flush=True)
            response = await get_claude_response(user_input)
            print(response)

            # Send text response to ESP32
            await broadcast_text(response)

        except EOFError:
            break
        except KeyboardInterrupt:
            print("\nShutting down...")
            break


async def main(port: int):
    """Main entry point"""
    global claude_client

    # Initialize Anthropic client
    claude_client = anthropic.Anthropic()

    # Start WebSocket server
    print(f"Starting WebSocket server on port {port}...")
    async with serve(handle_esp32_connection, "0.0.0.0", port):
        print(f"WebSocket server running at ws://0.0.0.0:{port}")

        # Run terminal input loop
        await terminal_input_loop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Luna Chat Server")
    parser.add_argument("--port", type=int, default=7860, help="WebSocket port")
    args = parser.parse_args()

    try:
        asyncio.run(main(args.port))
    except KeyboardInterrupt:
        print("\nServer stopped.")
