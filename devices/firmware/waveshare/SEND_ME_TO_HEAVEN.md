# Send Me to Heaven

A standalone toss-height game for the Waveshare ESP32-S3 Touch AMOLED 1.8.
It uses the onboard QMI8658 accelerometer to time the interval in which the
device is in freefall, then estimates peak height with `h = g * t^2 / 8`.

## Flash

With the device connected over USB:

```bash
cd devices/firmware/waveshare
pio run -e send-me-to-heaven -t upload --upload-port /dev/cu.usbmodem101
```

This is a separate PlatformIO environment. The normal
`waveshare-esp32-s3-touch-amoled-1_8` environment still builds the voice
assistant. Display initialization automatically supports both the original
SH8601/FT3168 board and the newer CO5300/CST820 V2 revision.

## Play

1. Read the safety screen and press BOOT to acknowledge it.
2. Unplug USB. The game will not arm while a cable is attached.
3. Use a fitted battery and a protective case. Stand over a large padded area,
   clear people, pets, lights, fans, and hard objects, and keep the toss low.
4. Press BOOT to arm, gently toss straight up, and catch the device close to
   its release height.

Pressing BOOT while armed cancels the attempt. The best result is saved in the
ESP32's non-volatile storage.

## Debug console and screenshots

The game build includes the same serial screenshot and synthetic-input console
as the translator firmware. It is development-only and is not part of the
normal Waveshare voice-assistant build.

Capture the current framebuffer, optionally selecting a preview screen first:

```bash
python3 devices/firmware/tools/screenshot.py /tmp/home.png \
  --command "screen home"
python3 devices/firmware/tools/screenshot.py /tmp/result.png \
  --command "screen result"
```

Open an interactive console to manually advance with `tap 184 336` or
`btn a click`, or jump directly with `screen home`, `screen armed`, `screen
result`, `screen highscore`, and `screen cable`:

```bash
python3 devices/firmware/tools/device_console.py
```

Capture the complete screen set with:

```bash
python3 devices/firmware/tools/capture_game_screens.py /tmp/heaven-screens
```

Each tool auto-detects the USB serial port. Pass `--port` when more than one
USB serial device is attached.

## Accuracy

The result is an estimate derived from airtime. It assumes the catch and
release heights are approximately equal. A catch at a different height, air
drag, a fumbled catch, or motion that does not contain a clean freefall interval
will affect the result. Short or implausibly long events are rejected.
