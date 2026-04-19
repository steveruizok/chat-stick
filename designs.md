# Designs

Design spec for the M5StickS3 live voice assistant firmware. Focuses on concrete patterns (typography, colors, spacing, effects) and all screens/states rendered by the UI layer. The AI responds via audio, not text; the screen only displays text when the model calls the `show_text` tool.

## UX Guidance

- Minimal UI: black background, monochrome text, no ornamentation.
- Use shimmer to indicate background work (boot, AI generating, updating, resetting). No spinner.
- Use device LED for recording state (solid, then faster blink in last 10s).
- Prefer left-aligned text; only center "Starting...", "Thinking...", "Updating...", "Resetting...".
- Be explicit and short with copy. Provide a clear next action hint.
- Keep interactions consistent: A = act/record/select; B = paginate/back/open menu.

## Patterns

### Requirements Ahead of Firmware (to implement)

- Layout transition
  - Move from current 7-row layout (header + 5 body + footer) to 8-row layout matching the standard: 7-line text area + footer row. Current firmware uses a dedicated header row for endpoint label and battery %; those should move to the Home menu status bar.
- Paging unification
  - Button B advances pages in tool text, WiFi lists, and Resume Chat lists.
  - Page indicator sits in the footer row (row 8) at far right: ▾ (more), ● (last).
- Menu system
  - Home shows a status bar (time left, battery % right). Items: Go back, New conversation, Resume chat, Device.
  - Resume Chat presents a paginated list of recent conversations; footer row includes "Go back".
- WiFi management
  - Runtime WiFi configuration via captive portal (AP mode). Currently WiFi credentials are compiled into firmware via credentials.h.
  - Paginated network list with exactly 5 SSIDs per page → 1 blank line → actions (Go back, Try again, Add WiFi network).
  - ● marks the connected SSID; ▸ marks the highlighted SSID; A selects; B cycles; B hold backs.
- Reset device behavior
  - "Reset device" keeps WiFi credentials and restarts into the last working version (no factory image). Show "Resetting..." with shimmer.
  - Implementation note: maintain a last-known-good firmware slot for rollback.
- Transient chat failure copy
  - On failed send (no connectivity end), show "Sorry, that didn't work." and allow immediate retry; only show boot/WiFi screens when internet ends.
- Updates flow
  - Checking/Available/None match designs; "Updating..." uses shimmer; failure and complete screens offer actions as shown.
- Shimmer effect
  - Not yet implemented; currently all status text is static.
- LED recording indicator
  - Not yet implemented; recording state indicated by screen text only.
- Error recovery
  - Error states currently require power cycle. Should offer actions (Try again, Reset device) as designed.
- Tool text persistence
  - Tool text should persist across turn completion until the next turn starts or the user dismisses it, not clear on every state change.

### Rendering Rules (TUI-first)

- Compose UI as text, line by line (8 rows × 30 chars including left space). Prefer a single text block per screen over multiple shapes/boxes.
- Reserve columns consistently:
  - Col 1: left padding (space)
  - Col 2: selection marker for lists/menus (`▸`, fallback `>`)
  - Col 30 (last col): page marker (`▾` more, `●` last; fallbacks `v` and `o`)
- Footer row (row 8): place page marker at the far right; optional left label (e.g., `Go back`).
- Indicators are text glyphs wherever possible. Avoid drawing triangles/arrows/circles; render them as characters in the line content.
- Glyph fallback mapping for AsciiFont8x16 (if extended glyphs unsupported): `▾`→`v`, `●`→`o`, `▸`→`>`
- Keep shimmer and text wrapping as they are (per-character drawing ok).
- Progress bars should use ASCII gauge (e.g., `[########....] 32%`) rather than filled rectangles.

### Typography

- Font: 8×16 monospace bitmap (`AsciiFont8x16`)
- Text size: `1.0`
- Character metrics: 8 px wide × 16 px high
- Max chars per line: 29
- Text lines per page: 7

### Layout and Spacing

