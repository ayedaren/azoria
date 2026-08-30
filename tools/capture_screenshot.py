#!/usr/bin/env python3
"""Capture an Azoria framebuffer over USB and save a PNG without extra deps."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import termios
import time
import tty
from pathlib import Path


HEADER = b"AZORIA_SCREENSHOT_BEGIN "
HEADER_RE = re.compile(rb"AZORIA_SCREENSHOT_BEGIN (\d+) (\d+) RGB565LE (\d+)\n")


def configure_serial(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def read_until(fd: int, marker: bytes, timeout: float) -> bytes:
    data = bytearray()
    deadline = time.monotonic() + timeout
    while marker not in data:
        if time.monotonic() >= deadline:
            raise TimeoutError("timed out waiting for screenshot header")
        chunk = os.read(fd, 4096)
        if chunk:
            data.extend(chunk)
    return bytes(data)


def read_exact(fd: int, size: int, timeout: float) -> bytes:
    data = bytearray()
    deadline = time.monotonic() + timeout
    while len(data) < size:
        if time.monotonic() >= deadline:
            raise TimeoutError(f"timed out reading framebuffer ({len(data)}/{size})")
        chunk = os.read(fd, min(16384, size - len(data)))
        if chunk:
            data.extend(chunk)
    return bytes(data)


def rgb565_to_ppm(raw: bytes, width: int, height: int, path: Path) -> None:
    rgb = bytearray(width * height * 3)
    output = 0
    for index in range(0, len(raw), 2):
        pixel = raw[index] | (raw[index + 1] << 8)
        rgb[output] = ((pixel >> 11) & 0x1F) * 255 // 31
        rgb[output + 1] = ((pixel >> 5) & 0x3F) * 255 // 63
        rgb[output + 2] = (pixel & 0x1F) * 255 // 31
        output += 3
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--buffer", type=int, choices=(0, 1), default=0)
    parser.add_argument("--output", type=Path, default=Path("azoria-screen.png"))
    args = parser.parse_args()

    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY)
    try:
        configure_serial(fd)
        os.write(fd, f"AZORIA_SCREENSHOT {args.buffer}\n".encode())
        pending = read_until(fd, HEADER, 15.0)
        match = HEADER_RE.search(pending)
        if not match:
            raise RuntimeError("invalid screenshot header")
        width, height, byte_count = map(int, match.groups())
        # The serial driver may return the header and the first part of the
        # binary payload in one read. Preserve those bytes instead of starting
        # a second read after them, otherwise the decoded image is shifted and
        # appears as false color bands.
        payload_start = match.end()
        raw = bytearray(pending[payload_start:])
        if len(raw) < byte_count:
            raw.extend(read_exact(fd, byte_count - len(raw), 30.0))
        raw = bytes(raw[:byte_count])
        ppm_path = args.output.with_suffix(".ppm")
        rgb565_to_ppm(raw, width, height, ppm_path)
        subprocess.run(["sips", "-s", "format", "png", str(ppm_path), "--out", str(args.output)],
                       check=True, stdout=subprocess.DEVNULL)
        ppm_path.unlink()
        print(args.output)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
