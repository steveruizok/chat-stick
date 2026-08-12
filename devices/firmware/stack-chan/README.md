# Stack-Chan firmware

Hardware bring-up firmware for the M5Stack Stack-Chan (K151) and its CoreS3
controller. This is intentionally a capability-first scaffold rather than a
two-button port of the handheld firmware.

## Included

- Animated face with blinking and seven expressions.
- Safe, clamped yaw/pitch movement through the official StackChan BSP.
- Push-to-talk on the three-zone top touch control and capacitive screen taps.
- Expression-linked control of all 12 body RGB LEDs.
- Battery voltage/current reporting.
- Serial console for exercising hardware without rebuilding.
- Device-hosted control website with live telemetry and safe head controls.
- Half-duplex audio exchange between the CoreS3 and control website, with
  independent device microphone and speaker mute controls.
- Native CoreS3 camera capture through Espressif `esp_video`, exposed as an
  MJPEG stream alongside the control website.

NFC, infrared, IMU-driven behavior, Wi-Fi voice sessions, and OTA are left as
explicit next layers. The CoreS3 and body hardware support
all of them, but they deserve robot-oriented services rather than being hidden
inside the existing handheld adapters.

## Build and flash

Use the USB-C port on the base so unexpected servo movement cannot strain the
upper cable. To enter download mode, hold the bottom reset button for three
seconds until the adjacent indicator turns green.

```bash
cd devices/firmware/stack-chan
pio run
pio run -t upload
pio device monitor
```

From the repository root:

```bash
./flash.sh stack-chan --monitor
```

The first dependency install can take a few minutes because PlatformIO fetches
the official StackChan BSP and builds the Espressif 5.5 camera components. Later
builds use the generated framework cache. The `stack-chan` environment remains
available as a camera-free recovery build; the default is
`stack-chan-native-camera`.

## Controls

- Tap the screen to cycle expressions.
- Hold the three-zone sensor on top to record a push-to-talk message; release
  it to make the message available to the control website.

The firmware never commands a vertical angle outside 5-85 degrees. Motion only
happens after deliberate touch or serial input; boot itself does not reposition
the head.

## Control website

By default Stack-Chan only joins a configured 2.4 GHz network. It does not
advertise an access point, which prevents a phone or computer from
automatically leaving its normal Wi-Fi for the robot's network.

Copy `src/credentials.h.example` to `src/credentials.h`, enter the network
details, and reflash. The serial boot log prints the assigned address; mDNS
also makes `http://stack-chan.local` available on compatible networks.

For a deliberate offline setup session, set `kEnableControlAccessPoint` in
`src/Config.h` to `true`. The temporary network is:

```text
Network:  stack-chan-control
Password: stack-chan
Website:  http://192.168.4.1
```

The website shows battery, physical servo feedback, motion state, top-touch
intensity, expression, and the live camera stream. The square head pad drives
yaw and pitch; firmware safety clamps still apply to every request. Camera
capture uses the device's native `esp_video`/V4L2 path with its frame buffer in
PSRAM, avoiding the internal-DMA exhaustion caused by the generic Arduino
camera driver.

The Walkie-talkie card can automatically play messages recorded by holding the
top touch control. Click `Enable walkie-talkie` once in the browser, hold the
top control while speaking, and release it to send the message to the page. A
manual five-second device recording remains available. In the other direction
the page records up to ten seconds in the browser, converts it to 16 kHz mono
PCM, and plays it through Stack-Chan. Pressing the top control interrupts
speaker playback immediately so the device can record.
The CoreS3 audio codec is half-duplex, so starting a device recording stops any
current speaker playback. Device microphone and speaker mutes are independent.

Modern browsers expose direct microphone capture only in a secure context.
Because this local page is normally served over plain HTTP, the card also has a
`Choose / record audio` control; mobile browsers can hand that off to the
system recorder, and desktop browsers can select an existing audio file. Direct
in-page recording works when the control origin is explicitly trusted or
proxied through localhost.

Type `help` in the 115200-baud serial monitor for commands. Useful smoke tests:

```text
status
expression listening
look -30 45
look 30 45
neutral
rgb 0 16 24
mic off
speaker off
torque off
network off
```

Angles are written in degrees at the console. The BSP uses tenths of a degree
internally. `network off` shuts down the website and Wi-Fi radio without
erasing credentials; reboot Stack-Chan to restore them.
