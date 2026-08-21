#!/usr/bin/env bash
# Build the current firmware and publish it to R2 for OTA distribution.
# Devices on an older version will pick this up on next boot.
#
# Releasing a new version:
#   1. ./publish-ota-release.sh [m5-stick|waveshare-v1|waveshare-v2]
#
# The two Waveshare panel variants are separate OTA lineages: waveshare-v1
# publishes under chat-stick/firmware/waveshare/ (the original SH8601 board's
# existing path, FIRMWARE_DEVICE "waveshare") and waveshare-v2 under
# chat-stick/firmware/waveshare-v2/ (FIRMWARE_DEVICE "waveshare-v2").
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

DEVICE="${FIRMWARE_DEVICE:-m5-stick}"
BUCKET="m5-stick-assets"

usage() {
  echo "Usage: $0 [m5-stick|waveshare-v1|waveshare-v2]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --device)
      shift
      [[ $# -gt 0 ]] || { usage; exit 1; }
      DEVICE="$1"
      ;;
    --device=*)
      DEVICE="${1#--device=}"
      ;;
    m5-stick|waveshare-v1|waveshare-v2)
      DEVICE="$1"
      ;;
    waveshare)
      echo "Error: 'waveshare' is ambiguous; use waveshare-v1 (SH8601 panel) or waveshare-v2 (CO5300 panel)" >&2
      exit 1
      ;;
    *)
      usage
      exit 1
      ;;
  esac
  shift
done

# Map the device name to its project directory, PlatformIO env, and the OTA
# device id the worker uses (FIRMWARE_DEVICE in Config.h / R2 key prefix).
PIO_ENV=""
OTA_DEVICE="$DEVICE"
case "$DEVICE" in
  waveshare-v1)
    FIRMWARE_DIR="devices/firmware/waveshare"
    PIO_ENV="waveshare-v1"
    OTA_DEVICE="waveshare"
    ;;
  waveshare-v2)
    FIRMWARE_DIR="devices/firmware/waveshare"
    PIO_ENV="waveshare-v2"
    OTA_DEVICE="waveshare-v2"
    ;;
  waveshare)
    echo "Error: 'waveshare' is ambiguous; use waveshare-v1 (SH8601 panel) or waveshare-v2 (CO5300 panel)" >&2
    exit 1
    ;;
  *)
    FIRMWARE_DIR="devices/firmware/$DEVICE"
    ;;
esac
CONFIG_H="$FIRMWARE_DIR/src/Config.h"
CREDENTIALS_H="$FIRMWARE_DIR/src/credentials.h"
PLATFORMIO_INI="$FIRMWARE_DIR/platformio.ini"
KEY_PREFIX="chat-stick/firmware/$OTA_DEVICE"

if [[ ! -d "$FIRMWARE_DIR" ]]; then
  echo "Error: unknown firmware device '$DEVICE'" >&2
  usage
  exit 1
fi

if [[ -z "$PIO_ENV" ]]; then
  PIO_ENV=$(
    sed -n 's/^\[env:\(.*\)\]$/\1/p' "$PLATFORMIO_INI" | head -1
  )
fi
if [[ -z "$PIO_ENV" ]]; then
  echo "Error: could not parse PlatformIO env from $PLATFORMIO_INI" >&2
  exit 1
fi

FIRMWARE_BIN="$FIRMWARE_DIR/.pio/build/$PIO_ENV/firmware.bin"

VERSION=$(grep -E "constexpr int FIRMWARE_VERSION" "$CONFIG_H" | grep -oE "[0-9]+" | head -1)
if [[ -z "$VERSION" ]]; then
  echo "Error: could not parse FIRMWARE_VERSION from $CONFIG_H" >&2
  exit 1
fi

CHECK_URL="${OTA_CHECK_URL:-}"
if [[ -z "$CHECK_URL" && -f "$CREDENTIALS_H" ]]; then
  PRODUCTION_HOST=$(
    grep -A1 -E "PRODUCTION_SERVER_ADDRESS" "$CREDENTIALS_H" \
      | grep -oE '"[^"]+"' \
      | head -1 \
      | tr -d '"' \
      || true
  )
  if [[ -n "$PRODUCTION_HOST" ]]; then
    CHECK_URL="https://$PRODUCTION_HOST/firmware/check?version=0&device=$OTA_DEVICE"
  fi
fi

if [[ -z "$CHECK_URL" ]]; then
  echo "Error: could not determine firmware check URL." >&2
  echo "Set OTA_CHECK_URL or create $CREDENTIALS_H with PRODUCTION_SERVER_ADDRESS." >&2
  exit 1
fi

DEVICE_TOKEN="${DEVICE_AUTH_TOKEN:-}"
if [[ -z "$DEVICE_TOKEN" && -f "$CREDENTIALS_H" ]]; then
  DEVICE_TOKEN=$(
    grep -E "DEVICE_AUTH_TOKEN" "$CREDENTIALS_H" \
      | grep -oE '"[^"]*"' \
      | head -1 \
      | tr -d '"' \
      || true
  )
fi

echo "[1/3] Checking latest published firmware..."
if [[ -n "$DEVICE_TOKEN" ]]; then
  CHECK_RESPONSE=$(curl -fsS -H "X-Device-Token: $DEVICE_TOKEN" "$CHECK_URL")
else
  CHECK_RESPONSE=$(curl -fsS "$CHECK_URL")
fi

LATEST_VERSION=$(
  printf "%s" "$CHECK_RESPONSE" \
    | grep -oE '"latest_version"[[:space:]]*:[[:space:]]*[0-9]+' \
    | grep -oE '[0-9]+' \
    | head -1 \
    || true
)

if [[ -z "$LATEST_VERSION" ]]; then
  echo "Error: could not parse latest_version from firmware check response." >&2
  exit 1
fi

echo "Latest published firmware: v$LATEST_VERSION"

if (( VERSION <= LATEST_VERSION )); then
  VERSION=$((LATEST_VERSION + 1))
  perl -0pi -e "s/(constexpr int FIRMWARE_VERSION = )\\d+;/\${1}$VERSION;/" "$CONFIG_H"
  echo "Bumped FIRMWARE_VERSION to v$VERSION"
else
  echo "Local FIRMWARE_VERSION v$VERSION is already ahead"
fi

KEY="$KEY_PREFIX/firmware-v$VERSION.bin"

echo "Publishing $OTA_DEVICE firmware v$VERSION (env $PIO_ENV) → $BUCKET/$KEY"
echo

echo "[2/3] Building firmware..."
(cd "$FIRMWARE_DIR" && pio run -e "$PIO_ENV")

if [[ ! -f "$FIRMWARE_BIN" ]]; then
  echo "Error: $FIRMWARE_BIN not found after build" >&2
  exit 1
fi

SIZE=$(wc -c < "$FIRMWARE_BIN" | tr -d ' ')

echo
echo "[3/3] Uploading $SIZE bytes to R2..."
(cd server && npx wrangler r2 object put "$BUCKET/$KEY" --file="../$FIRMWARE_BIN" --remote)

echo
echo "Published $OTA_DEVICE firmware v$VERSION ($SIZE bytes)"
echo "Devices on older versions will install this on next boot."
