#!/usr/bin/env python3
"""
Minimal Chat Server for ESP32-Luna

Simple WebSocket server that:
1. Accepts terminal input
2. Sends to Claude API
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


async def broadcast_to_esp32(text: str):
    """Send text to all connected ESP32 devices"""
    if not connected_clients:
        print("[No ESP32 connected]")
        return

    # Use the existing "text" command format from luna_protocol.c
    message = json.dumps({
        "cmd": "text",
        "content": text[:250],  # Limit to ESP32 buffer size
        "size": "medium",
        "color": "#FFFFFF",
        "bg": "#1E1E28"
    })

    # Send to all connected clients
    disconnected = set()
    for client in connected_clients:
        try:
            await client.send(message)
        except websockets.exceptions.ConnectionClosed:
            disconnected.add(client)

    # Remove disconnected clients
    connected_clients.difference_update(disconnected)

    print(f"[Sent to {len(connected_clients)} ESP32(s)]")


async def get_claude_response(user_input: str) -> str:
    """Get response from Claude API"""
    try:
        message = claude_client.messages.create(
            model="claude-sonnet-4-20250514",
            max_tokens=150,  # Keep responses short for display
            system="You are Luna, a friendly robot assistant. Keep responses very short (1-2 sentences max) since they display on a small screen.",
            messages=[
                {"role": "user", "content": user_input}
            ]
        )
        return message.content[0].text
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
            "content": "Connected to chat!",
            "size": "medium",
            "color": "#FFFFFF",
            "bg": "#1E1E28"
        }))

        # Keep connection alive, handle any incoming messages
        async for message in websocket:
            # ESP32 might send status updates, just log them
            print(f"[ESP32 -> Server]: {message[:100]}")

    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        connected_clients.discard(websocket)
        print(f"[ESP32 disconnected] ({len(connected_clients)} remaining)")


async def terminal_input_loop():
    """Read terminal input and process chat"""
    print("\n" + "="*50)
    print("Luna Chat Server")
    print("="*50)
    print("Type your message and press Enter to chat with Luna.")
    print("Commands: /quit, /status, /clear")
    print("="*50 + "\n")

    loop = asyncio.get_event_loop()

    while True:
        try:
            # Read input in a non-blocking way
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
            elif user_input.lower() == "/clear":
                await broadcast_to_esp32(" ")
                continue

            # Get Claude response
            print("Luna: ", end="", flush=True)
            response = await get_claude_response(user_input)
            print(response)

            # Send to ESP32
            await broadcast_to_esp32(response)

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
