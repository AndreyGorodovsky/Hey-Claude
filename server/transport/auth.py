"""Device authentication.

A device proves who it is with a shared secret, its token, sent once in the
headers of the WebSocket request. The device identifier names the device and
is not secret; the token is what is checked.

A token is either one the device was given by pairing (server/pairing.py)
or the device's entry in DEVICE_TOKENS. A paired token takes the place of
the entry, so that pairing again replaces a token that was changed or lost.
"""

from __future__ import annotations

import hmac
from collections.abc import Mapping

from pydantic import SecretStr

from server.pairing import PairedDevices

_SCHEME = "bearer "

#: Compared against when the device is unknown, so that an unknown device
#: and a wrong token take the same time to reject.
_NO_TOKEN = "-" * 32


def authenticate(
    authorization: str | None,
    device_id: str | None,
    tokens: Mapping[str, SecretStr],
    paired: PairedDevices | None = None,
) -> str | None:
    """Return the device identifier if the headers prove it, else ``None``.

    ``authorization`` is the ``Authorization`` header, expected to be
    ``Bearer <token>``; ``device_id`` is the ``X-Device-Id`` header.
    ``paired`` is read first; it blocks, so call this from a worker thread
    when it is given.
    """
    if not authorization or not device_id:
        return None
    if not authorization.lower().startswith(_SCHEME):
        return None
    presented = authorization[len(_SCHEME) :].strip()

    if paired is not None:
        verdict = paired.check(device_id, presented)
        if verdict is not None:
            return device_id if verdict else None

    expected = tokens.get(device_id)
    expected_value = expected.get_secret_value() if expected else _NO_TOKEN
    # Constant-time comparison: an ordinary one returns sooner the earlier
    # the first wrong character is, which lets a token be guessed in pieces.
    matches = hmac.compare_digest(presented.encode(), expected_value.encode())
    return device_id if expected is not None and matches else None
