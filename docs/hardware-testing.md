# Local hardware testing without a model API key

`server/scripts/hardware-fixture.ts` implements a small device-protocol fixture.
It runs in a local Wrangler Durable Object with no remote bindings or model requests. It counts
incoming PCM bytes and peak magnitude without saving audio. The reply is a quiet
one-second 440 Hz tone plus a fixed Turkish transcript; it is **not** transcription.
Client logs are ignored, and network names/endpoints are omitted from stored status
responses. State is in memory and disappears on server restart.

Use only on a trusted test LAN; never deploy this fixture. Its public test marker
`local-hardware-test` is not a production credential. It binds all interfaces so
the device can connect; stop it after testing. Back up and verify existing device
flash before overwriting firmware, and keep that backup private.

## Setup

```bash
cd server
npm ci
npm run dev:hardware
```

Copy the device's `credentials.h.example` to the ignored `credentials.h`. Configure
one endpoint: your computer's LAN address on port 8787, with no CA for local HTTP.
Set the device token to the test marker and supply your WiFi credentials locally.
Do not leave production fallback endpoints enabled for this test.

Build/flash `devices/firmware/m5-stick` with PlatformIO. Do not publish its binary:
credentials are embedded. The fixture's OTA check intentionally reports no update.

```bash
curl -H 'X-Device-Token: local-hardware-test' http://localhost:8787/__test/state
curl -H 'X-Device-Token: local-hardware-test' -H 'Content-Type: application/json' \
  -d '{"name":"get_device_status"}' http://localhost:8787/__test/control
```

## Device checks

1. Confirm `connected: 1`. Set a modest volume before testing the speaker:
   `{"name":"set_volume","args":{"level":80}}` at `/__test/control`.
2. Hold Button A, speak for a few seconds and release. Check starts/stops, nonzero
   PCM bytes/peak, the fixed Turkish text and the audible tone. Repeat to exercise
   microphone-to-speaker switching. These counters do not measure recognition quality.
3. Send `{"name":"show_text","args":{"text":"Türkçe: çğıöşü ÇĞİÖŞÜ"}}`.
   Inspect glyphs on the physical display and check idle noise between replies.
4. Send `{"action":"disconnect"}` and confirm the device reconnects and answers
   another status request.
5. Timer smoke test: `{"name":"set_timer","args":{"duration_seconds":15,"name":"Türkçe"}}`.
   Confirm the alarm and cancellation using `cancel_timer` with `{"all":true}`.
6. For a separate sleep test, send a settings frame using
   `{"action":"settings","frame":{"power":{"dim_ms":5000,"screen_off_ms":10000,"light_sleep_ms":15000,"power_off_ms":20000}}}`.
   Set a timer first if testing automatic wake. The StickS3 configuration permits
   idle deep sleep even on USB power. Verify actual transitions in serial logs;
   after timer wake, dismiss the alarm with Button A before expecting a new network
   connection. Test battery-only behavior separately. A status acknowledgement or
   a server-side socket count alone is not proof of sleep/wake or a live connection.

All control examples are JSON bodies posted with the same headers as above.
`/__test/state` retains only the last 30 tool responses. Use serial logs to diagnose
device behavior, but redact network names, addresses and device identifiers before sharing.

## Physical validation record (2026-10-03)

Hardware: M5StickS3 / ESP32-S3-PICO-1 with 8 MiB flash and PSRAM.
Full original flash backup: independent digest verification passed before flashing.
Candidate firmware: release build, flash write and written-data hash verification passed.

| Check | Result and evidence |
| --- | --- |
| WiFi and device WebSocket | Connected to the local fixture; status, volume and text commands returned device acknowledgements |
| Repeated hold/release recording | Two focused trials produced two start/stop pairs, 51 PCM chunks and 163,200 bytes (16 kHz mono PCM16); peak magnitude was nonzero (32,752) |
| Playback after capture | The operator confirmed the one-second reply tone in both trials |
| Turkish display | Operator confirmed `çğıöşü ÇĞİÖŞÜ` rendered correctly |
| Idle audio noise | Operator reported no hiss after playback returned to idle |
| Connection recovery | Fixture closed the socket; device reconnected and answered another status request |
| Timer expiry | A 15-second timer appeared in `list_timers`, expired in the serial log and was absent in the next response |
| Dim, screen off, timed deep sleep | Serial log recorded `Active -> Dimmed -> ScreenOff -> PowerOff`, deep sleep, `reset reason=deepsleep`, `wake ... cause=timer` and the saved timer expiring after boot |

The timed-wake test was performed with USB connected, using shortened timeouts and
a 45-second saved timer. The device entered its alarm screen after wake; automatic
network reconnection is deferred until the alarm is dismissed. Default timeouts are
restored by the reboot. No battery runtime conclusion is drawn from this test.

Relevant serial events (timestamps and unrelated device details omitted):

```text
Power - Active -> Dimmed
Power - Dimmed -> ScreenOff
Power - ScreenOff -> PowerOff
Power - deep sleep 5000000 us until next timer
Boot - reset reason=deepsleep
Boot - wake from deep sleep cause=timer
Timers - expired count=1 names="wake-test"
```

Two observations remain for follow-up: status reported an unknown power-source label
(`?`) despite a measured USB input around 5.2 V, and a nonfatal I2S
`i2s_driver_uninstall` diagnostic appeared during audio switching. Audible repeated
playback succeeded; these diagnostics have not been investigated or labeled fixed.

This fixture does not verify real Gemini conversation, production Cloudflare storage,
OTA flashing, battery life, or other device models. Results must distinguish serial
evidence, fixture counters and a human's visual/audible confirmation.
