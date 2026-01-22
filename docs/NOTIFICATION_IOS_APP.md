# Luna iOS Companion App Specification

## Overview

The Luna iOS app captures notifications from the user's iPhone and forwards them to the Luna server, which then displays them on the Luna device (ESP32 or simulator).

## Requirements

- iOS 15.0+
- Apple Developer Account (for Notification Service Extension)
- Xcode 15+

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Luna iOS App                            │
│                                                             │
│  ┌─────────────────┐    ┌─────────────────────────────────┐ │
│  │   Main App      │    │  Notification Service Extension │ │
│  │                 │    │                                 │ │
│  │  - Settings UI  │    │  - Intercepts notifications     │ │
│  │  - Server config│    │  - Filters by app               │ │
│  │  - Connection   │    │  - POSTs to server              │ │
│  │    status       │    │                                 │ │
│  └────────┬────────┘    └────────────────┬────────────────┘ │
│           │                              │                  │
│           │         App Group            │                  │
│           └──────────────────────────────┘                  │
│                          │                                  │
└──────────────────────────┼──────────────────────────────────┘
                           │
                           │ HTTPS POST
                           ▼
                    Luna Server
                    (POST /api/notify)
```

## Project Structure

```
LunaCompanion/
├── LunaCompanion/
│   ├── App/
│   │   ├── LunaCompanionApp.swift       # App entry point
│   │   └── ContentView.swift            # Main UI
│   ├── Views/
│   │   ├── SettingsView.swift           # Server & filter settings
│   │   ├── ConnectionStatusView.swift   # Connection indicator
│   │   └── AppFilterView.swift          # App whitelist/blacklist
│   ├── Services/
│   │   ├── NotificationService.swift    # Request notification permissions
│   │   ├── ServerAPI.swift              # HTTP client for Luna server
│   │   └── SettingsManager.swift        # UserDefaults wrapper
│   └── Models/
│       ├── LunaNotification.swift       # Notification data model
│       └── ServerConfig.swift           # Server configuration
├── NotificationExtension/
│   ├── NotificationService.swift        # Notification Service Extension
│   └── Info.plist
├── Shared/
│   └── SharedSettings.swift             # App Group shared data
└── LunaCompanion.entitlements
```

## Capabilities Required

Add these in Xcode under "Signing & Capabilities":

1. **App Groups** - For sharing data between main app and extension
   - Group ID: `group.com.yourcompany.luna`

2. **Push Notifications** - To receive notifications

3. **Background Modes** - For background URL sessions
   - Background fetch
   - Remote notifications

## Entitlements

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>aps-environment</key>
    <string>development</string>
    <key>com.apple.security.application-groups</key>
    <array>
        <string>group.com.yourcompany.luna</string>
    </array>
</dict>
</plist>
```

## Data Models

### LunaNotification.swift

```swift
import Foundation

struct LunaNotification: Codable {
    let id: String
    let appId: String
    let appName: String
    let title: String
    let body: String?
    let subtitle: String?
    let timestamp: Date
    let priority: NotificationPriority
    let category: String?
    let threadId: String?
    let attachments: AttachmentInfo?

    enum NotificationPriority: String, Codable {
        case low
        case normal
        case high
        case critical
    }

    struct AttachmentInfo: Codable {
        let hasImage: Bool
        let hasAction: Bool
    }

    // Convert to server JSON format
    func toServerPayload() -> [String: Any] {
        var payload: [String: Any] = [
            "id": id,
            "app_id": appId,
            "app_name": appName,
            "title": title,
            "timestamp": ISO8601DateFormatter().string(from: timestamp),
            "priority": priority.rawValue
        ]

        if let body = body { payload["body"] = body }
        if let subtitle = subtitle { payload["subtitle"] = subtitle }
        if let category = category { payload["category"] = category }
        if let threadId = threadId { payload["thread_id"] = threadId }

        if let attachments = attachments {
            payload["attachments"] = [
                "has_image": attachments.hasImage,
                "has_action": attachments.hasAction
            ]
        }

        return payload
    }
}
```

### ServerConfig.swift

