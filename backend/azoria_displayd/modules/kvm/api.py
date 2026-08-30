from __future__ import annotations

from collections import defaultdict, deque
import hmac
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import logging
from pathlib import Path
import threading
import time
from typing import Any, Protocol
from urllib.parse import urlparse

from ...core.provisioning import ProvisioningError
from ...core.flashing import FirmwareFlasher, FlashingError
from .config import Config
from .controller import DdcError


LOGGER = logging.getLogger("azoria-displayd")
MAX_BODY_BYTES = 2048


class Controller(Protocol):
    def status(self, force: bool = False) -> dict[str, Any]: ...
    def set_control(self, control: str, value: Any, final: bool = True) -> dict[str, Any]: ...


class Provisioner(Protocol):
    def scan_networks(self) -> list[dict[str, Any]]: ...
    def configure(self, ssid: str, password: str) -> None: ...
    def device_status(self) -> dict[str, Any]: ...


class RateLimiter:
    def __init__(self, limit: int, window_seconds: float):
        self.limit = limit
        self.window = window_seconds
        self._requests: dict[str, deque[float]] = defaultdict(deque)
        self._lock = threading.Lock()

    def allow(self, client: str) -> bool:
        now = time.monotonic()
        with self._lock:
            requests = self._requests[client]
            while requests and requests[0] < now - self.window:
                requests.popleft()
            if len(requests) >= self.limit:
                return False
            requests.append(now)
            return True


class LatestControlGate:
    """Serialize display writes while retaining only the latest waiting value."""

    def __init__(self, max_wait_seconds: float = 1.5):
        self.max_wait_seconds = max_wait_seconds
        self._generation = 0
        self._state_lock = threading.Lock()
        self._execution_lock = threading.Lock()

    def execute(self, operation: Any) -> tuple[bool, dict[str, Any] | None, str | None]:
        arrived = time.monotonic()
        with self._state_lock:
            self._generation += 1
            generation = self._generation
        remaining = self.max_wait_seconds - (time.monotonic() - arrived)
        if remaining <= 0 or not self._execution_lock.acquire(timeout=remaining):
            return False, None, "expired"
        try:
            with self._state_lock:
                if generation != self._generation:
                    return False, None, "superseded"
            return True, operation(), None
        finally:
            self._execution_lock.release()


class DeviceRegistry:
    def __init__(self):
        self._devices: dict[str, dict[str, Any]] = {}
        self._lock = threading.Lock()

    def register(self, address: str, payload: dict[str, Any]) -> dict[str, Any]:
        device_id = payload.get("device_id")
        if not isinstance(device_id, str) or not 3 <= len(device_id) <= 64:
            raise ValueError("invalid device_id")
        device = {
            "device_id": device_id,
            "hostname": payload.get("hostname", ""),
            "board": payload.get("board", ""),
            "firmware": payload.get("firmware", ""),
            "address": address,
            "last_seen": int(time.time()),
        }
        for key in ("hostname", "board", "firmware"):
            if not isinstance(device[key], str) or len(device[key]) > 100:
                raise ValueError(f"invalid {key}")
        with self._lock:
            self._devices[device_id] = device
        return device

    def list(self) -> list[dict[str, Any]]:
        now = int(time.time())
        with self._lock:
            return [{**device, "online": now - int(device["last_seen"]) < 45} for device in self._devices.values()]


