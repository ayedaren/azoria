from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import threading
from typing import Any


class LgUsbError(RuntimeError):
    pass


LG_ALT_INPUTS = {
    "dp1": 0xD0,
    "dp2": 0xD1,
    "hdmi1": 0x90,
    "hdmi2": 0x91,
    "usbc": 0xD2,
}
STANDARD_INPUTS = {
    "dp1": 0x0F,
    "dp2": 0x10,
    "hdmi1": 0x11,
    "hdmi2": 0x12,
    "usbc": 0x1B,
}

VCP_INPUT = 0x60
VCP_POWER_MODE = 0xD6


class LgUsbController:
    """LG Monitor Controls HID to DDC bridge.

    LG 32UQ85R exposes USB VID:PID 043e:9a39 independently of the active
    video input. The native helper talks to that vendor HID interface using
    fixed, allow-listed commands; it is not a serial port and provides no
    firmware-update operations.
    """

    def __init__(self, binary: str, *, timeout_seconds: float = 1.5):
        expanded = Path(binary).expanduser()
        self.binary = (
            str(expanded)
            if expanded.is_file()
            else (shutil.which(binary) or str(expanded))
        )
        self.timeout_seconds = timeout_seconds
        self._lock = threading.Lock()

    def available(self) -> bool:
        path = Path(self.binary)
        return path.is_file() and path.stat().st_mode & 0o111 != 0

    def _run(self, *arguments: str) -> dict[str, Any]:
        if not self.available():
            raise LgUsbError(f"LG HID helper is unavailable: {self.binary}")
        try:
            with self._lock:
                result = subprocess.run(
                    [self.binary, *arguments],
                    capture_output=True,
                    text=True,
                    timeout=self.timeout_seconds,
                    check=False,
                )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise LgUsbError(f"LG HID command failed: {exc}") from exc

        output = (result.stdout or result.stderr).strip()
        try:
            payload = json.loads(output)
        except (json.JSONDecodeError, TypeError) as exc:
            raise LgUsbError(
                output or f"LG HID helper exited with code {result.returncode}"
            ) from exc
        if result.returncode != 0 or not payload.get("ok"):
            raise LgUsbError(str(payload.get("error") or output))
        return payload

    def set_vcp(self, opcode: int, value: int) -> None:
        if not 0 <= opcode <= 0xFF:
            raise ValueError("VCP opcode must fit in one byte")
        if not 0 <= value <= 0xFFFF:
            raise ValueError("VCP value must fit in two bytes")
        self._run("set", hex(opcode), str(value))

    def get_vcp(self, opcode: int) -> tuple[int, int]:
        if not 0 <= opcode <= 0xFF:
            raise ValueError("VCP opcode must fit in one byte")
        payload = self._run("get", hex(opcode))
        return int(payload["current"]), int(payload["maximum"])

    def set_input(self, name: str, *, alternate: bool = True) -> None:
        normalized = name.lower()
        values = LG_ALT_INPUTS if alternate else STANDARD_INPUTS
        if normalized not in values:
            raise ValueError(
                "input must be one of: dp1, dp2, hdmi1, hdmi2, usbc"
            )
        if alternate:
            self._run("input", normalized)
        else:
            self.set_vcp(VCP_INPUT, values[normalized])

    def set_power(self, power_on: bool) -> None:
        if not isinstance(power_on, bool):
            raise ValueError("power value must be true or false")
        self.set_vcp(VCP_POWER_MODE, 1 if power_on else 2)

    def get_input(self) -> str:
        current, _ = self.get_vcp(VCP_INPUT)
        names = {value: name for name, value in STANDARD_INPUTS.items()}
        try:
            return names[current]
        except KeyError as exc:
            raise LgUsbError(f"unknown LG HID input value: {current}") from exc

    def get_power(self) -> bool:
        current, _ = self.get_vcp(VCP_POWER_MODE)
        if not 1 <= current <= 5:
            raise LgUsbError(f"unexpected LG HID power value: {current}")
        return current == 1