```swift
import Foundation

struct ServerConfig: Codable {
    var serverURL: String
    var apiKey: String
    var isEnabled: Bool

    static let `default` = ServerConfig(
        serverURL: "http://192.168.1.100:8080",
        apiKey: "",
        isEnabled: false
    )

    var notifyEndpoint: URL? {
        URL(string: serverURL)?.appendingPathComponent("api/notify")
    }
}
```

## Notification Service Extension

This is the core component that intercepts notifications.

### NotificationExtension/NotificationService.swift

```swift
import UserNotifications

class NotificationService: UNNotificationServiceExtension {

    var contentHandler: ((UNNotificationContent) -> Void)?
    var bestAttemptContent: UNMutableNotificationContent?

    // Shared settings via App Group
    private var sharedDefaults: UserDefaults? {
        UserDefaults(suiteName: "group.com.yourcompany.luna")
    }

    override func didReceive(
        _ request: UNNotificationRequest,
        withContentHandler contentHandler: @escaping (UNNotificationContent) -> Void
    ) {
        self.contentHandler = contentHandler
        bestAttemptContent = (request.content.mutableCopy() as? UNMutableNotificationContent)

        guard let content = bestAttemptContent else {
            contentHandler(request.content)
            return
        }

        // Check if forwarding is enabled
        guard let settings = loadSettings(), settings.isEnabled else {
            contentHandler(content)
            return
        }

        // Check if this app is filtered
        let appId = request.content.userInfo["app_id"] as? String ?? Bundle.main.bundleIdentifier ?? "unknown"
        if isAppFiltered(appId) {
            contentHandler(content)
            return
        }

        // Build notification payload
        let notification = LunaNotification(
            id: UUID().uuidString,
            appId: appId,
            appName: getAppName(for: appId),
            title: content.title,
            body: content.body.isEmpty ? nil : content.body,
            subtitle: content.subtitle.isEmpty ? nil : content.subtitle,
            timestamp: Date(),
            priority: determinePriority(content),
            category: content.categoryIdentifier.isEmpty ? nil : content.categoryIdentifier,
            threadId: content.threadIdentifier.isEmpty ? nil : content.threadIdentifier,
            attachments: AttachmentInfo(
                hasImage: !content.attachments.isEmpty,
                hasAction: false
            )
        )

        // Forward to Luna server
        forwardToServer(notification: notification, settings: settings) { success in
            // Always deliver the original notification
            contentHandler(content)
        }
    }

    override func serviceExtensionTimeWillExpire() {
        // Called just before the extension will be terminated
        if let contentHandler = contentHandler, let content = bestAttemptContent {
            contentHandler(content)
        }
    }

    // MARK: - Private Methods

    private func loadSettings() -> ServerConfig? {
        guard let data = sharedDefaults?.data(forKey: "serverConfig") else {
            return nil
        }
        return try? JSONDecoder().decode(ServerConfig.self, from: data)
    }

    private func isAppFiltered(_ appId: String) -> Bool {
        let blockedApps = sharedDefaults?.stringArray(forKey: "blockedApps") ?? []
        return blockedApps.contains(appId)
    }

    private func getAppName(for bundleId: String) -> String {
        // Map common bundle IDs to friendly names
        let appNames: [String: String] = [
            "com.apple.mobilemail": "Mail",
            "com.apple.MobileSMS": "Messages",
            "com.apple.mobilecal": "Calendar",
            "com.apple.reminders": "Reminders",
            "com.slack.Slack": "Slack",
            "com.microsoft.teams": "Teams",
            "com.facebook.Messenger": "Messenger",
            "com.whatsapp.WhatsApp": "WhatsApp",
            "com.burbn.instagram": "Instagram",
            "com.twitter.ios": "Twitter",
        ]
        return appNames[bundleId] ?? bundleId.components(separatedBy: ".").last ?? "Unknown"
    }

    private func determinePriority(_ content: UNNotificationContent) -> LunaNotification.NotificationPriority {
        // Check for interruption level (iOS 15+)
        if #available(iOS 15.0, *) {
            switch content.interruptionLevel {
            case .passive: return .low
            case .active: return .normal
            case .timeSensitive: return .high
            case .critical: return .critical
            @unknown default: return .normal
            }
        }
        return .normal
    }

    private func forwardToServer(
        notification: LunaNotification,
        settings: ServerConfig,
        completion: @escaping (Bool) -> Void
    ) {
        guard let url = settings.notifyEndpoint else {
            completion(false)
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("Bearer \(settings.apiKey)", forHTTPHeaderField: "Authorization")
        request.timeoutInterval = 5  // Short timeout for extension

        do {
            request.httpBody = try JSONSerialization.data(
                withJSONObject: notification.toServerPayload(),
                options: []
            )
        } catch {
            completion(false)
            return
        }

        // Use shared URLSession for background capability
        let session = URLSession.shared
        let task = session.dataTask(with: request) { data, response, error in
            if let error = error {
                print("Luna: Failed to forward notification: \(error)")
                completion(false)
                return
            }

            if let httpResponse = response as? HTTPURLResponse {
                completion(httpResponse.statusCode == 200)
            } else {
                completion(false)
            }
        }
        task.resume()
    }
}
```

