"""Advertising the server on the local network by mDNS.

A device looks the service up each time it connects, so the address of the
machine running the server can change without the device being reconfigured.
"""

from __future__ import annotations

import logging
import socket
from contextlib import suppress

from zeroconf import ServiceInfo, get_all_addresses
from zeroconf.asyncio import AsyncZeroconf

from server.protocol import MDNS_SERVICE_TYPE, PROTOCOL_VERSION, STREAM_PATH

log = logging.getLogger(__name__)

_INSTANCE_NAME = "Hey Claude Server"
_ANY_ADDRESS = "0.0.0.0"


def _outbound_address() -> str | None:
    """The address of the interface this machine reaches the network through.

    Connecting a UDP socket sends nothing; it only makes the system choose a
    route, and with it the local address. The destination is a reserved
    documentation address that is never contacted.
    """
    with suppress(OSError), socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect(("192.0.2.1", 9))
        address = probe.getsockname()[0]
        if not address.startswith(("127.", "0.")):
            return address
    return None


def _addresses(bind_host: str) -> list[str]:
    """The IPv4 addresses to advertise.

    A machine often has more addresses than a device can reach: virtual
    adapters, VPNs, link-local ones. Advertising them all leaves the device
    to guess, so the one on the default route is preferred.
    """
    if bind_host != _ANY_ADDRESS:
        return [bind_host]
    outbound = _outbound_address()
    if outbound is not None:
        return [outbound]
    return [
        address
        for address in get_all_addresses()
        if not address.startswith(("127.", "169.254."))
    ]


class ServiceAdvertiser:
    """Registers the server as an mDNS service while it runs."""

    def __init__(self, bind_host: str, port: int) -> None:
        self._bind_host = bind_host
        self._port = port
        self._zeroconf: AsyncZeroconf | None = None
        self._info: ServiceInfo | None = None

    async def start(self) -> None:
        """Begin advertising. A failure is logged and otherwise ignored:

        a device can still reach the server through its ``server_url``
        setting, so discovery must not stop the server from starting.
        """
        addresses = _addresses(self._bind_host)
        if not addresses:
            log.warning("mdns: no network address to advertise")
            return
        zeroconf: AsyncZeroconf | None = None
        try:
            info = ServiceInfo(
                MDNS_SERVICE_TYPE,
                f"{_INSTANCE_NAME}.{MDNS_SERVICE_TYPE}",
                port=self._port,
                parsed_addresses=addresses,
                properties={"path": STREAM_PATH, "protocol": str(PROTOCOL_VERSION)},
                server=f"{socket.gethostname()}.local.",
            )
            zeroconf = AsyncZeroconf()
            await zeroconf.async_register_service(info)
        except Exception:
            log.exception("mdns: could not advertise the server")
            if zeroconf is not None:
                with suppress(Exception):
                    await zeroconf.async_close()
            return
        self._zeroconf, self._info = zeroconf, info
        log.info("mdns: advertising %s on port %d", MDNS_SERVICE_TYPE, self._port)

    async def stop(self) -> None:
        """Withdraw the advertisement."""
        if self._zeroconf is None or self._info is None:
            return
        try:
            await self._zeroconf.async_unregister_service(self._info)
            await self._zeroconf.async_close()
        except Exception:
            log.exception("mdns: could not withdraw the advertisement")
        self._zeroconf = self._info = None
