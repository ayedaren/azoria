from __future__ import annotations

from dataclasses import dataclass, field
import ipaddress
import json
from pathlib import Path
from typing import Any


DEFAULT_NETWORKS = (
    "127.0.0.0/8",
    "::1/128",
)

SAFE_NETWORKS = tuple(
    ipaddress.ip_network(value)
    for value in (
        "127.0.0.0/8",
        "10.0.0.0/8",
        "172.16.0.0/12",
        "192.168.0.0/16",
        "169.254.0.0/16",
        "::1/128",
        "fe80::/10",
        "fc00::/7",
    )
)


def _network_is_local(network: ipaddress.IPv4Network | ipaddress.IPv6Network) -> bool:
    return any(
        network.version == safe.version and network.subnet_of(safe)
        for safe in SAFE_NETWORKS
    )


def _address_is_local(address: ipaddress.IPv4Address | ipaddress.IPv6Address) -> bool:
    return any(
        address.version == safe.version and address in safe
        for safe in SAFE_NETWORKS
    )


@dataclass(frozen=True)
class Config:
    bind: str = "127.0.0.1"
    port: int = 8732
    token: str = ""
    controller: str = "m1ddc"
    display: str = "1"
    ddc_binary: str = "m1ddc"
    input_mode: str = "alt"
    power_mode: str = "auto"
    power_control_enabled: bool = True
    lg_hid_binary: str = ""
    state_file: str = ""
    serial_device: str = "/dev/cu.usbmodem83101"
    command_timeout_seconds: float = 4.0
    status_cache_seconds: float = 2.0
    allowed_networks: tuple[ipaddress.IPv4Network | ipaddress.IPv6Network, ...] = field(
        default_factory=lambda: tuple(ipaddress.ip_network(value) for value in DEFAULT_NETWORKS)
    )

    @classmethod
    def from_dict(cls, raw: dict[str, Any]) -> "Config":
        token = str(raw.get("token", ""))
        if len(token) < 20:
            raise ValueError("token must contain at least 20 characters")

        port = int(raw.get("port", 8732))
        if not 1 <= port <= 65535:
            raise ValueError("port must be between 1 and 65535")

        input_mode = str(raw.get("input_mode", "alt")).lower()
        if input_mode not in {"standard", "alt", "alt-noverify", "disabled"}:
            raise ValueError(
                "input_mode must be 'standard', 'alt', 'alt-noverify', or 'disabled'"
            )

        power_mode = str(raw.get("power_mode", "auto")).lower()
        if power_mode not in {"auto", "ddc", "macos", "lg-hid"}:
            raise ValueError(
                "power_mode must be 'auto', 'ddc', 'macos', or 'lg-hid'"
            )

        controller = str(raw.get("controller", "m1ddc")).lower()
        if controller not in {"lg-usb", "m1ddc"}:
            raise ValueError("controller must be 'lg-usb' or 'm1ddc'")

        bind = str(raw.get("bind", "127.0.0.1"))
        try:
            bind_address = ipaddress.ip_address(bind)
        except ValueError as exc:
            raise ValueError("bind must be a literal local IP address") from exc
        if not _address_is_local(bind_address):
            raise ValueError("bind must be a loopback or private local IP address")

        network_values = raw.get("allowed_networks", DEFAULT_NETWORKS)
        networks = tuple(ipaddress.ip_network(value) for value in network_values)
        if not networks or any(not _network_is_local(network) for network in networks):
            raise ValueError(
                "allowed_networks may contain only loopback or private local networks"
            )
        return cls(
            bind=bind,
            port=port,
            token=token,
            controller=controller,
            display=str(raw.get("display", "1")),
            ddc_binary=str(raw.get("ddc_binary", "m1ddc")),
            input_mode=input_mode,
            power_mode=power_mode,
            power_control_enabled=bool(raw.get("power_control_enabled", True)),
            lg_hid_binary=str(raw.get("lg_hid_binary", "")),
            state_file=str(raw.get("state_file", "")),
            serial_device=str(raw.get("serial_device", "/dev/cu.usbmodem83101")),
            command_timeout_seconds=float(raw.get("command_timeout_seconds", 4)),
            status_cache_seconds=float(raw.get("status_cache_seconds", 2)),
            allowed_networks=networks,
        )

    def client_allowed(self, address: str) -> bool:
        ip = ipaddress.ip_address(address.split("%", 1)[0])
        return any(ip in network for network in self.allowed_networks if ip.version == network.version)

    @property
    def is_loopback_only(self) -> bool:
        return ipaddress.ip_address(self.bind).is_loopback


def load_config(path: str | Path) -> Config:
    config_path = Path(path).expanduser()
    with config_path.open("r", encoding="utf-8") as handle:
        raw = json.load(handle)
    if not isinstance(raw, dict):
        raise ValueError("configuration root must be an object")
    return Config.from_dict(raw)
