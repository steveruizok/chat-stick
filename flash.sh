#!/usr/bin/env bash
# Build and flash firmware to a USB-connected device.
#
# Usage:
#   ./flash.sh [m5-stick|waveshare|stack-chan] [--monitor] [--port /dev/cu.usbmodem101]
#   PORT=/dev/cu.usbmodem101 ./flash.sh waveshare
# Pass --monitor to also open the serial monitor after flashing.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

DEVICE="${FIRMWARE_DEVICE:-m5-stick}"
PORT="${FIRMWARE_PORT:-${PORT:-}}"
MONITOR=false

usage() {
  echo "Usage: $0 [m5-stick|waveshare|stack-chan] [--monitor] [--port /dev/cu.usbmodem101]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --monitor)
      MONITOR=true
      ;;
    --device)
      shift
      [[ $# -gt 0 ]] || { usage; exit 1; }
      DEVICE="$1"
      ;;
    --device=*)
      DEVICE="${1#--device=}"
      ;;
    --port)
      shift
      [[ $# -gt 0 ]] || { usage; exit 1; }
      PORT="$1"
      ;;
    --port=*)
      PORT="${1#--port=}"
      ;;
    m5-stick|waveshare|stack-chan)
      DEVICE="$1"
      ;;
    *)
      usage
      exit 1
      ;;
  esac
  shift
done

FIRMWARE_DIR="devices/firmware/$DEVICE"
if [[ ! -d "$FIRMWARE_DIR" ]]; then
  echo "Error: unknown firmware device '$DEVICE'" >&2
  usage
  exit 1
fi

if [[ -z "$PORT" ]]; then
  shopt -s nullglob
  PORT_CANDIDATES=(/dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial*)
  shopt -u nullglob

  if [[ ${#PORT_CANDIDATES[@]} -eq 0 ]]; then
    echo "Error: no ESP serial port found." >&2
    echo "Connect the device over a data-capable USB cable or put it in bootloader mode, then retry." >&2
    echo "Visible ports:" >&2
    pio device list >&2 || true
    exit 1
  fi

  if [[ ${#PORT_CANDIDATES[@]} -gt 1 ]]; then
    echo "Error: multiple candidate ESP serial ports found:" >&2
    printf '  %s\n' "${PORT_CANDIDATES[@]}" >&2
    echo "Retry with --port /dev/cu.usbmodem... or PORT=/dev/cu.usbmodem..." >&2
    exit 1
  fi

  PORT="${PORT_CANDIDATES[0]}"
fi

echo "Using $PORT for $DEVICE"
(cd "$FIRMWARE_DIR" && pio run -t upload --upload-port "$PORT")

if $MONITOR; then
  (cd "$FIRMWARE_DIR" && pio device monitor --port "$PORT")
fi
