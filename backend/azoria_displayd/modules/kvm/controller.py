from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import re
import shutil
import subprocess
import threading
import time
from typing import Any

from .config import Config
from .lg_usb import LgUsbController, LgUsbError


class DdcError(RuntimeError):
    pass


STANDARD_INPUTS = {"dp1": 15, "dp2": 16, "hdmi1": 17, "hdmi2": 18, "usbc": 27}
LG_ALT_INPUTS = {"dp1": 208, "hdmi1": 144, "hdmi2": 145, "usbc": 210}
STANDARD_INPUT_NAMES = {value: name for name, value in STANDARD_INPUTS.items()}
VALUE_CONTROLS = {"brightness": "luminance", "contrast": "contrast", "volume": "volume"}
HID_VCP_CONTROLS = {"brightness": 0x10, "contrast": 0x12, "volume": 0x62}
CONFIRM_DELAY_SECONDS = 0.15
CONFIRM_ATTEMPTS = 3
LG_USB_RETRY_SECONDS = 5.0


@dataclass
class CachedStatus:
    value: dict[str, Any]
    timestamp: float


class MacDisplayPowerController:
    """Sleep or wake macOS displays when a monitor ignores MCCS power writes."""

    def __init__(self, timeout_seconds: float):
        self.timeout_seconds = timeout_seconds

    def set_power(self, power_on: bool) -> None:
        command = (
            ["/usr/bin/caffeinate", "-u", "-t", "1"]
            if power_on
            else ["/usr/bin/pmset", "displaysleepnow"]
        )
        try:
            result = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=max(self.timeout_seconds, 2.0),
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise DdcError(f"macOS display power command failed: {exc}") from exc
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise DdcError(detail or "macOS display power command failed")


