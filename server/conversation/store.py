"""Conversations, kept per device and per chat date in SQLite.

A conversation is every completed exchange a device had on one chat date.
The chat date is the calendar date shifted by the rollover hour, so that a
conversation held past midnight is not split in two.

Only the current day is kept. The first exchange recorded on a new chat date
removes every earlier day's conversations, for all devices.

The database holds transcripts, which are personal data. It lives outside
version control; see SECRETS.md.
"""

from __future__ import annotations

import asyncio
import sqlite3
from contextlib import closing
from dataclasses import dataclass
from datetime import date, datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

_SCHEMA = """
CREATE TABLE IF NOT EXISTS exchanges (
    id             INTEGER PRIMARY KEY,
    device_id      TEXT NOT NULL,
    chat_date      TEXT NOT NULL,
    created_at     TEXT NOT NULL,
    user_text      TEXT NOT NULL,
    assistant_text TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS exchanges_by_conversation
    ON exchanges (device_id, chat_date, id);
"""


@dataclass(frozen=True)
class Exchange:
    """One completed request and its reply."""

    user_text: str
    assistant_text: str


def chat_date(now: datetime, tz: ZoneInfo, rollover_hour: int) -> date:
    """Return the chat date that the instant ``now`` belongs to.

    The day changes at ``rollover_hour`` local time, not at midnight: with a
    rollover of 4, 03:59 on the 2nd still belongs to the 1st.
    """
    if now.tzinfo is None:
        raise ValueError("chat_date needs a timezone-aware datetime")
    # Subtracting from a datetime in its own zone moves the wall clock, which
    # is what is wanted: the rollover is defined on the local clock.
    return (now.astimezone(tz) - timedelta(hours=rollover_hour)).date()


class ConversationStore:
    """SQLite-backed store of completed exchanges.

    The store owns the rule for which day an instant belongs to, so callers
    pass the time and never a date.

    Every operation opens its own connection in a worker thread. SQLite calls
    block, and the event loop must stay free to move audio; the calls are a
    few per request, so the cost of opening a connection each time is nothing.
    """

    def __init__(self, path: Path, tz: ZoneInfo, rollover_hour: int) -> None:
        self._path = path
        self._tz = tz
        self._rollover_hour = rollover_hour

    def _day(self, now: datetime) -> str:
        return chat_date(now, self._tz, self._rollover_hour).isoformat()

    def _connect(self) -> sqlite3.Connection:
        return sqlite3.connect(self._path)

    def _initialise(self) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        with closing(self._connect()) as db, db:
            db.executescript(_SCHEMA)

    def _history(self, device_id: str, day: str) -> list[Exchange]:
        with closing(self._connect()) as db:
            rows = db.execute(
                "SELECT user_text, assistant_text FROM exchanges"
                " WHERE device_id = ? AND chat_date = ? ORDER BY id",
                (device_id, day),
            ).fetchall()
        return [Exchange(user_text, assistant_text) for user_text, assistant_text in rows]

    def _append(
        self, device_id: str, day: str, exchange: Exchange, now: datetime
    ) -> None:
        with closing(self._connect()) as db, db:
            # Earlier days go in the same transaction as the first exchange
            # of the new one. Dates in ISO form compare correctly as text.
            db.execute("DELETE FROM exchanges WHERE chat_date < ?", (day,))
            db.execute(
                "INSERT INTO exchanges"
                " (device_id, chat_date, created_at, user_text, assistant_text)"
                " VALUES (?, ?, ?, ?, ?)",
                (
                    device_id,
                    day,
                    now.isoformat(),
                    exchange.user_text,
                    exchange.assistant_text,
                ),
            )

    async def initialise(self) -> None:
        """Create the database file and its table if they do not exist."""
        await asyncio.to_thread(self._initialise)

    async def history(self, device_id: str, now: datetime) -> list[Exchange]:
        """Return a device's exchanges on the chat date of ``now``, oldest first."""
        return await asyncio.to_thread(self._history, device_id, self._day(now))

    async def append(self, device_id: str, exchange: Exchange, now: datetime) -> None:
        """Record a completed exchange under the chat date of ``now``.

        Conversations of earlier chat dates are deleted as this is done.

        Passing the same ``now`` that the history was read with keeps a
        request made across the rollover in the conversation it was
        answered from.
        """
        await asyncio.to_thread(
            self._append, device_id, self._day(now), exchange, now
        )
