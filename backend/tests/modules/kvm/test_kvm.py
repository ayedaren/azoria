from __future__ import annotations

import json
from pathlib import Path
import sys
import socket
import subprocess
import threading
import time
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import ProxyHandler, Request, build_opener

sys.path.insert(0, str(Path(__file__).parents[3]))

from azoria_displayd.modules.kvm.api import (
    LatestControlGate,
    create_server,
)
from azoria_displayd.modules.kvm.config import Config
from azoria_displayd.modules.kvm.controller import (
    DdcError,
    LgUsbOnlyController,
    M1DdcController,
    MockDdcController,
)
from azoria_displayd.modules.kvm.discovery import UdpDiscoveryResponder
from azoria_displayd.modules.kvm.lg_usb import (
    LgUsbController,
    LgUsbError,
)


TOKEN = "test-token-with-more-than-20-characters"


class ConfigSecurityTests(unittest.TestCase):
    def test_defaults_to_loopback_only(self) -> None:
        config = Config.from_dict({"token": TOKEN})
        self.assertEqual(config.bind, "127.0.0.1")
        self.assertTrue(config.is_loopback_only)
        self.assertTrue(config.client_allowed("127.0.0.1"))
        self.assertFalse(config.client_allowed("192.168.1.20"))

    def test_rejects_public_or_all_interface_bind_addresses(self) -> None:
        for address in ("0.0.0.0", "8.8.8.8", "example.com"):
            with self.subTest(address=address):
                with self.assertRaisesRegex(ValueError, "bind"):
                    Config.from_dict({"token": TOKEN, "bind": address})

    def test_rejects_public_allowed_networks(self) -> None:
        with self.assertRaisesRegex(ValueError, "allowed_networks"):
            Config.from_dict(
                {"token": TOKEN, "allowed_networks": ["0.0.0.0/0"]}
            )

    def test_accepts_explicit_private_lan_configuration(self) -> None:
        config = Config.from_dict(
            {
                "token": TOKEN,
                "bind": "192.168.5.10",
                "allowed_networks": ["192.168.5.0/24"],
            }
        )
        self.assertFalse(config.is_loopback_only)
        self.assertTrue(config.client_allowed("192.168.5.20"))
        self.assertFalse(config.client_allowed("192.168.6.20"))


class LgUsbControllerTests(unittest.TestCase):
    def test_input_and_power_use_allow_listed_native_helper_commands(self) -> None:
        controller = LgUsbController(sys.executable)
        replies = [
            subprocess.CompletedProcess([], 0, '{"ok":true}', ""),
            subprocess.CompletedProcess([], 0, '{"ok":true}', ""),
        ]
        with patch(
            "azoria_displayd.modules.kvm.lg_usb.subprocess.run", side_effect=replies
        ) as run:
            controller.set_input("hdmi2")
            controller.set_power(False)
        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [
                [sys.executable, "input", "hdmi2"],
                [sys.executable, "set", "0xd6", "2"],
            ],
        )

    def test_surfaces_helper_error_and_malformed_output(self) -> None:
        controller = LgUsbController(sys.executable)
        with patch(
            "azoria_displayd.modules.kvm.lg_usb.subprocess.run",
            return_value=subprocess.CompletedProcess(
                [], 3, '{"ok":false,"error":"HID unavailable"}', ""
            ),
        ):
            with self.assertRaisesRegex(LgUsbError, "HID unavailable"):
                controller.set_power(True)
        with patch(
            "azoria_displayd.modules.kvm.lg_usb.subprocess.run",
            return_value=subprocess.CompletedProcess([], 1, "not json", ""),
        ):
            with self.assertRaisesRegex(LgUsbError, "not json"):
                controller.set_power(True)

    def test_parses_get_vcp_reply(self) -> None:
        controller = LgUsbController(sys.executable)
        with patch(
            "azoria_displayd.modules.kvm.lg_usb.subprocess.run",
            return_value=subprocess.CompletedProcess(
                [],
                0,
                '{"ok":true,"opcode":96,"maximum":255,"current":27}',
                "",
            ),
        ):
            self.assertEqual(controller.get_vcp(0x60), (0x1B, 0xFF))
            self.assertEqual(controller.get_input(), "usbc")