## Main App UI

### ContentView.swift (Main Screen with Remote Control)

```swift
import SwiftUI

struct ContentView: View {
    @State private var selectedTab = 0

    var body: some View {
        TabView(selection: $selectedTab) {
            RemoteControlView()
                .tabItem {
                    Label("Control", systemImage: "slider.horizontal.3")
                }
                .tag(0)

            SettingsView()
                .tabItem {
                    Label("Settings", systemImage: "gear")
                }
                .tag(1)
        }
    }
}
```

### RemoteControlView.swift

```swift
import SwiftUI

struct RemoteControlView: View {
    @AppStorage("serverConfig", store: UserDefaults(suiteName: "group.com.yourcompany.luna"))
    private var serverConfigData: Data = Data()

    @State private var currentMode: DisplayMode = .face
    @State private var timerMinutes: Int = 25
    @State private var isConnected = false

    enum DisplayMode: String, CaseIterable {
        case face = "Face"
        case clock = "Clock"
        case weather = "Weather"
        case timer = "Timer"
        case calendar = "Calendar"
        case subway = "Subway"

        var icon: String {
            switch self {
            case .face: return "face.smiling"
            case .clock: return "clock"
            case .weather: return "sun.max"
            case .timer: return "timer"
            case .calendar: return "calendar"
            case .subway: return "tram"
            }
        }

        var color: Color {
            switch self {
            case .face: return .white
            case .clock: return .orange
            case .weather: return .yellow
            case .timer: return .green
            case .calendar: return .blue
            case .subway: return .red
            }
        }
    }

    var body: some View {
        NavigationView {
            ScrollView {
                VStack(spacing: 24) {
                    // Connection Status
                    connectionStatus

                    // Display Mode Grid
                    displayModeGrid

                    // Timer Controls (shown when timer mode selected)
                    if currentMode == .timer {
                        timerControls
                    }

                    // Emotion Controls (shown when face mode selected)
                    if currentMode == .face {
                        emotionControls
                    }
                }
                .padding()
            }
            .navigationTitle("Luna Control")
        }
    }

    // MARK: - Connection Status

    private var connectionStatus: some View {
        HStack {
            Circle()
                .fill(isConnected ? Color.green : Color.red)
                .frame(width: 12, height: 12)
            Text(isConnected ? "Connected" : "Disconnected")
                .font(.subheadline)
                .foregroundColor(.secondary)
            Spacer()
        }
        .padding(.horizontal)
    }

    // MARK: - Display Mode Grid

    private var displayModeGrid: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Display Mode")
                .font(.headline)

            LazyVGrid(columns: [
                GridItem(.flexible()),
                GridItem(.flexible()),
                GridItem(.flexible())
            ], spacing: 16) {
                ForEach(DisplayMode.allCases, id: \.self) { mode in
                    ModeButton(
                        mode: mode,
                        isSelected: currentMode == mode
                    ) {
                        currentMode = mode
                        sendModeCommand(mode)
                    }
                }
            }
        }
    }

    // MARK: - Timer Controls

    private var timerControls: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Timer")
                .font(.headline)

            // Preset buttons
            HStack(spacing: 12) {
                ForEach([5, 15, 25, 45], id: \.self) { mins in
                    Button("\(mins)m") {
                        timerMinutes = mins
                        sendTimerReset(minutes: mins)
                    }
                    .buttonStyle(.bordered)
                    .tint(timerMinutes == mins ? .green : .gray)
                }
            }

            // Control buttons
            HStack(spacing: 16) {
                Button(action: { sendTimerCommand("timer_start") }) {
                    Label("Start", systemImage: "play.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .tint(.green)

                Button(action: { sendTimerCommand("timer_pause") }) {
                    Label("Pause", systemImage: "pause.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
            }
        }
        .padding()
        .background(Color(.secondarySystemBackground))
        .cornerRadius(12)
    }

    // MARK: - Emotion Controls

    private var emotionControls: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Emotion")
                .font(.headline)

            LazyVGrid(columns: [
                GridItem(.flexible()),
                GridItem(.flexible()),
                GridItem(.flexible()),
                GridItem(.flexible())
            ], spacing: 12) {
                EmotionButton(emoji: "😊", name: "happy") { sendEmotion("happy") }
                EmotionButton(emoji: "😢", name: "sad") { sendEmotion("sad") }
                EmotionButton(emoji: "😠", name: "angry") { sendEmotion("angry") }
                EmotionButton(emoji: "😲", name: "surprised") { sendEmotion("surprised") }
                EmotionButton(emoji: "🤔", name: "thinking") { sendEmotion("thinking") }
                EmotionButton(emoji: "😕", name: "confused") { sendEmotion("confused") }
                EmotionButton(emoji: "🤩", name: "excited") { sendEmotion("excited") }
                EmotionButton(emoji: "😺", name: "cat") { sendEmotion("cat") }
            }
        }
        .padding()
        .background(Color(.secondarySystemBackground))
        .cornerRadius(12)
    }

    // MARK: - API Calls

    private func sendModeCommand(_ mode: DisplayMode) {
        sendCommand(["cmd": "mode", "mode": mode.rawValue.lowercased()])
    }

    private func sendTimerCommand(_ cmd: String) {
        sendCommand(["cmd": cmd])
    }

    private func sendTimerReset(minutes: Int) {
        sendCommand(["cmd": "timer_reset", "minutes": minutes])
    }

    private func sendEmotion(_ emotion: String) {
        sendCommand(["cmd": "emotion", "emotion": emotion])
    }

    private func sendCommand(_ payload: [String: Any]) {
        guard let config = try? JSONDecoder().decode(ServerConfig.self, from: serverConfigData),
              let url = URL(string: config.serverURL)?.appendingPathComponent("api/control") else {
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("Bearer \(config.apiKey)", forHTTPHeaderField: "Authorization")

        do {
            request.httpBody = try JSONSerialization.data(withJSONObject: payload)
        } catch {
            return
        }

        URLSession.shared.dataTask(with: request) { data, response, error in
            DispatchQueue.main.async {
                if let httpResponse = response as? HTTPURLResponse {
                    isConnected = httpResponse.statusCode == 200
                }
            }
        }.resume()
    }
}

// MARK: - Supporting Views

struct ModeButton: View {
    let mode: RemoteControlView.DisplayMode
    let isSelected: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(spacing: 8) {
                Image(systemName: mode.icon)
                    .font(.system(size: 28))
                    .foregroundColor(isSelected ? mode.color : .gray)
                Text(mode.rawValue)
                    .font(.caption)
                    .foregroundColor(isSelected ? .primary : .secondary)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 16)
            .background(isSelected ? Color(.tertiarySystemBackground) : Color.clear)
            .cornerRadius(12)
            .overlay(
                RoundedRectangle(cornerRadius: 12)
                    .stroke(isSelected ? mode.color : Color.clear, lineWidth: 2)
            )
        }
        .buttonStyle(.plain)
    }
}

struct EmotionButton: View {
    let emoji: String
    let name: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(spacing: 4) {
                Text(emoji)
                    .font(.system(size: 32))
                Text(name)
                    .font(.caption2)
                    .foregroundColor(.secondary)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 8)
        }
        .buttonStyle(.plain)
    }
}
```