def make_handler(config: Config, controller: Controller, registry: DeviceRegistry, provisioner: Provisioner | None, flasher: FirmwareFlasher | None) -> type[BaseHTTPRequestHandler]:
    limiters = {
        "control": RateLimiter(160, 10.0),
        "status": RateLimiter(60, 10.0),
        "device": RateLimiter(30, 10.0),
        "health": RateLimiter(60, 10.0),
        "setup": RateLimiter(20, 60.0),
    }
    control_gate = LatestControlGate()
    setup_page = Path(__file__).with_name("setup.html").read_bytes()
    control_page = Path(__file__).with_name("index.html").read_text(encoding="utf-8")
    control_page = control_page.replace(
        "__AZORIA_TOKEN_JSON__", json.dumps(config.token)
    ).encode("utf-8")

    class Handler(BaseHTTPRequestHandler):
        server_version = "AzoriaDisplayd/0.1"

        def log_message(self, format_string: str, *arguments: Any) -> None:
            LOGGER.info("%s %s", self.client_address[0], format_string % arguments)

        def _json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
            body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(body)

        def _setup_page(self) -> None:
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(setup_page)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'none'; object-src 'none'; frame-ancestors 'none'")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("X-Frame-Options", "DENY")
            self.end_headers()
            self.wfile.write(setup_page)

        def _control_page(self) -> None:
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(control_page)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'none'; object-src 'none'; frame-ancestors 'none'")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("X-Frame-Options", "DENY")
            self.end_headers()
            self.wfile.write(control_page)

        def _local_setup_allowed(self) -> bool:
            client = self.client_address[0]
            if client not in {"127.0.0.1", "::1"}:
                self._json(HTTPStatus.FORBIDDEN, {"ok": False, "error": "setup is local-only"})
                return False
            if not limiters["setup"].allow(client):
                self._json(HTTPStatus.TOO_MANY_REQUESTS, {"ok": False, "error": "rate limit"})
                return False
            if provisioner is None:
                self._json(HTTPStatus.SERVICE_UNAVAILABLE, {"ok": False, "error": "USB provisioning unavailable"})
                return False
            return True

        def _preflight(self, require_auth: bool = True, bucket: str = "status") -> bool:
            client = self.client_address[0]
            if not config.client_allowed(client):
                self._json(HTTPStatus.FORBIDDEN, {"ok": False, "error": "client network denied"})
                return False
            if require_auth and not hmac.compare_digest(self.headers.get("Authorization", ""), f"Bearer {config.token}"):
                self._json(HTTPStatus.UNAUTHORIZED, {"ok": False, "error": "unauthorized"})
                return False
            if not limiters[bucket].allow(client):
                self._json(HTTPStatus.TOO_MANY_REQUESTS, {"ok": False, "error": "rate limit"})
                return False
            return True

        def do_GET(self) -> None:
            path = urlparse(self.path).path
            if path == "/":
                if self._local_setup_allowed():
                    self._control_page()
                return
            if path == "/setup":
                if self._local_setup_allowed():
                    self._setup_page()
                return
            if path == "/v1/setup/device":
                if not self._local_setup_allowed():
                    return
                flash_status = flasher.status() if flasher is not None else {"firmware_available": False, "flashing": False}
                self._json(HTTPStatus.OK, {"ok": True, **provisioner.device_status(), **flash_status})
                return
            if path == "/v1/setup/networks":
                if not self._local_setup_allowed():
                    return
                try:
                    self._json(HTTPStatus.OK, {"ok": True, "networks": provisioner.scan_networks()})
                except ProvisioningError as exc:
                    self._json(HTTPStatus.SERVICE_UNAVAILABLE, {"ok": False, "error": str(exc)})
                return
            if path == "/v1/health":
                if self._preflight(False, "health"):
                    self._json(HTTPStatus.OK, {"ok": True, "service": "azoria-displayd"})
                return
            if path == "/v1/devices":
                if self._preflight(bucket="device"):
                    self._json(HTTPStatus.OK, {"ok": True, "devices": registry.list()})
                return
            if path != "/v1/status":
                self._json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "not found"})
                return
            if not self._preflight(bucket="status"):
                return
            try:
                status = controller.status()
                self._json(
                    HTTPStatus.OK,
                    {
                        key: status[key]
                        for key in ("brightness", "volume", "mute", "input")
                        if key in status
                    },
                )
            except DdcError as exc:
                self._json(HTTPStatus.BAD_GATEWAY, {"ok": False, "error": str(exc)})

        def do_POST(self) -> None:
            path = urlparse(self.path).path
            if path == "/v1/setup/flash":
                if not self._local_setup_allowed():
                    return
                payload = self._read_json()
                if payload is None:
                    return
                device = payload.get("device")
                if not isinstance(device, str) or flasher is None:
                    self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "无效的刷写目标"})
                    return
                try:
                    self._json(HTTPStatus.OK, flasher.flash(device))
                except FlashingError as exc:
                    self._json(HTTPStatus.SERVICE_UNAVAILABLE, {"ok": False, "error": str(exc)})
                return
            if path == "/v1/setup/apply":
                if not self._local_setup_allowed():
                    return
                payload = self._read_json()
                if payload is None:
                    return
                ssid, password = payload.get("ssid"), payload.get("password")
                if not isinstance(ssid, str) or not isinstance(password, str):
                    self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "ssid and password must be strings"})
                    return
                try:
                    provisioner.configure(ssid, password)
                    self._json(HTTPStatus.OK, {"ok": True, "message": "configuration saved"})
                except ProvisioningError as exc:
                    self._json(HTTPStatus.SERVICE_UNAVAILABLE, {"ok": False, "error": str(exc)})
                return
            if path not in {"/v1/control", "/v1/device/register"}:
                self._json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "not found"})
                return
            bucket = "control" if path == "/v1/control" else "device"
            if not self._preflight(bucket=bucket):
                return
            payload = self._read_json()
            if payload is None:
                return
            if path == "/v1/device/register":
                try:
                    self._json(HTTPStatus.OK, {"ok": True, "device": registry.register(self.client_address[0], payload)})
                except ValueError as exc:
                    self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
                return
            control = payload.get("control")
            final = payload.get("final", True)
            if not isinstance(control, str) or not isinstance(final, bool):
                self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid control request"})
                return
            if control not in {"brightness", "volume", "mute", "input"}:
                self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "unsupported control"})
                return
            try:
                executed, result, reason = control_gate.execute(lambda: controller.set_control(control, payload.get("value"), final=final))
                if not executed:
                    self._json(HTTPStatus.OK, {"ok": True, "accepted": False, "confirmed": False, "dropped": True, "reason": reason, "control": control, "value": payload.get("value")})
                    return
                self._json(HTTPStatus.OK, result or {"ok": True})
            except ValueError as exc:
                self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
            except DdcError as exc:
                self._json(HTTPStatus.BAD_GATEWAY, {"ok": False, "error": str(exc)})

        def _read_json(self) -> dict[str, Any] | None:
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = -1
            if length < 1 or length > MAX_BODY_BYTES:
                self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid body length"})
                return None
            try:
                payload = json.loads(self.rfile.read(length))
                if not isinstance(payload, dict):
                    raise ValueError("body must be an object")
                return payload
            except (json.JSONDecodeError, UnicodeDecodeError, ValueError) as exc:
                self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
                return None

    return Handler


def create_server(config: Config, controller: Controller, address: tuple[str, int] | None = None, provisioner: Provisioner | None = None, flasher: FirmwareFlasher | None = None) -> ThreadingHTTPServer:
    registry = DeviceRegistry()
    return ThreadingHTTPServer(address or (config.bind, config.port), make_handler(config, controller, registry, provisioner, flasher))