class M1DdcController:
    def __init__(
        self,
        config: Config,
        *,
        video_ddc_enabled: bool = True,
    ):
        self.config = config
        self._video_ddc_enabled = video_ddc_enabled
        if video_ddc_enabled:
            resolved = shutil.which(config.ddc_binary)
            if not resolved:
                raise DdcError(
                    f"DDC tool '{config.ddc_binary}' was not found; "
                    "install it with: brew install m1ddc"
                )
            self.binary = resolved
        else:
            self.binary = ""
        self._lock = threading.RLock()
        self._cache: CachedStatus | None = None
        self._lg_usb = (
            LgUsbController(
                config.lg_hid_binary,
                timeout_seconds=min(config.command_timeout_seconds, 1.0),
            )
            if config.lg_hid_binary
            else None
        )
        # VCP 0x60 is known to report DP1 on this 32UQ85R while USB-C is
        # physically active. The last accepted alternate-address target is
        # therefore the authoritative input state.
        self._last_usb_state: dict[str, Any] = {
            "input": "usbc",
            "power": True,
        }
        self._state_file = (
            Path(config.state_file).expanduser()
            if config.state_file
            else None
        )
        self._load_last_state()
        self._mac_power = MacDisplayPowerController(config.command_timeout_seconds)
        self._soft_power_state: bool | None = None
        self._lg_usb_retry_after = 0.0

    def _load_last_state(self) -> None:
        if not self._state_file:
            return
        try:
            payload = json.loads(self._state_file.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError, TypeError):
            return
        input_value = payload.get("input")
        power_value = payload.get("power")
        if input_value in LG_ALT_INPUTS:
            self._last_usb_state["input"] = input_value
        if isinstance(power_value, bool):
            self._last_usb_state["power"] = power_value

    def _persist_last_state(self) -> None:
        if not self._state_file:
            return
        temporary = self._state_file.with_suffix(
            self._state_file.suffix + ".tmp"
        )
        try:
            self._state_file.parent.mkdir(parents=True, exist_ok=True)
            temporary.write_text(
                json.dumps(self._last_usb_state, separators=(",", ":")),
                encoding="utf-8",
            )
            temporary.replace(self._state_file)
        except OSError:
            # Control delivery must never fail only because the optional
            # restart-state cache could not be written.
            pass

    def _lg_usb_can_attempt(self, *, ignore_backoff: bool = False) -> bool:
        return bool(
            self._lg_usb
            and self._lg_usb.available()
            and (
                ignore_backoff
                or time.monotonic() >= self._lg_usb_retry_after
            )
        )

    def _mark_lg_usb_failure(self) -> None:
        self._lg_usb_retry_after = time.monotonic() + LG_USB_RETRY_SECONDS

    def _mark_lg_usb_success(self) -> None:
        self._lg_usb_retry_after = 0.0

    def _base(self) -> list[str]:
        return [self.binary, "display", self.config.display]

    def _run(self, *arguments: str) -> str:
        if not self._video_ddc_enabled:
            raise DdcError(
                "LG USB control failed; video-cable DDC fallback is disabled"
            )
        command = [*self._base(), *arguments]
        with self._lock:
            try:
                result = subprocess.run(
                    command,
                    capture_output=True,
                    text=True,
                    timeout=self.config.command_timeout_seconds,
                    check=False,
                )
            except subprocess.TimeoutExpired as exc:
                raise DdcError("DDC command timed out") from exc
            if result.returncode != 0:
                detail = (result.stderr or result.stdout).strip()
                raise DdcError(detail or f"DDC command failed with exit code {result.returncode}")
            return result.stdout.strip()

    @staticmethod
    def _parse_value(output: str) -> int:
        values = re.findall(r"-?\d+", output)
        if not values:
            raise DdcError(f"could not parse DDC value from: {output!r}")
        return int(values[-1])

    def get_value(self, control: str) -> int:
        if self._lg_usb_can_attempt():
            try:
                current, _ = self._lg_usb.get_vcp(HID_VCP_CONTROLS[control])
            except LgUsbError:
                self._mark_lg_usb_failure()
            else:
                self._mark_lg_usb_success()
                return current
        return self._parse_value(self._run("get", VALUE_CONTROLS[control]))

    def get_mute(self) -> bool:
        if self._lg_usb_can_attempt():
            try:
                value, _ = self._lg_usb.get_vcp(0x8D)
            except LgUsbError:
                self._mark_lg_usb_failure()
            else:
                self._mark_lg_usb_success()
                if value not in {1, 2}:
                    raise DdcError(f"unexpected LG HID mute value: {value}")
                return value == 1
        value = self._parse_value(self._run("get", "mute"))
        if value not in {1, 2}:
            raise DdcError(f"unexpected DDC mute value: {value}")
        return value == 1

    def get_input(self) -> str:
        if self._lg_usb_can_attempt():
            try:
                value = self._lg_usb.get_input()
            except LgUsbError:
                self._mark_lg_usb_failure()
            else:
                self._mark_lg_usb_success()
                return value
        value = self._parse_value(self._run("get", "input"))
        try:
            return STANDARD_INPUT_NAMES[value]
        except KeyError as exc:
            raise DdcError(f"unknown DDC input value: {value}") from exc

    def get_power(self) -> bool:
        if self._lg_usb_can_attempt():
            try:
                value = self._lg_usb.get_power()
            except LgUsbError:
                self._mark_lg_usb_failure()
            else:
                self._mark_lg_usb_success()
                return value
        # MCCS VCP D6: 1 is on, while 2 and the deeper sleep values are off.
        value = self._parse_value(self._run("get", "standby"))
        if value < 1 or value > 5:
            raise DdcError(f"unexpected DDC power value: {value}")
        return value == 1

    def status(self, force: bool = False) -> dict[str, Any]:
        now = time.monotonic()
        with self._lock:
            if (
                not force
                and self._cache
                and now - self._cache.timestamp < self.config.status_cache_seconds
            ):
                return dict(self._cache.value)

            values: dict[str, Any] = {
                "ok": True,
                "backend": (
                    "lg-usb"
                    if not self._video_ddc_enabled
                    else "m1ddc+lg-hid" if self._lg_usb else "m1ddc"
                ),
                "display": self.config.display,
                "input_mode": self.config.input_mode,
                "capabilities": [
                    "brightness",
                    "contrast",
                    "volume",
                    "mute",
                    *([] if self.config.input_mode == "disabled" else ["input"]),
                    *([] if not self.config.power_control_enabled else ["power"]),
                ],
            }
            errors: dict[str, str] = {}
            for control in VALUE_CONTROLS:
                try:
                    values[control] = self.get_value(control)
                except DdcError as exc:
                    values[control] = None
                    errors[control] = str(exc)
            for control, reader in (
                ("mute", self.get_mute),
                (
                    "input",
                    self._lg_usb.get_input
                    if self._lg_usb_can_attempt()
                    else self.get_input,
                ),
                (
                    "power",
                    self._lg_usb.get_power
                    if self._lg_usb_can_attempt()
                    else self.get_power,
                ),
            ):
                if control == "input" and self.config.input_mode in {
                    "alt",
                    "alt-noverify",
                    "disabled",
                }:
                    # Alternate 0x50/F4 addressing has no trustworthy 0x60
                    # readback. Preserve the latest accepted source instead of
                    # making the UI jump to the monitor's false DP1 report.
                    values[control] = self._last_usb_state.get("input")
                    continue
                if control == "power" and self._soft_power_state is not None:
                    values[control] = self._soft_power_state
                    continue
                if control == "power" and self.config.power_mode == "macos":
                    values[control] = self._last_usb_state.get("power", True)
                    continue
                try:
                    values[control] = reader()
                except (DdcError, LgUsbError) as exc:
                    if isinstance(exc, LgUsbError):
                        self._mark_lg_usb_failure()
                    values[control] = self._last_usb_state.get(control)
                    errors[control] = str(exc)
                else:
                    if getattr(reader, "__self__", None) is self._lg_usb:
                        self._mark_lg_usb_success()
            if errors:
                values["read_errors"] = errors
            self._cache = CachedStatus(dict(values), time.monotonic())
            return values

    def invalidate_cache(self) -> None:
        with self._lock:
            self._cache = None

    def _confirm_numeric(self, control: str, requested: int) -> int:
        last_value: int | None = None
        last_error: DdcError | None = None
        time.sleep(CONFIRM_DELAY_SECONDS)
        for attempt in range(CONFIRM_ATTEMPTS):
            try:
                actual = self.get_value(control)
            except DdcError as exc:
                last_error = exc
            else:
                last_value = actual
                last_error = None
                if abs(actual - requested) <= 1:
                    return actual
            if attempt + 1 < CONFIRM_ATTEMPTS:
                time.sleep(CONFIRM_DELAY_SECONDS)
        detail = f"; last read error: {last_error}" if last_error else ""
        raise DdcError(
            f"{control} confirmation failed "
            f"(requested {requested}, last value {last_value}){detail}"
        )

    def _confirm_exact(self, control: str, requested: Any) -> Any:
        reader = self.get_mute if control == "mute" else self.get_input
        last_value: Any = None
        last_error: DdcError | None = None
        time.sleep(CONFIRM_DELAY_SECONDS)
        for attempt in range(CONFIRM_ATTEMPTS):
            try:
                last_value = reader()
                last_error = None
            except DdcError as exc:
                last_error = exc
            else:
                if last_value == requested:
                    return last_value
            if attempt + 1 < CONFIRM_ATTEMPTS:
                time.sleep(CONFIRM_DELAY_SECONDS)
        detail = f"; last read error: {last_error}" if last_error else ""
        raise DdcError(
            f"{control} confirmation failed "
            f"(requested {requested!r}, last value {last_value!r}){detail}"
        )

    def _confirm_monitor_state(
        self, control: str, requested: Any, reader: Any
    ) -> Any:
        last_value: Any = None
        last_error: Exception | None = None
        time.sleep(CONFIRM_DELAY_SECONDS)
        for attempt in range(CONFIRM_ATTEMPTS):
            try:
                last_value = reader()
                last_error = None
            except (DdcError, LgUsbError) as exc:
                last_error = exc
            else:
                if last_value == requested:
                    return last_value
            if attempt + 1 < CONFIRM_ATTEMPTS:
                time.sleep(CONFIRM_DELAY_SECONDS)
        detail = f"; last read error: {last_error}" if last_error else ""
        raise DdcError(
            f"{control} verification failed "
            f"(requested {requested!r}, last value {last_value!r}){detail}"
        )

    def set_control(
        self, control: str, value: Any, final: bool = True
    ) -> dict[str, Any]:
        if not isinstance(final, bool):
            raise ValueError("final must be true or false")
        with self._lock:
            confirmed_value: Any | None = None
            transport: str | None = None
            if control in VALUE_CONTROLS:
                if isinstance(value, bool) or not isinstance(value, (int, float)):
                    raise ValueError(f"{control} value must be numeric")
                normalized = max(0, min(100, round(value)))
                if self._lg_usb_can_attempt(ignore_backoff=True):
                    try:
                        self._lg_usb.set_vcp(
                            HID_VCP_CONTROLS[control], normalized
                        )
                    except LgUsbError:
                        self._mark_lg_usb_failure()
                        self._run(
                            "set", VALUE_CONTROLS[control], str(normalized)
                        )
                    else:
                        self._mark_lg_usb_success()
                else:
                    self._run("set", VALUE_CONTROLS[control], str(normalized))
                requested: Any = normalized
            elif control == "mute":
                if not isinstance(value, bool):
                    raise ValueError("mute value must be true or false")
                if self._lg_usb_can_attempt(ignore_backoff=True):
                    try:
                        self._lg_usb.set_vcp(0x8D, 1 if value else 2)
                    except LgUsbError:
                        self._mark_lg_usb_failure()
                        self._run("set", "mute", "on" if value else "off")
                    else:
                        self._mark_lg_usb_success()
                else:
                    self._run("set", "mute", "on" if value else "off")
                requested = value
            elif control == "input":
                if not isinstance(value, str):
                    raise ValueError("input value must be a name")
                if self.config.input_mode == "disabled":
                    raise DdcError(
                        "input verification failed: LG 32UQ85R does not execute "
                        "software input switching through its DDC transports"
                    )
                name = value.lower()
                alternate_input = self.config.input_mode in {
                    "alt",
                    "alt-noverify",
                }
                inputs = LG_ALT_INPUTS if alternate_input else STANDARD_INPUTS
                if name not in inputs:
                    raise ValueError("input must be one of: dp1, hdmi1, hdmi2, usbc")
                transport = "m1ddc"
                usb_wrote = False
                if self.config.input_mode == "alt-noverify":
                    if self._lg_usb:
                        if not self._lg_usb.available():
                            raise DdcError(
                                "LG HID helper is unavailable; input command "
                                "was not sent"
                            )
                        try:
                            self._lg_usb.set_input(name, alternate=True)
                        except LgUsbError as usb_error:
                            self._mark_lg_usb_failure()
                            raise DdcError(
                                "LG HID input command was not sent: "
                                f"{usb_error}. Set the monitor OSD General > "
                                "USB Selection for this input to USB-C."
                            ) from usb_error
                        else:
                            self._mark_lg_usb_success()
                            transport = "lg-hid"
                            usb_wrote = True
                    else:
                        self._run("set", "input-alt", str(inputs[name]))
                elif self._lg_usb_can_attempt():
                    try:
                        self._lg_usb.set_input(
                            name,
                            # 32UQ85R uses LG's proprietary DDC source address
                            # 0x50 with VCP 0xF4 for input switching. A normal
                            # 0x51/0x60 write is ACKed but silently ignored.
                            alternate=alternate_input,
                        )
                    except LgUsbError as usb_error:
                        self._mark_lg_usb_failure()
                        command = "input-alt" if alternate_input else "input"
                        try:
                            self._run("set", command, str(inputs[name]))
                        except DdcError as ddc_error:
                            raise DdcError(
                                f"LG USB failed: {usb_error}; DDC fallback failed: {ddc_error}"
                            ) from ddc_error
                    else:
                        self._mark_lg_usb_success()
                        transport = "lg-usb"
                        usb_wrote = True
                else:
                    command = "input-alt" if alternate_input else "input"
                    self._run("set", command, str(inputs[name]))
                requested = name
                if self.config.input_mode == "alt-noverify":
                    # LG's DDC2AB manufacturer side-channel (source 0x50,
                    # VCP 0xF4) intentionally has no reliable readback. A
                    # second write or a standard-DDC "verification" can undo
                    # or duplicate a real source change, so acknowledge only
                    # delivery here and let the next independent status read
                    # update the selected input if it remains reachable.
                    self._last_usb_state["input"] = requested
                    self._persist_last_state()
                    self._cache = None
                    return {
                        "ok": True,
                        "control": control,
                        "requested": requested,
                        "value": requested,
                        "accepted": True,
                        "confirmed": False,
                        "transport": transport,
                        "verification": "write-only",
                    }
                if final:
                    reader = (
                        self._lg_usb.get_input
                        if self._lg_usb_can_attempt()
                        else self.get_input
                    )
                    try:
                        confirmed_value = self._confirm_monitor_state(
                            "input", requested, reader
                        )
                    except DdcError as first_error:
                        if not usb_wrote:
                            raise
                        command = (
                            "input-alt"
                            if alternate_input
                            else "input"
                        )
                        try:
                            self._run("set", command, str(inputs[name]))
                            transport = "m1ddc"
                            confirmed_value = self._confirm_monitor_state(
                                "input", requested, reader
                            )
                        except DdcError as fallback_error:
                            raise DdcError(
                                "input verification failed: LG 32UQ85R ignored "
                                f"both USB and video DDC writes; USB: {first_error}; "
                                f"video DDC: {fallback_error}"
                            ) from fallback_error
                    self._last_usb_state["input"] = confirmed_value
                    self._persist_last_state()
            elif control == "power":
                if not isinstance(value, bool):
                    raise ValueError("power value must be true or false")
                transport = "m1ddc"
                usb_wrote = False
                if self.config.power_mode == "lg-hid":
                    if not self._lg_usb_can_attempt(ignore_backoff=True):
                        raise DdcError(
                            "LG HID power control is unavailable; command was not sent"
                        )
                    try:
                        self._lg_usb.set_power(value)
                    except LgUsbError as exc:
                        self._mark_lg_usb_failure()
                        raise DdcError(f"LG HID power command failed: {exc}") from exc
                    self._mark_lg_usb_success()
                    requested = value
                    self._soft_power_state = value
                    self._last_usb_state["power"] = value
                    self._persist_last_state()
                    self._cache = None
                    return {
                        "ok": True,
                        "control": control,
                        "requested": requested,
                        "value": requested,
                        "accepted": True,
                        "confirmed": False,
                        "transport": "lg-hid",
                        "verification": "write-only",
                    }
                if self.config.power_mode == "macos":
                    self._mac_power.set_power(value)
                    requested = value
                    confirmed_value = value
                    transport = "macos-display-sleep"
                    self._soft_power_state = value
                    self._last_usb_state["power"] = value
                    self._persist_last_state()
                    self._cache = None
                    return {
                        "ok": True,
                        "control": control,
                        "requested": requested,
                        "value": confirmed_value,
                        "accepted": True,
                        "confirmed": True,
                        "transport": transport,
                    }
                # A previous macOS sleep must be explicitly woken. This model
                # keeps reporting D6=1 while asleep, so DDC readback alone
                # cannot distinguish that state.
                force_macos_wake = value and self._soft_power_state is False
                if not force_macos_wake:
                    if self._lg_usb_can_attempt():
                        try:
                            self._lg_usb.set_power(value)
                        except LgUsbError as usb_error:
                            self._mark_lg_usb_failure()
                            try:
                                self._run(
                                    "set", "standby", "on" if value else "off"
                                )
                            except DdcError as ddc_error:
                                raise DdcError(
                                    f"LG USB failed: {usb_error}; "
                                    f"DDC fallback failed: {ddc_error}"
                                ) from ddc_error
                        else:
                            self._mark_lg_usb_success()
                            transport = "lg-usb"
                            usb_wrote = True
                    else:
                        self._run("set", "standby", "on" if value else "off")
                requested = value
                if final:
                    hardware_confirmed = False
                    if not force_macos_wake:
                        reader = (
                            self._lg_usb.get_power
                            if self._lg_usb_can_attempt()
                            else self.get_power
                        )
                        try:
                            confirmed_value = self._confirm_monitor_state(
                                "power", requested, reader
                            )
                            hardware_confirmed = True
                        except DdcError:
                            if usb_wrote:
                                try:
                                    self._run(
                                        "set",
                                        "standby",
                                        "on" if value else "off",
                                    )
                                    transport = "m1ddc"
                                    confirmed_value = self._confirm_monitor_state(
                                        "power", requested, reader
                                    )
                                    hardware_confirmed = True
                                except DdcError:
                                    pass
                    if not hardware_confirmed and self.config.power_mode == "ddc":
                        raise DdcError(
                            "power verification failed: the monitor ignored "
                            "the MCCS D6 write"
                        )
                    if not hardware_confirmed:
                        self._mac_power.set_power(value)
                        transport = "macos-display-sleep"
                        confirmed_value = value
                        self._soft_power_state = value
                    else:
                        self._soft_power_state = None
                    self._last_usb_state["power"] = confirmed_value
                    self._persist_last_state()
            else:
                raise ValueError("unsupported control")

            # Invalidate immediately: confirmation can fail or time out and must
            # never leave a previously confirmed status entry in place.
            self._cache = None
            if not final:
                return {
                    "ok": True,
                    "control": control,
                    "requested": requested,
                    "value": requested,
                    "accepted": True,
                    "confirmed": False,
                    **(
                        {"transport": transport}
                        if control in {"input", "power"}
                        else {}
                    ),
                }

            if control in VALUE_CONTROLS:
                confirmed_value = self._confirm_numeric(control, requested)
            elif control in {"input", "power"}:
                return {
                    "ok": True,
                    "control": control,
                    "requested": requested,
                    "value": confirmed_value,
                    "accepted": True,
                    "confirmed": True,
                    "transport": transport,
                }
            else:
                confirmed_value = self._confirm_exact(control, requested)
            return {
                "ok": True,
                "control": control,
                "requested": requested,
                "value": confirmed_value,
                "accepted": True,
                "confirmed": True,
            }