class M1DdcControllerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.config = Config.from_dict(
            {
                "token": TOKEN,
                "ddc_binary": sys.executable,
                "status_cache_seconds": 30,
            }
        )
        self.controller = M1DdcController(self.config)
        sleep = patch("azoria_displayd.modules.kvm.controller.time.sleep")
        sleep.start()
        self.addCleanup(sleep.stop)

    def test_status_preserves_last_alt_input_instead_of_false_readback(self) -> None:
        def fake_run(command, **_kwargs):
            outputs = {
                "luminance": "66\n",
                "contrast": "75\n",
                "volume": "31\n",
                "mute": "2\n",
                "input": "15\n",
                "standby": "1\n",
            }
            return subprocess.CompletedProcess(command, 0, outputs[command[-1]], "")

        with patch("azoria_displayd.modules.kvm.controller.subprocess.run", side_effect=fake_run):
            status = self.controller.status(force=True)
        self.assertEqual(status["backend"], "m1ddc")
        self.assertEqual(status["brightness"], 66)
        self.assertEqual(status["contrast"], 75)
        self.assertEqual(status["volume"], 31)
        self.assertFalse(status["mute"])
        self.assertEqual(status["input"], "usbc")
        self.assertTrue(status["power"])

    def test_mute_maps_mccs_values_one_and_two(self) -> None:
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            side_effect=[
                subprocess.CompletedProcess([], 0, "1\n", ""),
                subprocess.CompletedProcess([], 0, "2\n", ""),
            ],
        ):
            self.assertTrue(self.controller.get_mute())
            self.assertFalse(self.controller.get_mute())

    def test_lg_input_write_must_match_standard_readback(self) -> None:
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            side_effect=[
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "18\n", ""),
            ],
        ) as run:
            result = self.controller.set_control("input", "hdmi2")
        self.assertEqual(result["value"], "hdmi2")
        self.assertEqual(
            run.call_args_list[0].args[0][1:],
            ["display", "1", "set", "input-alt", "145"],
        )
        self.assertEqual(
            run.call_args_list[1].args[0][1:],
            ["display", "1", "get", "input"],
        )
        self.assertTrue(result["accepted"])
        self.assertTrue(result["confirmed"])

    def test_lg_alt_noverify_sends_exactly_one_write(self) -> None:
        config = Config.from_dict(
            {
                "token": TOKEN,
                "ddc_binary": sys.executable,
                "input_mode": "alt-noverify",
            }
        )
        controller = M1DdcController(config)
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            return_value=subprocess.CompletedProcess([], 0, "", ""),
        ) as run:
            result = controller.set_control("input", "usbc")
        run.assert_called_once()
        self.assertEqual(
            run.call_args.args[0][1:],
            ["display", "1", "set", "input-alt", "210"],
        )
        self.assertTrue(result["accepted"])
        self.assertFalse(result["confirmed"])
        self.assertEqual(result["verification"], "write-only")

    def test_lg_alt_noverify_prefers_independent_hid(self) -> None:
        class FakeLgUsb:
            def __init__(self) -> None:
                self.calls: list[tuple[str, bool]] = []

            def available(self) -> bool:
                return True

            def set_input(self, value: str, *, alternate: bool) -> None:
                self.calls.append((value, alternate))

        config = Config.from_dict(
            {
                "token": TOKEN,
                "ddc_binary": sys.executable,
                "input_mode": "alt-noverify",
                "lg_hid_binary": sys.executable,
            }
        )
        controller = M1DdcController(config)
        usb = FakeLgUsb()
        controller._lg_usb = usb
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            return_value=subprocess.CompletedProcess([], 0, "", ""),
        ) as run:
            result = controller.set_control("input", "usbc")
        run.assert_not_called()
        self.assertEqual(usb.calls, [("usbc", True)])
        self.assertEqual(result["transport"], "lg-hid")

    def test_lg_alt_noverify_does_not_fake_success_when_hid_disconnects(self) -> None:
        class DisconnectedLgUsb:
            def available(self) -> bool:
                return True

            def set_input(self, value: str, *, alternate: bool) -> None:
                del value, alternate
                raise LgUsbError("LG Monitor Controls HID is not connected")

        config = Config.from_dict(
            {
                "token": TOKEN,
                "ddc_binary": sys.executable,
                "input_mode": "alt-noverify",
                "lg_hid_binary": sys.executable,
            }
        )
        controller = M1DdcController(config)
        controller._lg_usb = DisconnectedLgUsb()
        with patch("azoria_displayd.modules.kvm.controller.subprocess.run") as run:
            with self.assertRaisesRegex(DdcError, "USB Selection"):
                controller.set_control("input", "usbc")
        run.assert_not_called()

    def test_last_trusted_input_survives_backend_restart(self) -> None:
        class FakeLgUsb:
            def available(self) -> bool:
                return True

            def set_input(self, value: str, *, alternate: bool) -> None:
                self.value = value
                self.alternate = alternate

        with tempfile.TemporaryDirectory() as directory:
            state_file = str(Path(directory) / "monitor-state.json")
            config = Config.from_dict(
                {
                    "token": TOKEN,
                    "ddc_binary": sys.executable,
                    "input_mode": "alt-noverify",
                    "lg_hid_binary": sys.executable,
                    "state_file": state_file,
                }
            )
            first = M1DdcController(config)
            first._lg_usb = FakeLgUsb()
            first.set_control("input", "hdmi2")

            second = M1DdcController(config)
            self.assertEqual(second._last_usb_state["input"], "hdmi2")

    def test_lg_hid_power_mode_sends_exactly_one_write(self) -> None:
        class FakeLgUsb:
            def __init__(self) -> None:
                self.calls: list[bool] = []

            def available(self) -> bool:
                return True

            def set_power(self, value: bool) -> None:
                self.calls.append(value)

        config = Config.from_dict(
            {
                "token": TOKEN,
                "ddc_binary": sys.executable,
                "power_mode": "lg-hid",
                "lg_hid_binary": sys.executable,
            }
        )
        controller = M1DdcController(config)
        usb = FakeLgUsb()
        controller._lg_usb = usb
        with patch("azoria_displayd.modules.kvm.controller.subprocess.run") as run:
            result = controller.set_control("power", False)
        run.assert_not_called()
        self.assertEqual(usb.calls, [False])
        self.assertFalse(result["confirmed"])
        self.assertEqual(result["verification"], "write-only")
        self.assertEqual(result["transport"], "lg-hid")

    def test_numeric_control_uses_hid_and_confirms_over_hid(self) -> None:
        class FakeLgUsb:
            def __init__(self) -> None:
                self.value = 40
                self.calls: list[tuple] = []

            def available(self) -> bool:
                return True

            def set_vcp(self, opcode: int, value: int) -> None:
                self.calls.append(("set", opcode, value))
                self.value = value

            def get_vcp(self, opcode: int) -> tuple[int, int]:
                self.calls.append(("get", opcode))
                return self.value, 100

        config = Config.from_dict(
            {
                "token": TOKEN,
                "ddc_binary": sys.executable,
                "lg_hid_binary": sys.executable,
            }
        )
        controller = M1DdcController(config)
        usb = FakeLgUsb()
        controller._lg_usb = usb
        with patch("azoria_displayd.modules.kvm.controller.subprocess.run") as run:
            result = controller.set_control("volume", 23)
        run.assert_not_called()
        self.assertEqual(usb.calls, [("set", 0x62, 23), ("get", 0x62)])
        self.assertTrue(result["confirmed"])
        self.assertEqual(result["value"], 23)

    def test_lg_usb_is_preferred_for_input_and_power(self) -> None:
        class FakeLgUsb:
            def __init__(self) -> None:
                self.calls: list[tuple] = []
                self.input = "dp1"
                self.power = False

            def available(self) -> bool:
                return True

            def set_input(self, value: str, *, alternate: bool) -> None:
                self.calls.append(("input", value, alternate))
                self.input = value

            def get_input(self) -> str:
                self.calls.append(("get_input",))
                return self.input

            def set_power(self, value: bool) -> None:
                self.calls.append(("power", value))
                self.power = value

            def get_power(self) -> bool:
                self.calls.append(("get_power",))
                return self.power

        usb = FakeLgUsb()
        self.controller._lg_usb = usb
        with patch("azoria_displayd.modules.kvm.controller.subprocess.run") as run:
            input_result = self.controller.set_control("input", "usbc")
            power_result = self.controller.set_control("power", True)
        run.assert_not_called()
        self.assertEqual(
            usb.calls,
            [
                ("input", "usbc", True),
                ("get_input",),
                ("power", True),
                ("get_power",),
            ],
        )
        self.assertEqual(input_result["transport"], "lg-usb")
        self.assertEqual(power_result["transport"], "lg-usb")
        self.assertTrue(input_result["confirmed"])
        self.assertTrue(power_result["confirmed"])

    def test_lg_usb_failure_falls_back_to_m1ddc(self) -> None:
        class FailingLgUsb:
            def available(self) -> bool:
                return True

            def set_input(self, value: str, *, alternate: bool) -> None:
                del value, alternate
                raise LgUsbError("NACK")

            def get_input(self) -> str:
                return "hdmi1"

        self.controller._lg_usb = FailingLgUsb()
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            return_value=subprocess.CompletedProcess([], 0, "17\n", ""),
        ) as run:
            result = self.controller.set_control("input", "hdmi1")
        self.assertEqual(
            run.call_args_list[0].args[0][-3:],
            ["set", "input-alt", "144"],
        )
        self.assertEqual(result["transport"], "m1ddc")

    def test_non_final_only_writes_each_control(self) -> None:
        cases = (
            ("brightness", 42, ["set", "luminance", "42"], 42),
            ("contrast", 43, ["set", "contrast", "43"], 43),
            ("volume", 44, ["set", "volume", "44"], 44),
            ("mute", True, ["set", "mute", "on"], True),
            ("input", "hdmi1", ["set", "input-alt", "144"], "hdmi1"),
            ("power", True, ["set", "standby", "on"], True),
        )
        for control, value, expected_tail, expected_value in cases:
            with self.subTest(control=control):
                with patch(
                    "azoria_displayd.modules.kvm.controller.subprocess.run",
                    return_value=subprocess.CompletedProcess([], 0, "", ""),
                ) as run:
                    result = self.controller.set_control(control, value, final=False)
                self.assertTrue(result["accepted"])
                self.assertFalse(result["confirmed"])
                self.assertEqual(result["value"], expected_value)
                self.assertEqual(run.call_count, 1)
                self.assertEqual(run.call_args.args[0][-len(expected_tail) :], expected_tail)

    def test_final_numeric_accepts_one_point_tolerance(self) -> None:
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            side_effect=[
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "49\n", ""),
            ],
        ) as run:
            result = self.controller.set_control("brightness", 50, final=True)
        self.assertTrue(result["confirmed"])
        self.assertEqual(result["value"], 49)
        self.assertEqual(run.call_count, 2)

    def test_final_numeric_rejects_stable_previous_value(self) -> None:
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            side_effect=[
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "44\n", ""),
                subprocess.CompletedProcess([], 0, "44\n", ""),
                subprocess.CompletedProcess([], 0, "44\n", ""),
            ],
        ) as run:
            with self.assertRaises(DdcError):
                self.controller.set_control("volume", 50, final=True)
        self.assertEqual(run.call_count, 4)

    def test_final_numeric_raises_after_three_unstable_reads(self) -> None:
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            side_effect=[
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "44\n", ""),
                subprocess.CompletedProcess([], 0, "45\n", ""),
                subprocess.CompletedProcess([], 0, "46\n", ""),
            ],
        ) as run:
            with self.assertRaises(DdcError):
                self.controller.set_control("volume", 50, final=True)
        self.assertEqual(run.call_count, 4)

    def test_read_error_breaks_consecutive_stability(self) -> None:
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            side_effect=[
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "44\n", ""),
                subprocess.CompletedProcess([], 1, "", "temporary read failure"),
                subprocess.CompletedProcess([], 0, "44\n", ""),
            ],
        ) as run:
            with self.assertRaises(DdcError):
                self.controller.set_control("volume", 50, final=True)
        self.assertEqual(run.call_count, 4)

    def test_final_mute_reads_back_mute_from_m1ddc(self) -> None:
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            side_effect=[
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "1\n", ""),
            ],
        ) as run:
            result = self.controller.set_control("mute", True, final=True)
        self.assertTrue(result["confirmed"])
        self.assertTrue(result["value"])
        self.assertEqual(
            [call.args[0][-2:] for call in run.call_args_list],
            [["mute", "on"], ["get", "mute"]],
        )

    def test_final_input_rejects_successful_write_without_readback(self) -> None:
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            return_value=subprocess.CompletedProcess([], 0, "", ""),
        ) as run:
            with self.assertRaisesRegex(DdcError, "input verification failed"):
                self.controller.set_control("input", "hdmi2", final=True)
        self.assertEqual(run.call_count, 4)
        self.assertEqual(
            run.call_args_list[0].args[0][-3:],
            ["set", "input-alt", "145"],
        )

    def test_power_maps_to_mccs_standby_values(self) -> None:
        with patch(
            "azoria_displayd.modules.kvm.controller.subprocess.run",
            side_effect=[
                subprocess.CompletedProcess([], 0, "1\n", ""),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "2\n", ""),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "1\n", ""),
            ],
        ) as run:
            self.assertTrue(self.controller.get_power())
            off = self.controller.set_control("power", False)
            on = self.controller.set_control("power", True)
        self.assertTrue(off["confirmed"])
        self.assertTrue(on["confirmed"])
        self.assertEqual(
            [run.call_args_list[index].args[0][-3:] for index in (1, 3)],
            [["set", "standby", "off"], ["set", "standby", "on"]],
        )

    def test_power_falls_back_to_macos_when_monitor_ignores_d6(self) -> None:
        with (
            patch(
                "azoria_displayd.modules.kvm.controller.subprocess.run",
                side_effect=[
                    subprocess.CompletedProcess([], 0, "", ""),
                    subprocess.CompletedProcess([], 0, "1\n", ""),
                    subprocess.CompletedProcess([], 0, "1\n", ""),
                    subprocess.CompletedProcess([], 0, "1\n", ""),
                ],
            ),
            patch.object(self.controller._mac_power, "set_power") as set_power,
        ):
            result = self.controller.set_control("power", False)
        set_power.assert_called_once_with(False)
        self.assertEqual(result["transport"], "macos-display-sleep")
        self.assertTrue(result["confirmed"])
        self.assertFalse(result["value"])

    def test_soft_sleep_is_always_followed_by_explicit_macos_wake(self) -> None:
        self.controller._soft_power_state = False
        with (
            patch("azoria_displayd.modules.kvm.controller.subprocess.run") as run,
            patch.object(self.controller._mac_power, "set_power") as set_power,
        ):
            result = self.controller.set_control("power", True)
        run.assert_not_called()
        set_power.assert_called_once_with(True)
        self.assertEqual(result["transport"], "macos-display-sleep")
        self.assertTrue(result["value"])

    def test_macos_power_mode_skips_slow_ddc_probe(self) -> None:
        config = Config.from_dict(
            {
                "token": TOKEN,
                "ddc_binary": sys.executable,
                "power_mode": "macos",
            }
        )
        controller = M1DdcController(config)
        with (
            patch("azoria_displayd.modules.kvm.controller.subprocess.run") as run,
            patch.object(controller._mac_power, "set_power") as set_power,
        ):
            result = controller.set_control("power", False)
        run.assert_not_called()
        set_power.assert_called_once_with(False)
        self.assertEqual(result["transport"], "macos-display-sleep")
        self.assertTrue(result["confirmed"])

    def test_disabled_input_mode_fails_without_sending_ddc(self) -> None:
        config = Config.from_dict(
            {
                "token": TOKEN,
                "ddc_binary": sys.executable,
                "input_mode": "disabled",
            }
        )
        controller = M1DdcController(config)
        with patch("azoria_displayd.modules.kvm.controller.subprocess.run") as run:
            with self.assertRaisesRegex(DdcError, "input verification failed"):
                controller.set_control("input", "usbc")
        run.assert_not_called()

    def test_rejects_non_boolean_final_before_running_ddc(self) -> None:
        with patch("azoria_displayd.modules.kvm.controller.subprocess.run") as run:
            with self.assertRaisesRegex(ValueError, "final"):
                self.controller.set_control("brightness", 50, final=1)
        run.assert_not_called()

    def test_descending_write_invalidates_cache_and_reads_final_value(self) -> None:
        monitor = {
            "luminance": 66,
            "contrast": 75,
            "volume": 31,
            "mute": 2,
            "input": 15,
            "standby": 1,
        }

        def fake_run(command, **_kwargs):
            operation = command[3]
            control = command[4]
            if operation == "set":
                monitor[control] = int(command[5])
                return subprocess.CompletedProcess(command, 0, "", "")
            return subprocess.CompletedProcess(command, 0, f"{monitor[control]}\n", "")

        with patch("azoria_displayd.modules.kvm.controller.subprocess.run", side_effect=fake_run) as run:
            self.controller.status(force=True)
            self.controller.set_control("volume", 82)
            self.controller.set_control("volume", 19)
            status = self.controller.status()
        self.assertEqual(status["volume"], 19)
        volume_sets = [
            call.args[0][5]
            for call in run.call_args_list
            if call.args[0][3:5] == ["set", "volume"]
        ]
        self.assertEqual(volume_sets, ["82", "19"])


