# TODO

## Menu System

Port the menu system from the m5-stick project. Currently Button B only clears tool text — there is no menu.

- [ ] Add state machine (AppStateMachine) with regions: Initializing, Chat, Menu
- [ ] Define MenuState enum and MenuOption structures
- [ ] Home menu: Go back, New chat, Resume chat, Device
- [ ] Device submenu: Go back, Set up WiFi, Check for updates, Turn off
- [ ] Button mapping: A = select, B click = cycle items, B hold = back/open menu
- [ ] Bottom-up menu rendering (max 4 visible items, selection marker `▸`)
- [ ] Pagination support with footer indicator (▾ more, ● last page)

## Button State Machine

The reference project has a proper ButtonStateMachine with press/hold/click/double-click differentiation. chat-stick uses raw `wasPressed`/`wasReleased` checks.

- [ ] Port ButtonStateMachine with configurable hold thresholds (A: 500ms, B: 1000ms)
- [ ] Button A: hold to record (not press), release to stop
- [ ] Button B: click to cycle pages/menu items, hold to open/close menu
- [ ] Wake-from-sleep: consume button events during Waking state (prevent accidental actions)

## WiFi Setup

Currently WiFi credentials are hardcoded in `credentials.h`. Port the WiFi provisioning system so users can add/manage networks without rebuilding firmware.

- [ ] NVS-backed credential storage (Preferences API, up to 5 saved networks)
- [ ] WiFi menu screens: connected state, disconnected state, searching, connecting, failed
- [ ] Network detail screen: Connect, Forget, Go back
- [ ] Paginated saved network list (NVS-provisioned first, then hardcoded fallback)
- [ ] Captive portal provisioning (soft AP + DNS redirect + HTML form)
- [ ] Update WiFiService to support dynamic SSID/password from NVS
- [ ] Keep hardcoded credentials.h networks as fallback candidates

## Resume Chat

- [ ] Resume chat menu (paginated conversation history from server)
- [ ] Fetch recent conversations via `/history/:deviceId` endpoint
- [ ] Reconnect to existing chat_id on selection
- [ ] Boot restore: fetch last AI message for current session on startup

## NVS Persistence

The reference project persists device settings across reboots. chat-stick resets everything on power cycle.

- [ ] Persist brightness level
- [ ] Persist volume level
- [ ] Persist current chat_id (so boot restore works)
- [ ] Persist WiFi credentials (see WiFi Setup above)

## Recording UX

The reference project has recording polish that chat-stick lacks.

- [ ] Recording grace period (500ms after release to allow continuation by re-pressing A)
- [ ] Recording progress bar (red vertical bar on right edge, fills with duration)
- [ ] Max recording time with visual indicator
- [ ] Accidental recording detection (server returns ignore flag for very short/silent clips)

## Device Actions

chat-stick handles `set_brightness`, `set_volume`, `show_text`, and `get_device_status` on-device. The reference project supports more.

- [ ] `play_sound` tool call: beep, success, error, alert, fanfare
- [ ] `play_melody` tool call: custom note sequences ("C4:200 E4:200 G4:400")
- [ ] `power_off` tool call: shut down the device
- [ ] `new_chat` tool call: generate fresh chat_id (currently server-side only via `new_conversation`)
- [ ] Add these to the Gemini tool declarations in `live-session.ts`

## Display

- [ ] Double-buffered sprite rendering (M5Canvas) to prevent flicker
- [ ] Shimmer animation for loading/connecting states (diagonal gray→white wave)
- [ ] Image rendering from tool calls (1-bit dithered bitmap, cover-scaled to 232×112)
- [ ] Image pulse animation while generating
- [ ] Recording progress bar
- [ ] Page indicator in footer for multi-page tool text

## Boot Sequence

- [ ] Minimum boot display time (~800ms)
- [ ] Boot error recovery screens (Try again, Reset device)
- [ ] WiFi failure → offer Set up WiFi flow instead of dead-ending on error
- [ ] Session restore on boot (show last AI message from server)

## OTA Updates

- [ ] Firmware version constant in Config.h
- [ ] Server endpoints: `/firmware/check?version=N`, `/firmware/download`
- [ ] Store firmware binaries in R2
- [ ] Menu flow: Check for updates → Install → Progress bar → Restart
- [ ] Integrity checks (MD5/signature)

## Server

- [ ] Vectorize integration — wire up vector search as primary with keyword fallback
- [ ] Image generation endpoint (`/image`) with 1-bit dithering for device display
- [ ] Session restore endpoint (`/session/:chatId` → last AI message)
- [ ] R2 storage for generated images
- [ ] Location-aware context from CF geolocation headers
- [ ] Rate limiting on WebSocket connections

## Firmware Polish

- [ ] Error categories (WiFi timeout, server refused, Gemini unavailable) instead of plain text
- [ ] Audio buffer fallback for non-PSRAM boards
- [ ] Configurable power timeouts (server-pushed settings)
- [ ] Reset device flow (clear NVS, confirmation screen)
