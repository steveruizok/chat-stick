# chat-stick

A real-time voice assistant running on an [M5StickS3](https://docs.m5stack.com/en/core/M5StickS3) (ESP32-S3). Hold a button, talk, release to hear the AI respond. Powered by [Gemini Live API](https://ai.google.dev/gemini-api/docs/live) via a Cloudflare Worker relay.

## Architecture

```
M5StickS3 ──WebSocket──▶ Cloudflare Worker (Durable Object) ──WebSocket──▶ Gemini Live API
  mic/speaker               relay + tool handling                           speech-to-speech AI
```

**Firmware** (`firmware/`) — PlatformIO/Arduino project for the M5StickS3. Records audio from the MEMS mic, streams it over WebSocket to the server, and plays back AI audio responses through the speaker.

**Server** (`server/`) — Cloudflare Worker with a Durable Object (`LiveSession`) that bridges the device to Gemini's Live API. Handles tool calls (tldraw docs search, web fetch, device control), stores conversation history in D1, and manages session lifecycle.

## Prerequisites

- [PlatformIO](https://platformio.org/install) (firmware builds)
- [Node.js](https://nodejs.org/) 18+ (server)
- [Wrangler CLI](https://developers.cloudflare.com/workers/wrangler/install-and-update/) (`npm i -g wrangler`)
- A [Google AI Studio](https://aistudio.google.com/) API key with Gemini Live API access
- A Cloudflare account (for Workers, D1, Vectorize)

## Setup

### Server

```bash
cd server
npm install

# Copy the example env file and add your API keys
cp .dev.vars.example .dev.vars
# Edit .dev.vars with your GEMINI_API_KEY

# Create the D1 database
wrangler d1 create m5-live-conversations
# Update the database_id in wrangler.toml with the ID from the output above

# Apply the schema
wrangler d1 execute m5-live-conversations --local --file=schema.sql

# Run locally
npm run dev
```

### Firmware

```bash
cd firmware

# Copy the credentials template and fill in your WiFi networks
cp src/credentials.h.example src/credentials.h
# Edit src/credentials.h with your WiFi SSIDs and passwords

# Update src/Config.h if needed:
#   - SERVER_ENDPOINTS: set your local dev IP or deployed worker URL
#   - upload_port/monitor_port in platformio.ini: set to your device's serial port

# Build and upload
pio run -t upload

# Monitor serial output
pio device monitor
# or: python monitor.py
```

### Deploy

```bash
# Deploy the worker
cd server
wrangler secret put GEMINI_API_KEY
wrangler secret put HISTORY_API_TOKEN
wrangler deploy

# Update firmware to point at the deployed endpoint
# The default Config.h already includes m5-live.tldraw.workers.dev as a fallback
```

## Credentials

All secrets are gitignored. You need to create these files locally:

| File | Purpose | Template |
|------|---------|----------|
| `server/.dev.vars` | Gemini API key, history token | `server/.dev.vars.example` |
| `firmware/src/credentials.h` | WiFi network SSIDs and passwords | `firmware/src/credentials.h.example` |

Never commit credentials. The `.gitignore` is configured to exclude these files.

## Hardware

- **Device**: M5StickS3 (ESP32-S3, 135x240 LCD, MEMS mic, 1W speaker)
- **Buttons**: A (GPIO 11) = push-to-talk, B (GPIO 12) = menu/control
- **Audio**: 16kHz input / 24kHz output PCM

## Test Data

`test-data/` contains sample `.m4a` audio files for development and testing.

## License

MIT