class LgUsbOnlyControllerTests(unittest.TestCase):
    def test_usb_failure_never_invokes_video_ddc(self) -> None:
        class FailingLgUsb:
            def available(self) -> bool:
                return True

            def set_vcp(self, opcode: int, value: int) -> None:
                del opcode, value
                raise LgUsbError("USB write failed")

        config = Config.from_dict(
            {
                "token": TOKEN,
                "controller": "lg-usb",
                "lg_hid_binary": sys.executable,
            }
        )
        controller = LgUsbOnlyController(config)
        controller._lg_usb = FailingLgUsb()
        with patch("azoria_displayd.modules.kvm.controller.subprocess.run") as run:
            with self.assertRaisesRegex(DdcError, "fallback is disabled"):
                controller.set_control("brightness", 35, final=False)
        run.assert_not_called()


class MockProvisioner:
    def __init__(self) -> None:
        self.saved: tuple[str, str] | None = None

    def scan_networks(self) -> list[dict]:
        return [
            {"ssid": "Home 2.4G", "rssi": -42, "secure": True},
            {"ssid": "Guest", "rssi": -71, "secure": False},
        ]

    def configure(self, ssid: str, password: str) -> None:
        self.saved = (ssid, password)

    def device_status(self) -> dict:
        return {
            "configured": "/dev/cu.usbmodem-test",
            "connected": True,
            "devices": ["/dev/cu.usbmodem-test"],
        }


