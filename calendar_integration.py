"""
Google Calendar Integration for Luna Voice Bot

Setup Instructions:
1. Go to https://console.cloud.google.com/
2. Create a new project (or select existing)
3. Enable the Google Calendar API:
   - Go to "APIs & Services" > "Library"
   - Search for "Google Calendar API"
   - Click "Enable"
4. Create OAuth 2.0 credentials:
   - Go to "APIs & Services" > "Credentials"
   - Click "Create Credentials" > "OAuth client ID"
   - Choose "Desktop app" as application type
   - Download the JSON file
   - Save it as "credentials.json" in the pipecat directory
5. First run will open a browser to authenticate
"""

import os
import pickle
from datetime import datetime, timedelta
from typing import List, Optional

from loguru import logger

# Check if Google API libraries are available
try:
    from google.auth.transport.requests import Request
    from google.oauth2.credentials import Credentials
    from google_auth_oauthlib.flow import InstalledAppFlow
    from googleapiclient.discovery import build
    from googleapiclient.errors import HttpError
    GOOGLE_API_AVAILABLE = True
except ImportError:
    GOOGLE_API_AVAILABLE = False
    logger.warning("Google API libraries not installed. Run: pip install google-auth-oauthlib google-api-python-client")

# Scopes for Google Calendar (read-only access)
SCOPES = ['https://www.googleapis.com/auth/calendar.readonly']

# Paths for credentials and token
CREDENTIALS_FILE = os.path.join(os.path.dirname(__file__), 'credentials.json')
TOKEN_FILE = os.path.join(os.path.dirname(__file__), 'token.pickle')


