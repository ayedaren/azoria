from __future__ import annotations

import logging
import socket
import subprocess
import threading


LOGGER = logging.getLogger("azoria-displayd")


class MdnsAdvertiser:
    def __init__(self, port: int):
        self.port = port
        self.process: subprocess.Popen[bytes] | None = None

    def start(self) -> None:
        try:
            self.process = subprocess.Popen(
                [
                    "/usr/bin/dns-sd",
                    "-R",
                    "Azoria DDC",
                    "_azoria-ddc._tcp",
                    "local",
                    str(self.port),
                    "api=1",
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            LOGGER.info("advertising _azoria-ddc._tcp on port %d", self.port)
        except OSError as exc:
            LOGGER.warning("mDNS advertisement unavailable: %s", exc)

    def stop(self) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)


class UdpDiscoveryResponder:
    REQUEST = b"AZORIA_DISCOVER_V1"

    def __init__(self, backend_port: int, discovery_port: int = 8733, bind_address: str = "127.0.0.1"):
        self.backend_port = backend_port
        self.discovery_port = discovery_port
        self.bind_address = bind_address
        self.socket: socket.socket | None = None
        self.thread: threading.Thread | None = None
        self._stopping = threading.Event()

    def start(self) -> None:
        try:
            listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((self.bind_address, self.discovery_port))
            listener.settimeout(0.5)
            self.socket = listener
            self.thread = threading.Thread(
                target=self._serve, name="azoria-discovery", daemon=True
            )
            self.thread.start()
            LOGGER.info("listening for UDP discovery on port %d", self.discovery_port)
        except OSError as exc:
            LOGGER.warning("UDP discovery unavailable: %s", exc)

    def _serve(self) -> None:
        assert self.socket is not None
        response = f"AZORIA_DDC_V1 {self.backend_port}".encode("ascii")
        while not self._stopping.is_set():
            try:
                payload, address = self.socket.recvfrom(256)
            except socket.timeout:
                continue
            except OSError:
                break
            if payload.strip() != self.REQUEST:
                continue
            try:
                self.socket.sendto(response, address)
                LOGGER.info("answered UDP discovery from %s", address[0])
            except OSError:
                if not self._stopping.is_set():
                    LOGGER.warning("failed to answer UDP discovery from %s", address[0])

    def stop(self) -> None:
        self._stopping.set()
        if self.socket is not None:
            self.socket.close()
        if self.thread is not None:
            self.thread.join(timeout=2)
