#!/usr/bin/env python3
"""Interactive serial console for manually driving debug-enabled firmware."""

import argparse
import sys
import threading
import time

from screenshot import find_port, open_device


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    port = find_port(args.port)
    device = open_device(port, args.baud)
    stop = threading.Event()

    def read_serial():
        while not stop.is_set():
            data = device.read(4096)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()

    reader = threading.Thread(target=read_serial, daemon=True)
    reader.start()
    print(f"connected to {port}")
    print("commands: state, screen home|armed|result|highscore|cable, tap X Y, btn a click")
    print("type quit to close")
    try:
        while True:
            command = input("device> ").strip()
            if command in {"quit", "exit"}:
                break
            if command:
                device.write((command + "\n").encode())
                device.flush()
                time.sleep(0.05)
    except (EOFError, KeyboardInterrupt):
        pass
    finally:
        stop.set()
        device.close()


if __name__ == "__main__":
    main()
