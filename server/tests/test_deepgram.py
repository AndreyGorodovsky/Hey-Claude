"""The Deepgram stream, fed hand-written events over a stand-in connection.

The events follow the shapes Deepgram documents, with made-up text. No
connection is opened.
"""

import asyncio
import json

import pytest

from server.stt import SpeechToTextError
from server.stt.deepgram import DeepgramStream


def results(text: str, *, final: bool = False, speech_final: bool = False) -> dict:
    return {
        "type": "Results",
        "is_final": final,
        "speech_final": speech_final,
        "channel": {"alternatives": [{"transcript": text}]},
    }


UTTERANCE_END = {"type": "UtteranceEnd", "channel": [0], "last_word_end": 1.0}


class FakeConnection:
    """Delivers queued events; on CloseStream, delivers the rest and ends."""

    def __init__(self, after_close: list[dict] | None = None) -> None:
        self._incoming: asyncio.Queue[str | None] = asyncio.Queue()
        self._after_close = after_close or []
        self.sent: list[str | bytes] = []
        self.closed = False

    def deliver(self, *events: dict) -> None:
        for event in events:
            self._incoming.put_nowait(json.dumps(event))

    def drop(self) -> None:
        self._incoming.put_nowait(None)

    def __aiter__(self):
        return self

    async def __anext__(self) -> str:
        item = await self._incoming.get()
        if item is None:
            raise StopAsyncIteration
        return item

    async def send(self, message: str | bytes) -> None:
        self.sent.append(message)
        if message == json.dumps({"type": "CloseStream"}):
            self.deliver(*self._after_close)
            self.drop()

    async def close(self) -> None:
        self.closed = True


async def settle() -> None:
    for _ in range(5):
        await asyncio.sleep(0)


async def is_done(awaitable) -> bool:
    try:
        await asyncio.wait_for(awaitable, timeout=0.05)
    except TimeoutError:
        return False
    return True


async def test_a_request_ends_on_speech_final_without_waiting_further():
    connection = FakeConnection()
    stream = DeepgramStream(connection)
    connection.deliver(
        results("what is"),
        results("What is the capital", final=True),
        results("of France?", final=True, speech_final=True),
    )
    await stream.speech_ended()

    assert await stream.finish() == "What is the capital of France?"
    # Everything was final: nothing to flush.
    assert connection.sent == []
    await settle()
    assert connection.closed


async def test_interim_text_starts_speech_but_is_not_transcript():
    connection = FakeConnection()
    stream = DeepgramStream(connection)
    connection.deliver(results(""), results("what"))
    await stream.speech_started()

    assert not await is_done(stream.speech_ended())
    assert stream.tracker.transcript == ""
    await stream.aclose()


async def test_silence_before_any_speech_does_not_end_the_request():
    connection = FakeConnection()
    stream = DeepgramStream(connection)
    connection.deliver(results("", final=True, speech_final=True), UTTERANCE_END)

    assert not await is_done(stream.speech_started())
    assert not await is_done(stream.speech_ended())
    await stream.aclose()


async def test_words_reported_then_withdrawn_end_the_request_empty():
    connection = FakeConnection()
    stream = DeepgramStream(connection)
    connection.deliver(
        results("what was the first"),
        results("", final=True, speech_final=True),
    )
    await stream.speech_ended()

    assert await stream.finish() == ""


async def test_utterance_end_before_the_last_final_waits_for_it():
    # The ending is worked out from interim words; the final result holding
    # them arrives only once the stream is flushed.
    connection = FakeConnection(after_close=[results("of France?", final=True)])
    stream = DeepgramStream(connection)
    connection.deliver(
        results("What is the capital", final=True),
        results("of france"),
        UTTERANCE_END,
    )
    await stream.speech_ended()

    assert await stream.finish() == "What is the capital of France?"
    assert json.dumps({"type": "CloseStream"}) in connection.sent


async def test_stopping_mid_speech_flushes_what_was_said():
    connection = FakeConnection(after_close=[results("Tell me a long", final=True)])
    stream = DeepgramStream(connection)
    connection.deliver(results("tell me"))
    await stream.speech_started()

    assert await stream.finish() == "Tell me a long"


async def test_the_connection_dropping_wakes_the_waiter_with_an_error():
    connection = FakeConnection()
    stream = DeepgramStream(connection)
    connection.drop()

    with pytest.raises(SpeechToTextError):
        await asyncio.wait_for(stream.speech_started(), timeout=1)
    with pytest.raises(SpeechToTextError):
        await stream.finish()
    await stream.aclose()


async def test_an_error_event_fails_the_stream():
    connection = FakeConnection()
    stream = DeepgramStream(connection)
    connection.deliver({"type": "Error", "description": "x"})

    with pytest.raises(SpeechToTextError):
        await asyncio.wait_for(stream.speech_ended(), timeout=1)
    await stream.aclose()


async def test_closing_twice_is_harmless():
    connection = FakeConnection()
    stream = DeepgramStream(connection)
    await stream.aclose()
    await stream.aclose()
    await settle()

    assert connection.closed