class LgUsbOnlyController(M1DdcController):
    """LG vendor USB control with no video-cable DDC fallback."""

    def __init__(self, config: Config):
        super().__init__(config, video_ddc_enabled=False)
        if not self._lg_usb or not self._lg_usb.available():
            raise DdcError(
                "LG USB controller is unavailable: "
                f"{config.lg_hid_binary or 'lg_hid_binary is not configured'}"
            )


class MockDdcController:
    """In-memory/file-backed controller used by tests and UI development."""

    def __init__(self, state_file: str | Path | None = None):
        self.path = Path(state_file) if state_file else None
        self._lock = threading.Lock()
        self._state: dict[str, Any] = {
            "brightness": 55,
            "contrast": 70,
            "volume": 20,
            "mute": False,
            "input": "usbc",
            "power": True,
        }
        if self.path and self.path.exists():
            self._state.update(json.loads(self.path.read_text(encoding="utf-8")))

    def _save(self) -> None:
        if self.path:
            self.path.write_text(json.dumps(self._state, indent=2), encoding="utf-8")

    def status(self, force: bool = False) -> dict[str, Any]:
        del force
        with self._lock:
            return {
                "ok": True,
                "backend": "mock",
                "display": "mock-1",
                "input_mode": "alt",
                "capabilities": [
                    "brightness",
                    "contrast",
                    "volume",
                    "mute",
                    "input",
                    "power",
                ],
                **self._state,
            }

    def set_control(
        self, control: str, value: Any, final: bool = True
    ) -> dict[str, Any]:
        if not isinstance(final, bool):
            raise ValueError("final must be true or false")
        with self._lock:
            if control in VALUE_CONTROLS:
                if isinstance(value, bool) or not isinstance(value, (int, float)):
                    raise ValueError(f"{control} value must be numeric")
                value = max(0, min(100, round(value)))
            elif control == "mute":
                if not isinstance(value, bool):
                    raise ValueError("mute value must be true or false")
            elif control == "input":
                if value not in STANDARD_INPUTS:
                    raise ValueError("input must be one of: dp1, dp2, hdmi1, hdmi2, usbc")
            elif control == "power":
                if not isinstance(value, bool):
                    raise ValueError("power value must be true or false")
            else:
                raise ValueError("unsupported control")
            self._state[control] = value
            self._save()
            return {
                "ok": True,
                "control": control,
                "requested": value,
                "value": value,
                "accepted": True,
                "confirmed": final,
            }