- Screen: 240×135 px (landscape)
- Padding: T:4 L:4 R:4 B:3
- Text area: 232×112 px — 7 lines × 29 chars
- Footer row: 232×16 px at y=116 (8th row); contains page indicator right-aligned and optionally a left-aligned action label (e.g. "Go back")
- Non-paginated menus: 232×128 px (8 full text rows, no separate footer row)

### Colors

- Background: black (#000)
- Text: white (#FFF) by default; gray for secondary/inactive or recording/generating.

Constants (RGB565): BLACK 0x0000, WHITE 0xFFFF, GRAY 0x7BEF, LIGHT_GRAY 0xBDF7, DARK_GRAY 0x4208, DARKER_GRAY 0x2104.

### Text Wrapping & Pagination

- Word wrapping at 29 chars; 7 lines per page.
- Footer row (row 8): ▾ (more pages) or ● (last page), right-aligned. Optionally a left-aligned action label.
- Tool text follows the same paging rules; Button B advances pages.
- Lists (e.g., WiFi networks, Resume Chat) follow the same paging rules and use Button B to advance pages.

### Effects

- Shimmer (text): diagonal wave sweeping top-right → bottom-left over 6 gray→white steps. Used for "Starting...", "Thinking...", "Updating...", "Resetting..." and tool text while Generating.
- Animation step: ~80 ms per frame.

### Indicators & Icons

- Page indicator: ▾ (more content) or ● (last page), right-aligned in footer row.
- Selection indicator: ▸ before the currently highlighted menu item.
- Current item indicator: ● before the active item in a list (connected network, active chat session).
- No on-screen recording dot/spinner. The device LED indicates recording.

### Text Sanitization

- Replace curly quotes, em/en dashes, and ellipses with `'`, `-`, `...` to ensure display compatibility.

## Screens

### ASCII Mockups

These are schematic but consistent in size with the actual screen layout. Each box uses:

- Fixed width frame: `┌──────────────────────────────┐` / `└──────────────────────────────┘`
- Exactly 8 interior lines of 30 characters between `│` borders (1 space left padding + 29 text chars)
- Row 8 is the footer row: page indicator at far right (▾ more pages, ● last page), optionally a left-aligned action label

### Boot — Starting — BOOT-01

```
┌──────────────────────────────┐
│                              │
│                              │
│                              │
│          Starting...         │  ← shimmer, centered
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: No effect
- PRESS_B: No effect
- HOLD_B: No effect

Events:

- BOOT_OK: Proceed to CHAT-01 (new chat)
- BOOT_FAIL: Go to BOOT-02

### Boot — Failure — BOOT-02

```
┌──────────────────────────────┐
│ Could not start device.      │
│                              │
│                              │
│                              │
│                              │
│                              │
│ ▸ Try again                  │
│   Reset device               │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option (Try again → BOOT-01; Reset device → RESET-01)
- PRESS_B: Next item
- HOLD_B: No effect

Events:

- POWER_CYCLE: Go to BOOT-01

### Chat — Idle (new chat) — CHAT-01

Default idle screen. Shown after boot, after "New conversation", and whenever no tool text is displayed. The AI responds via audio; no text appears on screen unless the model calls `show_text`.

```
┌──────────────────────────────┐
│ Hi, how can I help? Hold the │
│ big button and speak to get  │
│ a response.                  │
│                              │
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Start recording → CHAT-05
- PRESS_B: No effect (single page)
- HOLD_B: Open menu → HOME-01

Events:

- INTERNET_LOST: Show inline message "Could not connect to the internet." with options "Set up WiFi", "Try again" (remain in Chat)

### Chat — Idle (tool text, single page) — CHAT-02

Shown when the model calls `show_text` and the text fits in one page. Tool text persists until the next turn starts or the user dismisses it with Button B.

```
┌──────────────────────────────┐
│ The weather in San Francisco │
│ is 62F and partly cloudy.    │
│                              │
│                              │
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Start recording → CHAT-05
- PRESS_B: Dismiss tool text → CHAT-01
- HOLD_B: Open menu → HOME-01

Events:

- INTERNET_LOST: Show inline message "Could not connect to the internet." with options "Set up WiFi", "Try again" (remain in Chat)

### Chat — Idle (tool text, multi-page, first page) — CHAT-03

```
┌──────────────────────────────┐
│ Here are some fun facts about│
│ the M5StickS3: it has a      │
│ built-in 1.14 inch TFT       │
│ display, an ESP32-S3 chip    │
│ with 8MB of flash, a 6-axis  │
│ IMU, a microphone, and a     │
│ tiny 1W speaker. The whole   │
│                            ▾ │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Start recording → CHAT-05
- PRESS_B: Next page of tool text
- HOLD_B: Open menu → HOME-01

Events:

- INTERNET_LOST: Show inline message "Could not connect to the internet." with options "Set up WiFi", "Try again" (remain in Chat)

### Chat — Idle (tool text, last page) — CHAT-04

```
┌──────────────────────────────┐
│ device measures just         │
│ 48x24x15mm and weighs 15g.   │
│                              │
│                              │
│                              │
│                              │
│                              │
│                            ● │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Start recording → CHAT-05
- PRESS_B: Dismiss tool text → CHAT-01
- HOLD_B: Open menu → HOME-01

Events:

- INTERNET_LOST: Show inline message "Could not connect to the internet." with options "Set up WiFi", "Try again" (remain in Chat)

### Chat — Generating — CHAT-05

Waiting for the AI to respond. Shimmer effect applied to whatever text is currently displayed. If tool text is visible, shimmer applies to it; otherwise centered "Thinking..." with shimmer.

```
┌──────────────────────────────┐
│                              │
│                              │
│                              │
│          Thinking...         │  ← shimmer, centered
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

With previous tool text visible:

```
┌──────────────────────────────┐
│ The weather in San Francisco │  ← shimmer
│ is 62F and partly cloudy.    │  ← shimmer
│                              │
│                              │
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Interrupt generation and start recording → CHAT-05 (new turn)
- PRESS_B: If tool text has another page, next page; otherwise no effect
- HOLD_B: Open menu → HOME-01

Events:

- AUDIO_START: Begin audio playback → CHAT-07 (shimmer stops)
- TOOL_TEXT: Display tool text (with shimmer, since still generating)
- TURN_COMPLETE (no audio): Return to CHAT-01 or CHAT-02 (idle, shimmer stops)
- ERROR_TIMEOUT (15s): Display "Sorry, that didn't work." → CHAT-08
- SEND_FAILED: Display "Sorry, that didn't work." → CHAT-08
- INTERNET_LOST: Go to WIFI-04

### Chat — Recording / Grace — CHAT-06

Audio is being captured while Button A is held. Previous content shown in gray. Recording state indicated by device LED (solid → faster blink in last 10s). Grace period (~500ms) after release allows pressing A to resume.

```
┌──────────────────────────────┐
│ Hi, how can I help? Hold the │  ← gray
│ big button and speak to get  │  ← gray
│ a response.                  │  ← gray
│                              │
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

With previous tool text:

```
┌──────────────────────────────┐
│ The weather in San Francisco │  ← gray
│ is 62F and partly cloudy.    │  ← gray
│                              │
│                              │
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

Interactions:

- RELEASE_A: End recording, begin generation → CHAT-05
- PRESS_A (during grace): Resume recording
- PRESS_B: No effect while recording/grace
- HOLD_B: No effect while recording/grace

Events:

- GRACE_TIMEOUT: Begin generation → CHAT-05

### Chat — Speaking — CHAT-07

AI audio is playing through the speaker. Screen shows tool text if any was received during this turn; otherwise returns to idle content. Shimmer has stopped (visual cue that the response is arriving).

```
┌──────────────────────────────┐
│ The weather in San Francisco │
│ is 62F and partly cloudy.    │
│                              │
│                              │
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Interrupt playback, start recording → CHAT-06
- PRESS_B: If tool text has another page, next page; otherwise dismiss tool text
- HOLD_B: Open menu → HOME-01

Events:

- TURN_COMPLETE + PLAYBACK_IDLE: Return to idle → CHAT-01 or CHAT-02
- TOOL_TEXT: Update displayed tool text

### Chat — Transient failure — CHAT-08

```
┌──────────────────────────────┐
│ Sorry, that didn't work.     │
│                              │
│                              │
│                              │
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Start recording (retry) → CHAT-06
- PRESS_B: No effect
- HOLD_B: Open menu → HOME-01

Events:

- INTERNET_LOST: Go to WIFI-04

### Menu — Home — HOME-01

```
┌──────────────────────────────┐
│ 10:47AM                  65% │  ← status bar: time + battery
│ wifiname            2640ffab │  ← status bar 2: wifi name, thread id
│ dev                          │  ← shown only when connected to the dev server
│                              │
│ ▸ Go back                    │
│   New conversation           │
│   Resume chat                │
│   Device                     │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option
  - "Go back" → Return to previous screen (usually Chat)
  - "New conversation" → CHAT-01
  - "Resume chat" → RESUME-01/RESUME-02 (depending on pagination)
  - "Device" → DEVICE-01/DEVICE-02 (based on WiFi state)
- PRESS_B: Next menu item
- HOLD_B: Close menu → Return to previous screen (usually Chat)

Events:

- TIME_TICK: Update clock in status bar
- BATTERY_UPDATE: Update battery percentage in status bar

### Menu — Device (connected to WiFi) — DEVICE-01

```
┌──────────────────────────────┐
│ M5 Live v1.0.0               │
│ by Steve Ruiz                │
│                              │
│                              │
│ ▸ Go back                    │
│   Set up WiFi                │
│   Check for updates          │
│   Turn off                   │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option
  - "Go back" → Return to previous screen
  - "Set up WiFi" → WIFI-01
  - "Check for updates" → UPDATES-01
  - "Turn off" → Sleep/power-down state
- PRESS_B: Next menu item
- HOLD_B: Go back/close menu → Return to previous screen

Events:

- WIFI_DISCONNECTED: Switch to DEVICE-02

### Menu — Device (not connected to WiFi) — DEVICE-02

```
┌──────────────────────────────┐
│ M5 Live v1.0.0               │
│ by Steve Ruiz                │
│                              │
│                              │
│ ▸ Go back                    │
│   Set up WiFi                │
│   Turn off                   │
│   Reset device               │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option
  - "Go back" → Return to previous screen
  - "Set up WiFi" → WIFI-02
  - "Turn off" → Sleep/power-down state
  - "Reset device" → RESET-01
- PRESS_B: Next menu item
- HOLD_B: Go back/close menu → Return to previous screen

Events:

- WIFI_CONNECTED: Switch to DEVICE-01

### Menu — WiFi (connected) — WIFI-01

```
┌──────────────────────────────┐
│ Connected to WiFi.           │
│ myhome123                    │
│                              │
│                              │
│ ▸ Go back                    │
│   Select network             │
│   Connect to new network     │
│   Disconnect                 │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option
  - "Go back" → Return to previous screen (Device/Home)
  - "Select network" → WIFI-06/WIFI-07 (first page if paginated)
  - "Connect to new network" → WIFI-03
  - "Disconnect" → WIFI-02
- PRESS_B: Next menu item
- HOLD_B: Go back/close menu → Return to previous screen

Events:

- WIFI_DISCONNECTED: Remain on WIFI-02
- WIFI_CONNECTED: Remain on WIFI-01

### Menu — WiFi (not connected) — WIFI-02

```
┌──────────────────────────────┐
│ Not connected to WiFi.       │
│                              │
│                              │
│                              │
│ ▸ Go back                    │
│   Select network             │
│   Connect to new network     │
│                              │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option
  - "Go back" → Return to previous screen (Device/Home)
  - "Select network" → WIFI-06/WIFI-07 (first page if paginated)
  - "Connect to new network" → WIFI-03
- PRESS_B: Next menu item
- HOLD_B: Go back/close menu → Return to previous screen

Events:

- WIFI_CONNECTED: Switch to WIFI-01

### Menu — WiFi — Set up new network — WIFI-03

```
┌──────────────────────────────┐
│ To set up a new WiFi network,│
│ connect your phone to the    │
│ WiFi network named M5Setup   │
│ and enter your network's     │
│ name and password.           │
│                              │
│ ▸ Go back                    │
│                              │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: "Go back" → Return to previous WiFi screen (WIFI-01 or WIFI-02)
- PRESS_B: No effect (single option)
- HOLD_B: Go back → Return to previous WiFi screen

Events:

- ERROR_TIMEOUT: Display "Sorry, that didn't work." (remain in WiFi flow)

### Menu — WiFi — Failed connection — WIFI-04

```
┌──────────────────────────────┐
│ Could not connect to the     │
│ internet.                    │
│                              │
│                              │
│                              │
│                              │
│ ▸ Go back                    │
│   Try again                  │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option
  - "Go back" → Return to previous WiFi screen
  - "Try again" → Retry connection (on success → WIFI-01; on failure → stay on WIFI-04)
- PRESS_B: Next menu item
- HOLD_B: Go back → Return to previous WiFi screen

Events:

- CONNECT_SUCCESS: Go to WIFI-01
- CONNECT_FAILED: Remain on WIFI-04

### Menu — WiFi — No networks found — WIFI-05

```
┌──────────────────────────────┐
│ No WiFi networks found.      │
│                              │
│                              │
│                              │
│                              │
│                              │
│ ▸ Go back                    │
│   Try again                  │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option
  - "Go back" → Return to previous WiFi screen
  - "Try again" → Rescan (on networks found → WIFI-06/WIFI-07; none found → stay on WIFI-05)
- PRESS_B: Next menu item
- HOLD_B: Go back → Return to previous WiFi screen

Events:

- SCAN_RESULTS_AVAILABLE: Go to WIFI-06/WIFI-07 (first page)

### Menu — WiFi — Select network (single page) — WIFI-06

All known networks. Selecting a network takes you to WIFI-09.

```
┌──────────────────────────────┐
│ ▸ MyHomeNetwork123           │
│   OtherSavedNetwork          │
│   OtherSavedNetwork          │
│   OtherSavedNetwork          │
│   OtherSavedNetwork          │
│                              │
│                              │
│   Go back                  ● │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option
  - Any SSID → WIFI-09
  - "Go back" → Return to previous WiFi screen
- PRESS_B: Next item within list (no pagination)
- HOLD_B: Go back → Return to previous WiFi screen

Events:

- NETWORK_LIST_UPDATED: Refresh list contents

### Menu — WiFi — Select network (multi-page, first page) — WIFI-07

```
┌──────────────────────────────┐
│ ● MyHomeNetwork123           │  ← currently connected
│ ▸ OtherSavedNetwork          │  ← selected
│   OtherSavedNetwork          │
│   OtherSavedNetwork          │
│   OtherSavedNetwork          │
│                              │
│   Next page                  │
│   Go back                  ▾ │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option
  - Any SSID → WIFI-09
  - "Next page" → WIFI-08
  - "Go back" → Return to previous WiFi screen
- PRESS_B: Next item within current page; when highlight moves past the last item, advance to next page
- HOLD_B: Go back → Return to previous WiFi screen

Events:

- NETWORK_LIST_UPDATED: Refresh list contents

### Menu — WiFi — Select network (last page) — WIFI-08

```
┌──────────────────────────────┐
│ ▸ OtherSavedNetwork          │
│   OtherSavedNetwork          │
│   OtherSavedNetwork          │
│   OtherSavedNetwork          │
│                              │
│                              │
│   Next page                  │
│   Go back                  ● │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option
  - Any SSID → WIFI-09
  - "Add WiFi network" → WIFI-03
  - "Go back" → Return to previous WiFi screen
- PRESS_B: Next item within current page (no further pages)
- HOLD_B: Go back → Return to previous WiFi screen

Events:

- NETWORK_LIST_UPDATED: Refresh list contents

### Menu — WiFi — Selected network — WIFI-09

After selecting a network from the list.

```
┌──────────────────────────────┐
│ OtherSavedNetwork            │
│                              │
│                              │
│                              │
│                              │
│ ▸ Go back                    │
│   Connect to network         │
│   Forget network             │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option
  - "Go back" → WIFI-06/WIFI-07 (where user came from)
  - "Connect to network" → Attempt connection (on success → WIFI-01; on failure → WIFI-04)
  - "Forget network" → Remove SSID → WIFI-06/WIFI-07
- PRESS_B: Next menu item
- HOLD_B: Go back → WIFI-06/WIFI-07

Events:

- CONNECT_SUCCESS: Go to WIFI-01
- CONNECT_FAILED: Go to WIFI-04

### Menu — Resume Chat (single page) — RESUME-01

Lists previous conversations, displaying the most recent transcribed message (one line, truncated) from the assistant.

```
┌──────────────────────────────┐
│ ▸ What's on your mind?       │
│   Sure, here's a picture ... │
│   Haha! Good joke.           │
│                              │
│                              │
│                              │
│                              │
│   Go back                  ● │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option (chat item → resume that session in Chat; "Go back" → HOME-01)
- PRESS_B: Next item (no pagination)
- HOLD_B: Go back → HOME-01

Events:

- LIST_UPDATED: Refresh list contents

### Menu — Resume Chat (multi-page, first page) — RESUME-02

```
┌──────────────────────────────┐
│ ▸ What's on your mind?       │
│   Sure, here's a picture ... │
│   Haha! Good joke.           │
│   The capital of France i... │
│   Here's a snack made of ... │
│   The world is roughly 30... │
│                              │
│   Go back                  ▾ │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option (chat item → resume that session in Chat; "Go back" → HOME-01)
- PRESS_B: Next item; when past bottom and more pages available, advance to next page
- HOLD_B: Go back → HOME-01

Events:

- LIST_UPDATED: Refresh list contents

### Menu — Resume Chat (last page) — RESUME-03

```
┌──────────────────────────────┐
│ ▸ That sounds fun, why no... │
│   Rock beats paper!          │
│                              │
│                              │
│                              │
│                              │
│                              │
│   Go back                  ● │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option (chat item → resume that session in Chat; "Go back" → HOME-01)
- PRESS_B: Next item within current page (no further pages)
- HOLD_B: Go back → HOME-01

Events:

- LIST_UPDATED: Refresh list contents

### Menu — Resume Chat (go back selected) — RESUME-04

```
┌──────────────────────────────┐
│   That sounds fun, why no... │
│   Rock beats paper!          │
│                              │
│                              │
│                              │
│                              │
│                              │
│ ▸ Go back                  ● │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: "Go back" → HOME-01
- PRESS_B: Next item (moves highlight back into list)
- HOLD_B: Go back → HOME-01

Events:

- LIST_UPDATED: Refresh list contents

### Menu — Updates — Checking — UPDATES-01

```
┌──────────────────────────────┐
│ Checking for updates...      │
│                              │
│                              │
│                              │
│                              │
│                              │
│                              │
│ ▸ Go back                    │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: "Go back" → Return to previous screen
- PRESS_B: No effect (single option)
- HOLD_B: Go back → Return to previous screen

Events:

- UPDATE_AVAILABLE: Go to UPDATES-03
- UPDATE_NONE: Go to UPDATES-02
- ERROR_TIMEOUT: Display "Sorry, that didn't work." (remain or return to previous screen)

### Menu — Updates — None available — UPDATES-02

```
┌──────────────────────────────┐
│ No update available, you're  │
│ on the latest version.       │
│                              │
│                              │
│                              │
│                              │
│                              │
│ ▸ Go back                    │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: "Go back" → Return to previous screen
- PRESS_B: No effect (single option)
- HOLD_B: Go back → Return to previous screen

Events:

- UPDATE_AVAILABLE: Go to UPDATES-03

### Menu — Updates — Available — UPDATES-03

```
┌──────────────────────────────┐
│ Update available!            │
│                              │
│                              │
│                              │
│                              │
│                              │
│ ▸ Go back                    │
│   Install update             │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option ("Go back" → Return to previous screen; "Install update" → OTA-01)
- PRESS_B: Next menu item
- HOLD_B: Go back → Return to previous screen

Events:

- INSTALL_CONFIRMED: Start installing → OTA-01

### Reset Device — Confirmation — RESET-01

```
┌──────────────────────────────┐
│ Are you sure? Reset will     │
│ remove data and restart into │
│ the last working version.    │
│ WiFi credentials are kept.   │
│                              │
│                              │
│ ▸ Go back                    │
│   Reset device               │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option ("Go back" → Return to previous screen; "Reset device" → RESET-02)
- PRESS_B: Next menu item
- HOLD_B: Go back → Return to previous screen

Events:

- RESET_CONFIRMED: Go to RESET-02

### Reset Device — Resetting — RESET-02

```
┌──────────────────────────────┐
│                              │
│                              │
│                              │
│         Resetting...         │  ← shimmer, centered
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: No effect
- PRESS_B: No effect
- HOLD_B: No effect

Events:

- RESET_COMPLETE: Reboot → BOOT-01
- RESET_FAILED: Go to BOOT-02

### Installing Update — Updating — OTA-01

```
┌──────────────────────────────┐
│                              │
│                              │
│                              │
│          Updating...         │  ← shimmer, centered
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: No effect
- PRESS_B: No effect
- HOLD_B: No effect

Events:

- UPDATE_COMPLETE: Go to OTA-03
- UPDATE_FAILED: Go to OTA-02

### Installing Update — Failed — OTA-02

```
┌──────────────────────────────┐
│ Update failed.               │
│                              │
│                              │
│                              │
│                              │
│                              │
│ ▸ Try again                  │
│   Reset device               │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: Activate highlighted option ("Try again" → OTA-01; "Reset device" → RESET-02)
- PRESS_B: Next menu item
- HOLD_B: Go back → Return to previous screen

Events:

- RETRY_REQUESTED: Go to OTA-01

### Installing Update — Complete — OTA-03

```
┌──────────────────────────────┐
│ Update complete!             │
│                              │
│                              │
│                              │
│                              │
│                              │
│                              │
│ ▸ Restart device             │
└──────────────────────────────┘
```

Interactions:

- PRESS_A: "Restart device" → BOOT-01
- PRESS_B: No effect (single option)
- HOLD_B: Go back → Return to previous screen (if available)

Events:

- RESTART_CONFIRMED: Go to BOOT-01

### Sleep

Visual: two closed "eyes" (short horizontal lines) centered; floating z/Z glyphs upper-right; gray color.

## Power States

Power management dims or disables the display based on idle time. Power timers only advance while in the Ready state.

| State      | Trigger       | Display                 | WiFi |
| ---------- | ------------- | ----------------------- | ---- |
| Active     | User activity | Full brightness         | On   |
| Dimmed     | 60s idle      | Reduced brightness (48) | On   |
| ScreenOff  | 2 min idle    | Display off (0)         | On   |
| LightSleep | 5 min idle    | Display off             | Off  |
| PowerOff   | 10 min idle   | Device off              | Off  |

Waking from Dimmed/ScreenOff/LightSleep:

- Any button press begins waking (transitional Waking state)
- Button release finishes waking, restores Active state and full brightness
- If waking from LightSleep, WiFi reconnects and WebSocket re-establishes

## Controls

- **Button A**: Start recording (or resume during grace); interrupt generation/playback and start recording; select in menus.
- **Button B** (click): Paginate tool text and WiFi/Resume lists; cycle menu items; dismiss tool text.
- **Button B** (hold): Open menu from chat; go back/close menu.
- **During power-save** (Dimmed, ScreenOff, LightSleep): Either button press begins waking; release finishes.

## Copy Reference

- Boot: "Starting...", "Could not start device."
- Chat idle: "Hi, how can I help? Hold the big button and speak to get a response."
- Chat generating: "Thinking..."
- Chat failure: "Sorry, that didn't work."
- Chat no WiFi: "Could not connect to the internet.", "Set up WiFi", "Try again"
- Device menu: "M5 Live v{N}", "by Steve Ruiz"
- WiFi connected: "Connected to WiFi.", "Not connected to WiFi."
- WiFi setup: "To set up a new WiFi network, connect your phone to the WiFi network named M5Setup and enter your network's name and password."
- WiFi failure: "Could not connect to the internet.", "No WiFi networks found."
- Updates: "Checking for updates...", "No update available, you're on the latest version.", "Update available!"
- Reset: "Are you sure? Reset will remove data and restart into the last working version. WiFi credentials are kept.", "Resetting..."
- OTA: "Updating...", "Update failed.", "Update complete!"