class CalendarEvent:
    """Represents a calendar event."""

    def __init__(self, summary: str, start: datetime, end: datetime,
                 location: Optional[str] = None, description: Optional[str] = None,
                 is_all_day: bool = False):
        self.summary = summary
        self.start = start
        self.end = end
        self.location = location
        self.description = description
        self.is_all_day = is_all_day

    def __str__(self) -> str:
        if self.is_all_day:
            return f"{self.summary} (all day)"
        return f"{self.summary} at {self.start.strftime('%I:%M %p')}"

    def time_until(self) -> str:
        """Get human-readable time until event."""
        now = datetime.now(self.start.tzinfo) if self.start.tzinfo else datetime.now()
        delta = self.start - now

        if delta.total_seconds() < 0:
            return "now"

        hours = int(delta.total_seconds() // 3600)
        minutes = int((delta.total_seconds() % 3600) // 60)

        if hours > 24:
            days = hours // 24
            return f"in {days} day{'s' if days > 1 else ''}"
        elif hours > 0:
            return f"in {hours} hour{'s' if hours > 1 else ''}"
        elif minutes > 0:
            return f"in {minutes} minute{'s' if minutes > 1 else ''}"
        else:
            return "starting now"


def get_credentials() -> Optional[Credentials]:
    """Get valid Google API credentials, refreshing or re-authenticating as needed."""
    if not GOOGLE_API_AVAILABLE:
        logger.error("Google API libraries not installed")
        return None

    creds = None

    # Load existing token if available
    if os.path.exists(TOKEN_FILE):
        with open(TOKEN_FILE, 'rb') as token:
            creds = pickle.load(token)

    # If no valid credentials, authenticate
    if not creds or not creds.valid:
        if creds and creds.expired and creds.refresh_token:
            try:
                creds.refresh(Request())
            except Exception as e:
                logger.warning(f"Token refresh failed: {e}")
                creds = None

        if not creds:
            if not os.path.exists(CREDENTIALS_FILE):
                logger.error(f"Credentials file not found: {CREDENTIALS_FILE}")
                logger.error("Please download OAuth credentials from Google Cloud Console")
                return None

            try:
                flow = InstalledAppFlow.from_client_secrets_file(CREDENTIALS_FILE, SCOPES)
                creds = flow.run_local_server(port=0)
            except Exception as e:
                logger.error(f"OAuth flow failed: {e}")
                return None

        # Save the credentials for next time
        with open(TOKEN_FILE, 'wb') as token:
            pickle.dump(creds, token)

    return creds


def get_upcoming_events(max_results: int = 5, time_min: Optional[datetime] = None,
                        time_max: Optional[datetime] = None) -> List[CalendarEvent]:
    """
    Get upcoming events from Google Calendar.

    Args:
        max_results: Maximum number of events to return (default 5)
        time_min: Start time for events (default: now)
        time_max: End time for events (default: 24 hours from now)

    Returns:
        List of CalendarEvent objects
    """
    if not GOOGLE_API_AVAILABLE:
        logger.error("Google API libraries not installed")
        return []

    creds = get_credentials()
    if not creds:
        return []

    try:
        service = build('calendar', 'v3', credentials=creds)

        # Set default time range
        if time_min is None:
            time_min = datetime.utcnow()
        if time_max is None:
            time_max = time_min + timedelta(hours=24)

        # Format times for API
        time_min_str = time_min.isoformat() + 'Z'
        time_max_str = time_max.isoformat() + 'Z'

        # Get events from primary calendar
        events_result = service.events().list(
            calendarId='primary',
            timeMin=time_min_str,
            timeMax=time_max_str,
            maxResults=max_results,
            singleEvents=True,
            orderBy='startTime'
        ).execute()

        events = events_result.get('items', [])

        result = []
        for event in events:
            # Parse start/end times
            start = event['start'].get('dateTime', event['start'].get('date'))
            end = event['end'].get('dateTime', event['end'].get('date'))

            # Check if all-day event (date only, no time)
            is_all_day = 'date' in event['start']

            # Parse datetime
            if is_all_day:
                start_dt = datetime.strptime(start, '%Y-%m-%d')
                end_dt = datetime.strptime(end, '%Y-%m-%d')
            else:
                # Handle timezone-aware datetimes
                start_dt = datetime.fromisoformat(start.replace('Z', '+00:00'))
                end_dt = datetime.fromisoformat(end.replace('Z', '+00:00'))

            result.append(CalendarEvent(
                summary=event.get('summary', 'Untitled Event'),
                start=start_dt,
                end=end_dt,
                location=event.get('location'),
                description=event.get('description'),
                is_all_day=is_all_day
            ))

        return result

    except HttpError as e:
        logger.error(f"Google Calendar API error: {e}")
        return []
    except Exception as e:
        logger.error(f"Error fetching calendar events: {e}")
        return []


def get_todays_events() -> List[CalendarEvent]:
    """Get all events for today."""
    now = datetime.utcnow()
    # Start of today (midnight UTC)
    today_start = now.replace(hour=0, minute=0, second=0, microsecond=0)
    # End of today (11:59 PM UTC)
    today_end = today_start + timedelta(days=1)

    return get_upcoming_events(max_results=10, time_min=today_start, time_max=today_end)


def get_next_event() -> Optional[CalendarEvent]:
    """Get the next upcoming event."""
    events = get_upcoming_events(max_results=1)
    return events[0] if events else None


def format_events_for_voice(events: List[CalendarEvent]) -> str:
    """Format events for voice output."""
    if not events:
        return "You have no upcoming events."

    if len(events) == 1:
        event = events[0]
        return f"Your next event is {event.summary} {event.time_until()}."

    # Multiple events
    parts = []
    for i, event in enumerate(events[:3]):  # Limit to 3 for voice
        if i == 0:
            parts.append(f"Your next event is {event.summary} {event.time_until()}")
        else:
            parts.append(f"then {event.summary} {event.time_until()}")

    return ", ".join(parts) + "."


def is_configured() -> bool:
    """Check if calendar integration is configured."""
    if not GOOGLE_API_AVAILABLE:
        return False
    return os.path.exists(CREDENTIALS_FILE) or os.path.exists(TOKEN_FILE)


# Quick test
if __name__ == "__main__":
    print("Testing Google Calendar Integration...")

    if not GOOGLE_API_AVAILABLE:
        print("ERROR: Google API libraries not installed")
        print("Run: pip install google-auth-oauthlib google-api-python-client")
        exit(1)

    if not os.path.exists(CREDENTIALS_FILE):
        print(f"ERROR: Credentials file not found: {CREDENTIALS_FILE}")
        print("Please download OAuth credentials from Google Cloud Console")
        exit(1)

    print("Fetching upcoming events...")
    events = get_upcoming_events()

    if events:
        print(f"\nFound {len(events)} upcoming events:")
        for event in events:
            print(f"  - {event}")
        print(f"\nVoice output: {format_events_for_voice(events)}")
    else:
        print("No upcoming events found (or error occurred)")
