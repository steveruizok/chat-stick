#!/usr/bin/env python3
"""Capture an RGB565 framebuffer screenshot from a device debug console."""

import argparse
import base64
import struct
import time
import zlib

import serial
from serial.tools import list_ports

BEGIN = b"SCREENSHOT BEGIN"
END = b"SCREENSHOT END"


def find_port(requested=None):
    if requested:
        return requested
    candidates = [
        p.device
        for p in list_ports.comports()
        if "usbmodem" in p.device.lower() or "usbserial" in p.device.lower()
    ]
    if not candidates:
        raise SystemExit("no USB serial device found; pass --port")
    return sorted(candidates)[0]


def open_device(port, baud):
    device = serial.Serial()
    device.port = port
    device.baudrate = baud
    device.timeout = 0.3
    device.dtr = False
    device.rts = False
    device.open()
    return device


def capture(port, baud, command=None, settle=0.5, timeout=20.0):
    with open_device(port, baud) as device:
        time.sleep(0.2)
        device.reset_input_buffer()
        if command:
            device.write((command + "\n").encode())
            device.flush()
            time.sleep(settle)
            device.reset_input_buffer()

        device.write(b"screenshot\n")
        device.flush()
        header, payload, collecting, buffer = None, [], False, b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            buffer += device.read(4096)
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                line = line.strip()
                if line.startswith(BEGIN):
                    header, payload, collecting = line.decode(), [], True
                elif line.startswith(END) and collecting:
                    time.sleep(0.1)
                    return header, b"".join(payload)
                elif collecting and line:
                    payload.append(line)
    raise SystemExit("timed out waiting for screenshot data")


def decode(header, payload, strict=True):
    fields = dict(token.split("=", 1) for token in header.split() if "=" in token)
    width, height = int(fields["w"]), int(fields["h"])
    raw = base64.b64decode(payload + b"=" * (-len(payload) % 4))
    if len(raw) % 4:
        raw = raw[: len(raw) - len(raw) % 4]

    pixels = bytearray()
    for offset in range(0, len(raw), 4):
        run = raw[offset] | raw[offset + 1] << 8
        value = raw[offset + 2] | raw[offset + 3] << 8
        red = ((value >> 11) & 0x1F) * 255 // 31
        green = ((value >> 5) & 0x3F) * 255 // 63
        blue = (value & 0x1F) * 255 // 31
        pixels += bytes((red, green, blue)) * run

    expected = width * height * 3
    if strict and len(pixels) != expected:
        raise ValueError(f"truncated screenshot: {len(pixels)} of {expected} bytes")
    pixels += b"\x00" * max(0, expected - len(pixels))
    return width, height, bytes(pixels[:expected])


def write_png(path, width, height, rgb):
    rows = b"".join(
        b"\x00" + rgb[y * width * 3 : (y + 1) * width * 3]
        for y in range(height)
    )

    def chunk(tag, data):
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as output:
        output.write(png)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output")
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--command", help='run first, e.g. "screen armed"')
    parser.add_argument("--settle", type=float, default=0.5)
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()
    port = find_port(args.port)
    header, payload = capture(port, args.baud, args.command, args.settle, args.timeout)
    width, height, rgb = decode(header, payload)
    write_png(args.output, width, height, rgb)
    print(f"{args.output}: {width}x{height} from {port}")


if __name__ == "__main__":
    main()
