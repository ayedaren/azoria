from __future__ import annotations

import glob
from pathlib import Path
import subprocess
import threading
from typing import Any


class FlashingError(RuntimeError):
    pass


class FirmwareFlasher:
    REQUIRED_IMAGES = ("bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin")

    def __init__(self, image_directory: str | Path):
        self.image_directory = Path(image_directory)
        self._lock = threading.Lock()

    def _tool_paths(self) -> tuple[Path, Path]:
        platformio = Path.home() / ".platformio"
        return (
            platformio / "penv/bin/python",
            platformio / "packages/tool-esptoolpy/esptool.py",
        )

    def available(self) -> bool:
        python, esptool = self._tool_paths()
        return (
            python.is_file()
            and esptool.is_file()
            and all((self.image_directory / name).is_file() for name in self.REQUIRED_IMAGES)
        )

    def status(self) -> dict[str, Any]:
        return {"firmware_available": self.available(), "flashing": self._lock.locked()}

    def flash(self, device: str) -> dict[str, Any]:
        detected = set(glob.glob("/dev/cu.usbmodem*"))
        if device not in detected:
            raise FlashingError("目标不是当前检测到的 USB 小屏幕")
        if not self.available():
            raise FlashingError("本地安装包不包含可刷写固件")
        if not self._lock.acquire(blocking=False):
            raise FlashingError("另一项刷写任务正在进行")
        try:
            python, esptool = self._tool_paths()
            image = self.image_directory
            command = [
                str(python), str(esptool), "--chip", "esp32s3", "--port", device,
                "--baud", "921600", "--before", "default_reset", "--after",
                "hard_reset", "write_flash", "-z", "--flash_mode", "qio",
                "--flash_freq", "80m", "--flash_size", "16MB",
                "0x0", str(image / "bootloader.bin"),
                "0x8000", str(image / "partitions.bin"),
                "0xe000", str(image / "boot_app0.bin"),
                "0x10000", str(image / "firmware.bin"),
            ]
            completed = subprocess.run(
                command, capture_output=True, text=True, timeout=180, check=False
            )
            if completed.returncode != 0:
                detail = (completed.stderr or completed.stdout).strip().splitlines()
                raise FlashingError(detail[-1] if detail else "刷写失败")
            return {"ok": True, "device": device, "message": "固件刷写完成"}
        except subprocess.TimeoutExpired as exc:
            raise FlashingError("刷写超时，请重新插线后再试") from exc
        finally:
            self._lock.release()
