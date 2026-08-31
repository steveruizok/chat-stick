# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A handheld voice assistant built on ESP32-S3 devices: M5StickS3 and Waveshare ESP32-S3 Touch AMOLED 1.8. Hold a button, talk, release to hear the AI respond. Audio streams over WiFi/WebSocket to a Cloudflare Worker, which relays it to Google's Gemini 3.1 Live API for speech-to-speech AI.

```
ESP32-S3 device ──WebSocket──▶ Cloudflare Worker (Durable Object) ──WebSocket──▶ Gemini Live API
  mic/speaker/display        relay + tool handling                           speech-to-speech AI
```

## Repository Structure

Two independent codebases in one repo:

- **`devices/firmware/m5-stick/`** — PlatformIO/Arduino C++ project targeting M5StickS3
- **`devices/firmware/waveshare/`** — PlatformIO/Arduino C++ project targeting Waveshare ESP32-S3 Touch AMOLED 1.8. Two PlatformIO envs from one source tree: `waveshare-v1` (original board, SH8601 panel, OTA device `waveshare`) and `waveshare-v2` (V2 board, CO5300 panel, OTA device `waveshare-v2`). The panel is selected at compile time via `-DWAVESHARE_PANEL_SH8601` / `-DWAVESHARE_PANEL_CO5300`; only `Config.h` and `hal/Board.cpp` branch on it
- **`server/`** — Cloudflare Worker (TypeScript) with Durable Objects, D1, Vectorize, Workers AI

## Build & Run Commands

### Server (Cloudflare Worker)

```bash
cd server
npm install
npm run dev          # wrangler dev (local)
npm run deploy       # wrangler deploy (production)

# D1 migrations
wrangler d1 migrations apply --local

# Docs indexing
DOCS_PATH=/path/to/docs npx tsx scripts/build-docs-index.ts
wrangler dev scripts/index-docs-worker.ts --port 8799
curl http://localhost:8799/index
```

### Firmware (PlatformIO)

```bash
cd devices/firmware/m5-stick      # or devices/firmware/waveshare
pio run -t upload          # build & flash (waveshare: add -e waveshare-v1 or -e waveshare-v2)
pio device monitor         # serial monitor (115200 baud)
python monitor.py          # alternative serial monitor
```

Serial port is configured in `platformio.ini` (`upload_port`/`monitor_port`). Update these when switching USB ports.

## Architecture Details

### Server

- **Entry point**: `server/src/index.ts` — HTTP router handling `/ws`, `/health`, `/history/:deviceId`, `/session/:chatId`, `/firmware/check`, `/firmware/download`, and admin endpoints
- **`LiveSession` Durable Object** (`server/src/live-session.ts`) — one instance per device. Manages dual WebSocket connections (device ↔ Gemini), routes tool calls, tracks transcriptions, persists conversation history to D1. The full tool schema sent to Gemini lives here — when adding/renaming a tool, update both the declaration array and the `case` dispatch
- **Tool call routing**:
  - Server-side tools (handled in `live-session.ts`): `search_docs`, `web_fetch`, `new_conversation`, `new_chat`; file-CRUD `list_files`, `read_file`, `write_file`, `append_to_file`, `search_files`; image tools `show_image`, `show_animation`, `list_recent_images`, `search_images`, `show_saved_image`; and `email_me` when email is configured
  - Device-side tools (forwarded as JSON over the device WebSocket; response relayed back to Gemini): `set_brightness`, `set_volume`, `set_speaker`, `set_external_speaker_gain`, `set_voice`, `show_text`, `play_sound`, `play_melody`, `power_off`, `get_device_status`, and timer tools `set_timer`, `list_timers`, `cancel_timer`, `extend_timer`
- **Image generation** (`server/src/image-gen.ts`, `server/src/images.ts`) — `show_image` calls Google Imagen, dithers to a 232×112 1-bit bitmap (chat text area, rows 1–7 of the 240×135 LCD — see `designs.md`), sends packed bits to the device, and stores both the dithered + original PNGs in R2 plus a record in the D1 `images` table. `list_recent_images` / `search_images` / `show_saved_image` recall by id without regeneration. While generation runs, the server sends `show_image_pending` / `show_image_failed` frames so the device can show a pulse animation. `show_animation` generates a 2–5 frame flipbook: preferred path is ONE model call drawing all frames as a grid of equal sections in a single image, sliced server-side (consistent + fast); fallback is chained per-frame generation using the previous frame as reference. Frames go to the device as `show_image` (frame 0) then `animation_frame` messages and the device flips them every 500ms. Frames share an `animation_group` in the D1 `images` table so `show_saved_image` replays the whole animation
- **Optional email** (`server/src/email.ts`) — `email_me` tool is only declared to Gemini when the `[[send_email]]` binding plus `EMAIL_SENDER`/`EMAIL_RECIPIENT` secrets are all present. Cloudflare Email Routing requires the recipient to be pre-verified, so this is for self-notifications only. See README "Optional: Email Notifications"
- **Docs search** (`server/src/docs-search.ts`) — keyword search (in-memory JSON index) with vector search fallback (Cloudflare Vectorize + Workers AI embeddings)
- **Device files** (`server/src/files.ts`) — device-scoped notes/files in D1, every query filtered by `device_id`. `MAX_FILE_BYTES = 100_000`. Append uses SQL concat for atomicity (no read-then-write)
- **D1 schema** — authoritative source is `server/migrations/`: `0001_initial.sql` (`conversations`/`message_log`/`tool_log`), `0002_files.sql` (`files`), `0003_images.sql` (`images`), `0004_animations.sql` (animation grouping columns on `images`). `server/schema.sql` is a snapshot reference. Apply with `wrangler d1 migrations apply --local` (or `--remote`)