### SettingsView.swift

```swift
import SwiftUI

struct SettingsView: View {
    @AppStorage("serverConfig", store: UserDefaults(suiteName: "group.com.yourcompany.luna"))
    private var serverConfigData: Data = Data()

    @State private var serverURL: String = ""
    @State private var apiKey: String = ""
    @State private var isEnabled: Bool = false
    @State private var connectionStatus: ConnectionStatus = .unknown

    enum ConnectionStatus {
        case unknown, checking, connected, failed
    }

    var body: some View {
        NavigationView {
            Form {
                Section("Server Configuration") {
                    TextField("Server URL", text: $serverURL)
                        .textContentType(.URL)
                        .autocapitalization(.none)
                        .keyboardType(.URL)

                    SecureField("API Key", text: $apiKey)
                        .textContentType(.password)

                    Toggle("Enable Forwarding", isOn: $isEnabled)
                }

                Section("Connection Status") {
                    HStack {
                        Text("Status")
                        Spacer()
                        statusView
                    }

                    Button("Test Connection") {
                        testConnection()
                    }
                    .disabled(serverURL.isEmpty || apiKey.isEmpty)
                }

                Section("App Filters") {
                    NavigationLink("Blocked Apps") {
                        AppFilterView()
                    }
                }

                Section("Info") {
                    Text("Notifications from your iPhone will be forwarded to your Luna device when enabled.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }
            }
            .navigationTitle("Luna Settings")
            .onAppear(perform: loadSettings)
            .onChange(of: serverURL) { _ in saveSettings() }
            .onChange(of: apiKey) { _ in saveSettings() }
            .onChange(of: isEnabled) { _ in saveSettings() }
        }
    }

    @ViewBuilder
    private var statusView: some View {
        switch connectionStatus {
        case .unknown:
            Text("Not tested")
                .foregroundColor(.secondary)
        case .checking:
            ProgressView()
        case .connected:
            Label("Connected", systemImage: "checkmark.circle.fill")
                .foregroundColor(.green)
        case .failed:
            Label("Failed", systemImage: "xmark.circle.fill")
                .foregroundColor(.red)
        }
    }

    private func loadSettings() {
        if let config = try? JSONDecoder().decode(ServerConfig.self, from: serverConfigData) {
            serverURL = config.serverURL
            apiKey = config.apiKey
            isEnabled = config.isEnabled
        }
    }

    private func saveSettings() {
        let config = ServerConfig(
            serverURL: serverURL,
            apiKey: apiKey,
            isEnabled: isEnabled
        )
        if let data = try? JSONEncoder().encode(config) {
            serverConfigData = data
        }
    }

    private func testConnection() {
        connectionStatus = .checking

        guard let url = URL(string: serverURL)?.appendingPathComponent("api/notify/status") else {
            connectionStatus = .failed
            return
        }

        var request = URLRequest(url: url)
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")

        URLSession.shared.dataTask(with: request) { data, response, error in
            DispatchQueue.main.async {
                if let httpResponse = response as? HTTPURLResponse,
                   httpResponse.statusCode == 200 {
                    connectionStatus = .connected
                } else {
                    connectionStatus = .failed
                }
            }
        }.resume()
    }
}
```

