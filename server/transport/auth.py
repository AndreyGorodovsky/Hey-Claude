"""Device authentication.

A device proves who it is with a shared secret, its token, sent once in the
headers of the WebSocket request. The device identifier names the device and
is not secret; the token is what is checked.
"""

from __future__ import annotations

import hmac
from collections.abc import Mapping

from pydantic import SecretStr

_SCHEME = "bearer "

#: Compared against when the device is unknown, so that an unknown device
#: and a wrong token take the same time to reject.
_NO_TOKEN = "-" * 32


def authenticate(
    authorization: str | None,
    device_id: str | None,
    tokens: Mapping[str, SecretStr],
) -> str | None:
    """Return the device identifier if the headers prove it, else ``None``.

    ``authorization`` is the ``Authorization`` header, expected to be
    ``Bearer <token>``; ``device_id`` is the ``X-Device-Id`` header.
    """
    if not authorization or not device_id:
        return None
    if not authorization.lower().startswith(_SCHEME):
        return None
    presented = authorization[len(_SCHEME) :].strip()

    expected = tokens.get(device_id)
    expected_value = expected.get_secret_value() if expected else _NO_TOKEN
    # Constant-time comparison: an ordinary one returns sooner the earlier
    # the first wrong character is, which lets a token be guessed in pieces.
    matches = hmac.compare_digest(presented.encode(), expected_value.encode())
    return device_id if expected is not None and matches else None
