# Reliability review and validation scope

Review baseline: `c2709cf75a322c4efe6d6a9aa678d8eaccf90840` (2026-10-02).
This contribution keeps the existing Gemini/Cloudflare architecture and device protocol.
It addresses reproducible defects without replacing the application or changing its visual layout.

## Fixed issues

| Observed problem | Change | Evidence |
| --- | --- | --- |
| M5StickS3 build fails when aggregate defaults are compiled as GNU C++11 | Select GNU C++17; pin the tested platform and M5 dependencies | M5StickS3 PlatformIO build; CI repeats with placeholder credentials |
| OTA binary download skips the device-token check used by OTA discovery | Apply the same optional-token policy; send the token during firmware download | Router authentication regressions |
| Firmware discovery considers only the first R2 list page | Follow cursors while preserving device lineage and legacy fallback | Multi-page R2 stub tests |
| Session lookup reveals missing vs existing chat before authorization | Authorize before querying D1 | Route test proves rejected requests never access D1 |
| A supplied foreign chat ID can be used for writes | Reject ownership mismatch before replacing the active socket; scope history writes and logs to both IDs | Ownership and queued-write regression tests |
| Failed exchange writes lose pending transcripts; concurrent commits race | Capture immutable turns, serialize writes, retain failures for retry, batch history and log transactionally | Failed-batch retry and overlapping-turn tests |
| Idle closure is not rearmed after subsequent input | Schedule the next idle alarm when starting Gemini again; recheck activity after awaited persistence | Idle/restart and concurrent activity regressions |
| Old asynchronous operations can send output or change a replacement session | Capture session/socket identity and check continuations, including image/animation and thinking/reconnect paths | Deferred-operation replacement tests |
| URL fetches follow redirects to unchecked destinations and can hang after headers | Validate every redirect; cap redirects, bytes and the complete response duration | Private/relative redirects, loops, stalled headers/body and size-limit tests |
| OTA falls back to unauthenticated TLS and follows redirects | Require configured origin/device route and CA for HTTPS; permit only configured LAN HTTP; disable redirects | Host OTA policy tests and firmware build |
| M5StickS3 leaves unused boost/amplifier circuitry enabled | Disable external 5V for the internal speaker; gate PM1 GPIO3 around playback/capture/idle/sleep | Build plus physical tests recorded separately below |
| Playback queue failures are ignored; asynchronous speaker stop can outlive buffer reset | Show a retryable playback error, validate PCM frame size and synchronize stopping before buffer reuse; defer speaker reconfiguration during recording | Firmware build; physical microphone/playback cycling; buffer saturation still needs stress testing |
| Turkish UTF-8 is discarded by the fixed-cell display | Decode supported code points into 12 glyphs within the existing 8×16 cells and alarm rendering | Sanitized host tests; physical visual confirmation recorded separately |
| Validation scripts have no common entry point or CI | Add npm test/typecheck, sanitized host firmware tests, M5StickS3 compile workflow and local device fixture | Offline commands described in README |

## Remaining work and suggested solutions

These are proposals, not completed guarantees.

| Area | Remaining limitation | Suggested next step |
| --- | --- | --- |
| Device identity | Optional shared token intentionally preserves existing single-owner compatibility. A token holder can still choose any device ID. | Provision a separate credential per device; bind identity at the router and authorize history/files/images against it. Offer an explicit local-development mode instead of silently changing existing installations. |
| Durable persistence | Retry queue is in memory. Process eviction can lose it; a committed write with a lost response can be duplicated. | Persist an outbox in Durable Object storage and give each exchange a unique ID enforced by D1, with recovery and ambiguous-commit tests. |
| Web destinations | URL checks do not resolve hostnames or prevent DNS rebinding; this is not a complete SSRF boundary. | Restrict tools to an allowlist or a controlled egress service which validates resolved destinations at connection time. |
| OTA authenticity | Origin/CA checks do not cryptographically sign firmware or protect against a compromised authorized server. LAN HTTP has no transport confidentiality. | Signed manifests/images, pinned verification keys, version policy and boot-confirmation/rollback tests. Keep credential-bearing binaries private. |
| Audio latency | Capture still blocks for roughly one 100 ms chunk; playback uses bounded PSRAM/heap buffers. | Measure capture-to-send and playback underruns first; then consider a bounded DMA/ring-buffer task design. Stress test long replies and allocation failure. |
| Battery | Disabling unused power circuits is not a measured runtime improvement. | Measure current and full discharge in idle, screen-off, active conversation and timer wake modes, with brightness/volume/network conditions recorded. |
| Hardware diagnostics | This unit reported an unknown PM1 power-source label on USB and an I2S uninstall diagnostic during otherwise working audio transitions. | Check PM1 firmware/register semantics and the driver's init/end lifecycle; add physical regression coverage before changing either. |
| Architecture | Large controller/session modules mix provider, tools, storage and transport. | Extract small tested boundaries incrementally if adding another provider. Retain reusable device protocol, display/image conversion and hardware services. |
| Platform coverage | Shared firmware changes affect other devices, but only StickS3 hardware is available here. | Build and physically exercise Waveshare, stopwatch and external-speaker paths before asserting those platforms are validated. |
| Model integration | Configured model IDs and real service access are account-dependent. | Run real Gemini speech, tool, history and resumption acceptance tests with an authorized API key. |

For context, the baseline source inventory was 17,963 code lines across 99 source files
(`cloc`, excluding comments, blank lines, dependencies, builds and assets): shared firmware
6,720; M5StickS3 998; other device firmware 3,653; server 4,675; Android 1,671; root scripts
246. Shared source includes other device implementations, so this is not the size of a
single StickS3 firmware image. The repository had 135 tracked files.

## Validation record

Local offline checks: server regression scripts, TypeScript, C++17 host tests with
AddressSanitizer/UBSan, and the M5StickS3 PlatformIO release build pass. The candidate
build uses espressif32 6.13.0, Arduino 2.0.17, M5Unified 0.2.25, M5GFX 0.2.32,
M5PM1 1.0.7, ArduinoJson 7.4.3 and ArduinoWebsockets 0.5.4.

Before physical testing, a full 8 MiB backup of the existing StickS3 flash was read
and independently verified against the device with esptool (digest matched).
The private backup and credential-bearing candidate binary are not included here.

Physical test results are recorded in [hardware testing](hardware-testing.md).
No Gemini key, paid model call, production deployment, real D1/R2 integration test,
OTA installation, battery discharge measurement or non-StickS3 hardware test is
part of this validation.