### AppFilterView.swift

```swift
import SwiftUI

struct AppFilterView: View {
    @AppStorage("blockedApps", store: UserDefaults(suiteName: "group.com.yourcompany.luna"))
    private var blockedAppsData: Data = Data()

    @State private var blockedApps: [String] = []

    private let commonApps: [(id: String, name: String)] = [
        ("com.facebook.Facebook", "Facebook"),
        ("com.burbn.instagram", "Instagram"),
        ("com.twitter.ios", "Twitter"),
        ("com.zhiliaoapp.musically", "TikTok"),
        ("com.linkedin.LinkedIn", "LinkedIn"),
        ("com.reddit.Reddit", "Reddit"),
        ("com.spotify.client", "Spotify"),
        ("com.netflix.Netflix", "Netflix"),
        ("com.amazon.Amazon", "Amazon"),
        ("com.ubercab.UberClient", "Uber"),
    ]

    var body: some View {
        List {
            Section("Block notifications from these apps") {
                ForEach(commonApps, id: \.id) { app in
                    Toggle(app.name, isOn: Binding(
                        get: { blockedApps.contains(app.id) },
                        set: { isBlocked in
                            if isBlocked {
                                blockedApps.append(app.id)
                            } else {
                                blockedApps.removeAll { $0 == app.id }
                            }
                            saveBlockedApps()
                        }
                    ))
                }
            }

            Section {
                Text("Blocked apps will not have their notifications forwarded to Luna.")
                    .font(.footnote)
                    .foregroundColor(.secondary)
            }
        }
        .navigationTitle("Blocked Apps")
        .onAppear(perform: loadBlockedApps)
    }

    private func loadBlockedApps() {
        if let apps = try? JSONDecoder().decode([String].self, from: blockedAppsData) {
            blockedApps = apps
        }
    }

    private func saveBlockedApps() {
        if let data = try? JSONEncoder().encode(blockedApps) {
            blockedAppsData = data
        }
    }
}
```