### Firmware

- **Device projects** live under `devices/firmware/<device>/`; shared behavior is intentionally mirrored between the M5 and Waveshare firmware where hardware allows.
- **`main.cpp`** — thin shell delegating to `AppController`
- **`AppController`** (`app/`) — central coordinator. Owns all services, manages state machine (`AppState`: Connecting → Ready → Recording → Thinking → Playing), handles button input, menu navigation, and display updates
- **Services** (`services/`):
  - `LiveSessionService` — WebSocket client to the server; handles connection, audio streaming, tool call dispatch, history fetches, firmware update checks
  - `AudioService` — mic capture (16kHz) and speaker playback (24kHz PCM)
  - `WiFiService` — multi-network connection with saved credential support
  - `SettingsStore` — NVS persistence for brightness, volume, voice, speaker settings, and legacy chat_id cleanup
  - `TimerService` — countdown timers persisted in NVS, surviving reboots. Stable monotonic `id` per timer (never reused); `harvestExpired()` is polled by `AppController` so the device can chime + show a bell glyph when a timer fires. Tool dispatch happens device-side, not server-side
- **`ButtonStateMachine`** (`input/`) — debounced press/long-press/release detection for A (push-to-talk) and B (menu) buttons
- **`PowerManager`** (`power/`) — firmware-owned idle timeout cascade: dim → screen off → power off
- **`TextDisplay`** (`ui/`) — 135×240 LCD rendering with header/body/footer layout, menus, and state-based coloring
- **`Config.h`** — all hardware constants, server endpoints, audio rates, pin assignments, power timeouts

### Key Data Flows

1. **Voice exchange**: Button A press → mic capture at 16kHz → PCM chunks over WebSocket → server base64-encodes → Gemini `realtimeInput` → Gemini responds with audio parts → server decodes base64 → raw PCM binary frames back to device → speaker playback at 24kHz
2. **Tool calls**: Gemini emits `toolCall` → server handles server-side tools directly, forwards device-side tools as JSON → device executes and sends `tool_response` → server relays `toolResponse` to Gemini
3. **Startup session**: On boot, device starts with no `chat_id` so the server creates a fresh conversation. Previous conversations remain available through the Resume chat menu.

## Credentials

All deployment-specific config is gitignored. Templates exist at:
- `server/.dev.vars.example` → `server/.dev.vars` (GEMINI_API_KEY, HISTORY_API_TOKEN, optional ADMIN_API_TOKEN, DEVICE_AUTH_TOKEN, EMAIL_SENDER/EMAIL_RECIPIENT)
- `server/wrangler.toml.example` → `server/wrangler.toml` (Cloudflare bindings — D1 database_id, optional `[[send_email]]` and `[[r2_buckets]]` blocks). The committed `wrangler.toml.example` is the canonical structure; do not edit `wrangler.toml` expecting forks to inherit it
- `devices/firmware/<device>/src/credentials.h.example` → `devices/firmware/<device>/src/credentials.h` (server endpoints, optional device token, WiFi networks)

## Audio Format

- Input (mic → Gemini): 16-bit PCM, 16kHz, mono
- Output (Gemini → speaker): 16-bit PCM, 24kHz, mono
- Short/silent clips are detected and ignored (MIN_TURN_BYTES, SILENCE_AVG_ABS_THRESHOLD)

## Convenience Scripts (repo root)

- `./flash.sh [m5-stick|waveshare-v1|waveshare-v2|stack-chan] [--monitor]` — build firmware and upload over USB
- `./deploy.sh` — `wrangler deploy` the Cloudflare Worker
- `./publish-ota-release.sh [m5-stick|waveshare-v1|waveshare-v2]` — bump version if needed, build firmware, and upload `firmware-v<N>.bin` to R2 under `chat-stick/firmware/<device>/` (`waveshare-v1` publishes as device `waveshare`)
- `./publish.sh` — publish OTA + deploy worker (chains the two above)

## Releasing Firmware (OTA)

1. Run `./publish-ota-release.sh m5-stick`, `./publish-ota-release.sh waveshare-v1`, or `./publish-ota-release.sh waveshare-v2`
2. Devices running an older version pick up the new binary on next boot via `/firmware/check` → `/firmware/download`

The worker auto-detects the highest `firmware-v<N>.bin` per device in R2; there's no separate version registry. Valid device names are whitelisted in `FIRMWARE_DEVICE_IDS` in `server/src/index.ts` — unknown names silently fall back to `m5-stick`, so add any new `FIRMWARE_DEVICE` there too. Because `credentials.h` is compiled into the binary, **never commit or publish built `.bin` files** — `strings` will surface WiFi creds and the worker URL.

## Display Layout

`designs.md` is the canonical reference for screen layout (header/body/footer rows, the 232×112 chat-area bounding box used for images, glyph metrics). Consult it before changing `TextDisplay` or anything that draws on the LCD — the image-generation pipeline depends on those exact dimensions.
