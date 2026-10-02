# M5 StopWatch chat firmware

Full voice client using the shared chat controller, WiFi setup, live session transport, settings, timers, and power state machine. Shares the Waveshare AMOLED renderer/SmartBrick font and M5 audio service. M5Unified handles the StopWatch display, ES8311 codec, microphone and amplifier; the board adapter handles M5PM1 battery/power and GPIO buttons.

- **G2 / yellow = A**: hold to speak, release to send; select in menus.
- **G1 / blue = B**: click to page/cycle; hold for menu/back.
- **Home → Thinking**: toggle standard/Extended Thinking (medium). Ask for low/medium/high or to turn thinking off by voice. Mode is saved per conversation.
- **A + B hold**: existing factory reset prompt.
- Conversation text follows the circular screen with a 12px radial inset and eight vertically centered rows per page. Menus retain their centered layout. New images use the full 466 × 466 canvas; the round panel clips the corners.
- Separate device/chat ID and `m5-stopwatch` OTA lineage. Firmware rejects OTA URLs for another board, including older workers that default unknown devices to M5Stick.
- Idle deep sleep is disabled until RTC/button wake has been validated on this board. Screen dim/off and PM1 power-off remain available; USB prevents idle power-off.

Copy `src/credentials.h.example` to `src/credentials.h` and configure your deployment, then run from the repository root:

```sh
./flash.sh m5-stopwatch --monitor
```

Build only: `pio run -d devices/firmware/m5-stopwatch`. Publish OTA with `./publish-ota-release.sh m5-stopwatch` after deploying the worker that recognizes this device ID. Credentials and firmware binaries stay local/ignored.

Hardware reference: https://docs.m5stack.com/en/core/StopWatch
