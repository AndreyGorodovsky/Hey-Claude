from datetime import date, datetime, timezone
from zoneinfo import ZoneInfo

import pytest

from server.conversation import ConversationStore, Exchange, chat_date

TZ = ZoneInfo("Europe/Berlin")


def local(*args: int) -> datetime:
    return datetime(*args, tzinfo=TZ)


def store_at(path) -> ConversationStore:
    return ConversationStore(path, TZ, 4)


def test_day_changes_at_the_rollover_hour_not_midnight():
    assert chat_date(local(2026, 10, 6, 23, 59), TZ, 4) == date(2026, 10, 6)
    assert chat_date(local(2026, 10, 7, 0, 0), TZ, 4) == date(2026, 10, 6)
    assert chat_date(local(2026, 10, 7, 3, 59), TZ, 4) == date(2026, 10, 6)
    assert chat_date(local(2026, 10, 7, 4, 0), TZ, 4) == date(2026, 10, 7)


def test_instant_is_converted_to_the_configured_zone():
    # 01:30 UTC is 03:30 in Berlin in October: still the previous day.
    instant = datetime(2026, 10, 7, 1, 30, tzinfo=timezone.utc)
    assert chat_date(instant, TZ, 4) == date(2026, 10, 6)


def test_rollover_at_midnight_is_the_calendar_date():
    assert chat_date(local(2026, 10, 7, 0, 0), TZ, 0) == date(2026, 10, 7)


def test_naive_datetime_is_refused():
    with pytest.raises(ValueError):
        chat_date(datetime(2026, 10, 7, 12, 0), TZ, 4)


async def test_exchanges_come_back_in_order(tmp_path):
    store = store_at(tmp_path / "db.sqlite3")
    await store.initialise()
    now = local(2026, 10, 6, 12, 0)
    await store.append("dev", Exchange("one", "first"), now)
    await store.append("dev", Exchange("two", "second"), now)
    assert await store.history("dev", now) == [
        Exchange("one", "first"),
        Exchange("two", "second"),
    ]


async def test_conversations_are_separate_by_device_and_day(tmp_path):
    store = store_at(tmp_path / "db.sqlite3")
    await store.initialise()
    evening = local(2026, 10, 6, 22, 0)
    await store.append("dev", Exchange("q", "a"), evening)

    assert await store.history("other", evening) == []
    # Past midnight is the same conversation; past the rollover is not.
    assert await store.history("dev", local(2026, 10, 7, 3, 0)) == [Exchange("q", "a")]
    assert await store.history("dev", local(2026, 10, 7, 4, 0)) == []


async def test_a_new_day_removes_every_earlier_conversation(tmp_path):
    store = store_at(tmp_path / "db.sqlite3")
    await store.initialise()
    yesterday, today = local(2026, 10, 6, 12, 0), local(2026, 10, 7, 12, 0)
    await store.append("dev", Exchange("old", "old"), yesterday)
    await store.append("other", Exchange("old", "old"), yesterday)

    # Still there until something is recorded on the new day.
    assert await store.history("dev", yesterday) == [Exchange("old", "old")]

    await store.append("dev", Exchange("new", "new"), today)
    assert await store.history("dev", yesterday) == []
    assert await store.history("other", yesterday) == []
    assert await store.history("dev", today) == [Exchange("new", "new")]

    # A later exchange on the same day removes nothing.
    await store.append("dev", Exchange("more", "more"), today)
    assert len(await store.history("dev", today)) == 2


async def test_history_survives_a_restart(tmp_path):
    path = tmp_path / "nested" / "db.sqlite3"
    now = local(2026, 10, 6, 12, 0)
    first = store_at(path)
    await first.initialise()
    await first.append("dev", Exchange("q", "a"), now)

    second = store_at(path)
    await second.initialise()
    assert await second.history("dev", now) == [Exchange("q", "a")]
