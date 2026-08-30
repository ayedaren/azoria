from __future__ import annotations

import os
from pathlib import Path
import glob
import select
import termios
import threading
import time
import tty
from typing import Any
from urllib.parse import parse_qs, urlencode


class ProvisioningError(RuntimeError):
    pass


class UsbProvisioner:
    def __init__(self, device: str, token: str, backend_port: int):
        self.device = device
        self.token = token
        self.backend_port = backend_port
        self._lock = threading.Lock()

    def _exchange(self, command: str, marker: str, timeout: float) -> str:
        with self._lock:
            return self._exchange_locked(command, marker, timeout)

    def device_status(self) -> dict[str, Any]:
        devices = sorted(
            set(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/tty.usbmodem*"))
        )
        return {
            "configured": self.device,
            "connected": Path(self.device).exists(),
            "devices": devices,
        }

    def _exchange_locked(self, command: str, marker: str, timeout: float) -> str:
        if not Path(self.device).exists():
            raise ProvisioningError(f"ESP32 USB device not found: {self.device}")
        try:
            descriptor = os.open(
                self.device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK
            )
        except OSError as exc:
            raise ProvisioningError(f"cannot open ESP32 USB device: {exc}") from exc

        try:
            tty.setraw(descriptor)
            termios.tcflush(descriptor, termios.TCIOFLUSH)
            os.write(descriptor, command.encode("utf-8"))
            deadline = time.monotonic() + timeout
            received = bytearray()
            while time.monotonic() < deadline:
                readable, _, _ = select.select([descriptor], [], [], 0.25)
                if not readable:
                    continue
                try:
                    chunk = os.read(descriptor, 4096)
                except BlockingIOError:
                    continue
                received.extend(chunk)
                text = received.decode("utf-8", errors="replace")
                if marker in text or "AZORIA_ERROR" in text:
                    return text
            raise ProvisioningError("ESP32 did not respond over USB")
        finally:
            os.close(descriptor)

    def scan_networks(self) -> list[dict[str, Any]]:
        response = self._exchange("AZORIA_SCAN\n", "AZORIA_NETWORKS_DONE", 18)
        if "AZORIA_ERROR" in response and "AZORIA_NETWORK " not in response:
            raise ProvisioningError("ESP32 Wi-Fi scan failed")
        networks: list[dict[str, Any]] = []
        for line in response.splitlines():
            if not line.startswith("AZORIA_NETWORK "):
                continue
            values = parse_qs(line.removeprefix("AZORIA_NETWORK "), keep_blank_values=True)
            try:
                ssid = values["ssid"][0]
                rssi = int(values["rssi"][0])
                secure = values["secure"][0] == "1"
            except (KeyError, ValueError, IndexError):
                continue
            networks.append({"ssid": ssid, "rssi": rssi, "secure": secure})
        return networks

    def configure(self, ssid: str, password: str) -> None:
        if not 1 <= len(ssid.encode("utf-8")) <= 32:
            raise ProvisioningError("Wi-Fi name must contain 1–32 bytes")
        if len(password.encode("utf-8")) > 63:
            raise ProvisioningError("Wi-Fi password is too long")
        payload = urlencode(
            {
                "ssid": ssid,
                "pass": password,
                "host": "",
                "port": self.backend_port,
                "token": self.token,
            }
        )
        response = self._exchange(
            f"AZORIA_CONFIG {payload}\n", "AZORIA_OK", 28
        )
        if "AZORIA_OK" not in response:
            if "Wi-Fi connection failed" in response:
                raise ProvisioningError("Wi-Fi 连接失败，请检查密码后重试")
            raise ProvisioningError("ESP32 拒绝了这份配置，请重试")
