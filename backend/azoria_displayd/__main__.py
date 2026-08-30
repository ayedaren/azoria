from __future__ import annotations

import argparse
import logging
from pathlib import Path
import signal

from .core.provisioning import UsbProvisioner
from .core.flashing import FirmwareFlasher
from .modules.kvm.api import create_server
from .modules.kvm.config import load_config
from .modules.kvm.discovery import MdnsAdvertiser, UdpDiscoveryResponder
from .modules.kvm.controller import (
    DdcError,
    LgUsbOnlyController,
    M1DdcController,
    MockDdcController,
)


def _request_shutdown(_signum: int, _frame: object) -> None:
    raise KeyboardInterrupt


def main() -> int:
    parser = argparse.ArgumentParser(description="Azoria local module backend")
    parser.add_argument("--config", default=str(Path(__file__).parents[1] / "config.local.json"))
    parser.add_argument("--mock", action="store_true", help="do not send DDC commands")
    parser.add_argument("--mock-state", help="optional JSON file for persisted mock state")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    config = load_config(args.config)
    try:
        if args.mock:
            controller = MockDdcController(args.mock_state)
        elif config.controller == "lg-usb":
            controller = LgUsbOnlyController(config)
        else:
            controller = M1DdcController(config)
    except DdcError as exc:
        logging.error("%s", exc)
        return 2

    provisioner = UsbProvisioner(config.serial_device, config.token, config.port)
    firmware_directory = Path(__file__).parents[1] / "firmware-package"
    flasher = FirmwareFlasher(firmware_directory)
    server = create_server(config, controller, provisioner=provisioner, flasher=flasher)
    advertiser = None
    udp_discovery = None
    if not config.is_loopback_only:
        advertiser = MdnsAdvertiser(config.port)
        udp_discovery = UdpDiscoveryResponder(config.port, bind_address=config.bind)
        advertiser.start()
        udp_discovery.start()
    signal.signal(signal.SIGTERM, _request_shutdown)
    signal.signal(signal.SIGINT, _request_shutdown)
    logging.info("listening on http://%s:%d", config.bind, config.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if advertiser is not None:
            advertiser.stop()
        if udp_discovery is not None:
            udp_discovery.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