class LatestControlGateTests(unittest.TestCase):
    def test_only_latest_waiting_control_executes(self) -> None:
        gate = LatestControlGate(max_wait_seconds=1)
        first_started = threading.Event()
        release_first = threading.Event()
        executed: list[str] = []
        results: dict[str, tuple[bool, dict | None, str | None]] = {}

        def invoke(name: str, block: bool = False) -> None:
            def operation() -> dict:
                executed.append(name)
                if block:
                    first_started.set()
                    release_first.wait(timeout=1)
                return {"name": name}

            results[name] = gate.execute(operation)

        threads = [
            threading.Thread(target=invoke, args=("first", True)),
            threading.Thread(target=invoke, args=("stale",)),
            threading.Thread(target=invoke, args=("latest",)),
        ]
        threads[0].start()
        self.assertTrue(first_started.wait(timeout=1))
        threads[1].start()
        time.sleep(0.02)
        threads[2].start()
        time.sleep(0.02)
        release_first.set()
        for thread in threads:
            thread.join(timeout=1)

        self.assertEqual(executed, ["first", "latest"])
        self.assertEqual(results["stale"][2], "superseded")
        self.assertTrue(results["latest"][0])

    def test_waiting_control_expires_without_execution(self) -> None:
        gate = LatestControlGate(max_wait_seconds=0.03)
        self.assertTrue(gate._execution_lock.acquire(timeout=0.1))
        called = False

        def operation() -> dict:
            nonlocal called
            called = True
            return {}

        try:
            executed, result, reason = gate.execute(operation)
        finally:
            gate._execution_lock.release()
        self.assertFalse(executed)
        self.assertIsNone(result)
        self.assertEqual(reason, "expired")
        self.assertFalse(called)


class ApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.config = Config.from_dict({"token": TOKEN})
        cls.controller = MockDdcController()
        cls.provisioner = MockProvisioner()
        cls.server = create_server(
            cls.config,
            cls.controller,
            ("127.0.0.1", 0),
            provisioner=cls.provisioner,
        )
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.server.server_port}"
        cls.opener = build_opener(ProxyHandler({}))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def request(self, path: str, payload: dict | None = None, token: str = TOKEN):
        data = json.dumps(payload).encode() if payload is not None else None
        headers = {"Authorization": f"Bearer {token}"}
        if data:
            headers["Content-Type"] = "application/json"
        return self.opener.open(Request(self.base + path, data=data, headers=headers), timeout=2)

    def test_health_does_not_require_token(self) -> None:
        with self.opener.open(self.base + "/v1/health", timeout=2) as response:
            self.assertEqual(json.load(response)["service"], "azoria-displayd")

    def test_setup_page_and_wifi_scan(self) -> None:
        with self.opener.open(self.base + "/setup", timeout=2) as response:
            self.assertIn("连接家庭 Wi‑Fi", response.read().decode())
        with self.opener.open(self.base + "/v1/setup/networks", timeout=2) as response:
            networks = json.load(response)["networks"]
            self.assertEqual(networks[0]["ssid"], "Home 2.4G")

    def test_root_is_control_center_and_reports_usb_device(self) -> None:
        with self.opener.open(self.base + "/", timeout=2) as response:
            page = response.read().decode()
            self.assertIn("显示器控制中心", page)
            self.assertIn(TOKEN, page)
        with self.opener.open(self.base + "/v1/setup/device", timeout=2) as response:
            device = json.load(response)
            self.assertTrue(device["connected"])
            self.assertEqual(device["configured"], "/dev/cu.usbmodem-test")

    def test_setup_applies_password_without_auth_header(self) -> None:
        payload = json.dumps({"ssid": "Home 2.4G", "password": "private-pass"}).encode()
        request = Request(
            self.base + "/v1/setup/apply",
            data=payload,
            headers={"Content-Type": "application/json"},
        )
        with self.opener.open(request, timeout=2) as response:
            self.assertTrue(json.load(response)["ok"])
        self.assertEqual(self.provisioner.saved, ("Home 2.4G", "private-pass"))

    def test_status_requires_token(self) -> None:
        with self.assertRaises(HTTPError) as caught:
            self.request("/v1/status", token="wrong-token")
        self.assertEqual(caught.exception.code, 401)
        caught.exception.close()

    def test_set_and_read_brightness(self) -> None:
        with self.request("/v1/control", {"control": "brightness", "value": 61}) as response:
            result = json.load(response)
            self.assertEqual(result["value"], 61)
            self.assertTrue(result["confirmed"])
        with self.request("/v1/status") as response:
            self.assertEqual(json.load(response)["brightness"], 61)

    def test_removed_controls_are_rejected(self) -> None:
        for control in ("contrast", "power"):
            with self.assertRaises(HTTPError) as caught:
                self.request(
                    "/v1/control", {"control": control, "value": 50}
                )
            self.assertEqual(caught.exception.code, 400)
            caught.exception.close()

    def test_status_omits_removed_controls(self) -> None:
        with self.request("/v1/status") as response:
            status = json.load(response)
        self.assertNotIn("contrast", status)
        self.assertNotIn("power", status)

    def test_control_final_false_is_forwarded(self) -> None:
        with self.request(
            "/v1/control",
            {"control": "mute", "value": True, "final": False},
        ) as response:
            result = json.load(response)
        self.assertTrue(result["accepted"])
        self.assertFalse(result["confirmed"])

    def test_control_rejects_non_boolean_final(self) -> None:
        with self.assertRaises(HTTPError) as caught:
            self.request(
                "/v1/control",
                {"control": "brightness", "value": 61, "final": 1},
            )
        self.assertEqual(caught.exception.code, 400)
        caught.exception.close()

    def test_long_drag_burst_does_not_hit_control_rate_limit(self) -> None:
        values = [*range(20, 81), *range(80, 16, -1)]
        self.assertGreaterEqual(len(values), 120)
        for value in values:
            with self.request(
                "/v1/control", {"control": "volume", "value": value}
            ) as response:
                self.assertEqual(response.status, 200)
        with self.request("/v1/status") as response:
            self.assertEqual(json.load(response)["volume"], values[-1])

    def test_rejects_unknown_control(self) -> None:
        with self.assertRaises(HTTPError) as caught:
            self.request("/v1/control", {"control": "shell", "value": "whoami"})
        self.assertEqual(caught.exception.code, 400)
        caught.exception.close()

    def test_register_and_list_device(self) -> None:
        payload = {
            "device_id": "68EE8F58413C",
            "hostname": "azoria-display-58413c",
            "board": "VIEWE UEDX48480040E-WB-A V1.3",
            "firmware": "0.2.0",
        }
        with self.request("/v1/device/register", payload) as response:
            device = json.load(response)["device"]
            self.assertEqual(device["device_id"], payload["device_id"])
            self.assertEqual(device["address"], "127.0.0.1")
        with self.request("/v1/devices") as response:
            devices = json.load(response)["devices"]
            self.assertEqual(len(devices), 1)
            self.assertTrue(devices[0]["online"])

    def test_udp_discovery_responder(self) -> None:
        responder = UdpDiscoveryResponder(8732, 0)
        responder.start()
        self.assertIsNotNone(responder.socket)
        assert responder.socket is not None
        port = responder.socket.getsockname()[1]
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client.settimeout(2)
        try:
            client.sendto(b"AZORIA_DISCOVER_V1", ("127.0.0.1", port))
            payload, _ = client.recvfrom(256)
            self.assertEqual(payload, b"AZORIA_DDC_V1 8732")
        finally:
            client.close()
            responder.stop()


if __name__ == "__main__":
    unittest.main()
