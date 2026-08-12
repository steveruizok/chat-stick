#!/usr/bin/env python3
"""Capture every Send Me to Heaven screen in one tethered session."""

import argparse
import os

from screenshot import capture, decode, find_port, write_png


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("outdir")
    parser.add_argument("--port")
    args = parser.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    port = find_port(args.port)

    screens = ("home", "armed", "result", "highscore", "cable")
    for index, name in enumerate(screens, 1):
        header, payload = capture(port, 115200, f"screen {name}")
        width, height, rgb = decode(header, payload)
        path = os.path.join(args.outdir, f"{index:02d}-{name}.png")
        write_png(path, width, height, rgb)
        print(f"saved {path}")


if __name__ == "__main__":
    main()