## Server API Payload

The app sends notifications to the server in this format:

### POST `/api/notify`

**Headers:**
```
Content-Type: application/json
Authorization: Bearer <API_KEY>
```

**Body:**
```json
{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "app_id": "com.apple.mobilemail",
  "app_name": "Mail",
  "title": "John Doe",
  "body": "Hey, are you free for lunch tomorrow?",
  "subtitle": "Re: Meeting",
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

**Expected Response:**
```json
{
  "status": "accepted",
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "queued": true
}
```

## Notification Permissions

Request notification permissions on first launch:

```swift
import UserNotifications

class NotificationManager {
    static let shared = NotificationManager()

    func requestPermission() async -> Bool {
        let center = UNUserNotificationCenter.current()

        do {
            let granted = try await center.requestAuthorization(
                options: [.alert, .sound, .badge]
            )
            return granted
        } catch {
            print("Failed to request notification permission: \(error)")
            return false
        }
    }

    func checkPermissionStatus() async -> UNAuthorizationStatus {
        let settings = await UNUserNotificationCenter.current().notificationSettings()
        return settings.authorizationStatus
    }
}
```

## Testing

### Test Notification Script

Create a simple Python script to send test notifications to the iOS app for verification:

```python
# test_notification.py
import requests
import json
from datetime import datetime

SERVER_URL = "http://localhost:8080/api/notify"
API_KEY = "your-api-key"

notification = {
    "id": "test-" + datetime.now().strftime("%Y%m%d%H%M%S"),
    "app_id": "com.apple.mobilemail",
    "app_name": "Mail",
    "title": "Test Notification",
    "body": "This is a test notification from the iOS app",
    "timestamp": datetime.utcnow().isoformat() + "Z",
    "priority": "normal"
}

response = requests.post(
    SERVER_URL,
    headers={
        "Content-Type": "application/json",
        "Authorization": f"Bearer {API_KEY}"
    },
    json=notification
)

print(f"Status: {response.status_code}")
print(f"Response: {response.json()}")
```

## Privacy Considerations

1. **Data Storage**: Store API key securely in Keychain, not UserDefaults
2. **Network Security**: Use HTTPS in production
3. **Data Minimization**: Only forward necessary notification data
4. **User Control**: Allow users to disable forwarding or filter apps
5. **Privacy Policy**: Update privacy policy to mention notification forwarding

## App Store Submission Notes

When submitting to App Store:

1. **Notification Service Extension**: Explain the legitimate use case
2. **Background Processing**: Justify the need for background URL sessions
3. **Privacy Nutrition Labels**: Declare data collection accurately
4. **App Review Notes**: Explain that the app forwards notifications to a companion device

## Future Enhancements

1. **Rich Notifications**: Forward images/attachments
2. **Actions**: Support notification actions (reply, dismiss from phone)
3. **Read Receipts**: Sync read status between phone and Luna
4. **Quick Reply**: Reply to messages directly from Luna device
5. **Focus Mode Integration**: Respect iOS Focus modes
