"""Paired devices, and the one-time code that pairs a new one.

Typing a long token on a phone is the hard part of setting a device up, so
a device can be paired instead: ``python -m server pair`` makes a short
code, the person types it into the device, and the device trades it once
for a token of its own (``POST /v1/pair``, docs/PROTOCOL.md).

Both live in one SQLite file beside the conversations, kept apart from them
so that credentials and transcripts are never in the same file. The
commands that make a code or remove a device and the server that takes
the code are separate processes; the file is what they share. Neither a
token nor a code is stored as itself, only as its SHA-256 hash, so the
file does not let anyone connect as a device. A plain hash is enough
because both are random: a token carries 256 bits, and a code lasts
minutes and a few tries.

A paired device's token takes the place of any entry for it in
DEVICE_TOKENS, so pairing again, after the token was changed or lost, is
how a device is given a new one; ``unpair`` withdraws it, for a device
that is lost or given away.
"""

from __future__ import annotations

import hashlib
import hmac
import secrets
import sqlite3
from contextlib import closing
from pathlib import Path

from server.protocol import (
    PAIR_CODE_DIGITS,
    PAIR_CODE_LIFETIME_SECONDS,
    PAIR_CODE_TRIES,
)

_SCHEMA = """
CREATE TABLE IF NOT EXISTS paired (
    device_id  TEXT PRIMARY KEY,
    token_hash TEXT NOT NULL,
    paired_at  REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS pending_code (
    only_row   INTEGER PRIMARY KEY CHECK (only_row = 1),
    code_hash  TEXT NOT NULL,
    expires_at REAL NOT NULL,
    tries_left INTEGER NOT NULL
);
"""

#: Random bytes in a token: 43 characters once encoded, within the
#: protocol's 24 to 64.
_TOKEN_BYTES = 32


def _hash(secret: str) -> str:
    return hashlib.sha256(secret.encode()).hexdigest()


class PairedDevices:
    """The paired devices and the pending code, in one SQLite file.

    Every call opens its own connection and blocks; the server calls these
    from a worker thread. Times are seconds since the epoch, passed in so
    that tests can move the clock.
    """

    def __init__(self, path: Path) -> None:
        self._path = path

    def _connect(self) -> sqlite3.Connection:
        # Autocommit, so that each change below is one explicit transaction.
        return sqlite3.connect(self._path, isolation_level=None)

    def initialise(self) -> None:
        """Create the file and its tables if they do not exist."""
        self._path.parent.mkdir(parents=True, exist_ok=True)
        with closing(self._connect()) as db:
            db.executescript(_SCHEMA)

    def new_code(self, now: float) -> str:
        """Make a pairing code, replacing any earlier one, and return it."""
        code = f"{secrets.randbelow(10**PAIR_CODE_DIGITS):0{PAIR_CODE_DIGITS}d}"
        self.initialise()
        with closing(self._connect()) as db:
            db.execute(
                "INSERT OR REPLACE INTO pending_code VALUES (1, ?, ?, ?)",
                (_hash(code), now + PAIR_CODE_LIFETIME_SECONDS, PAIR_CODE_TRIES),
            )
        return code

    def pair(self, device_id: str, code: str, now: float) -> str | None:
        """Trade the pending code for a new token for ``device_id``.

        Returns the token, or ``None`` if there is no code, it has expired,
        or ``code`` is not it. A wrong code uses up one of the tries, and
        the last try or a success ends the code.
        """
        with closing(self._connect()) as db:
            # Taken before reading, so that two requests at once, from two
            # processes or two threads, cannot both use the last try.
            db.execute("BEGIN IMMEDIATE")
            try:
                row = db.execute(
                    "SELECT code_hash, expires_at, tries_left FROM pending_code"
                ).fetchone()
                if row is None:
                    db.execute("COMMIT")
                    return None
                code_hash, expires_at, tries_left = row
                if now >= expires_at:
                    db.execute("DELETE FROM pending_code")
                    db.execute("COMMIT")
                    return None
                if not hmac.compare_digest(_hash(code), code_hash):
                    if tries_left <= 1:
                        db.execute("DELETE FROM pending_code")
                    else:
                        db.execute(
                            "UPDATE pending_code SET tries_left = tries_left - 1"
                        )
                    db.execute("COMMIT")
                    return None
                token = secrets.token_urlsafe(_TOKEN_BYTES)
                db.execute("DELETE FROM pending_code")
                db.execute(
                    "INSERT OR REPLACE INTO paired VALUES (?, ?, ?)",
                    (device_id, _hash(token), now),
                )
                db.execute("COMMIT")
                return token
            except BaseException:
                db.execute("ROLLBACK")
                raise

    def check(self, device_id: str, token: str) -> bool | None:
        """Say whether ``token`` is ``device_id``'s; ``None`` if not paired."""
        with closing(self._connect()) as db:
            row = db.execute(
                "SELECT token_hash FROM paired WHERE device_id = ?", (device_id,)
            ).fetchone()
        if row is None:
            return None
        return hmac.compare_digest(_hash(token), row[0])

    def device_ids(self) -> list[str]:
        """The identifiers of the paired devices, in order."""
        with closing(self._connect()) as db:
            rows = db.execute("SELECT device_id FROM paired ORDER BY device_id")
            return [r[0] for r in rows]

    def unpair(self, device_id: str) -> bool:
        """Withdraw a device's paired token; ``False`` if it was not paired.

        The device can then connect only with a DEVICE_TOKENS entry, if it
        has one, or by pairing again.
        """
        with closing(self._connect()) as db:
            gone = db.execute("DELETE FROM paired WHERE device_id = ?", (device_id,))
            return gone.rowcount > 0
