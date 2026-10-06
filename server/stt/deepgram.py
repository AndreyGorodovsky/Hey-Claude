"""Deepgram streaming speech-to-text.

Speaks Deepgram's live-transcription WebSocket directly. The wire protocol is
small and stable: audio goes up as binary frames, and JSON events come back.
Three of them matter here:

- ``Results`` carries a stretch of transcript. ``is_final`` means it will not
  be revised; ``speech_final`` means Deepgram heard the configured length of
  silence after it, which is the end-of-speech signal.
- ``UtteranceEnd`` is a second end-of-speech signal, worked out from word
  timings instead of silence. It still arrives when background noise keeps
  ``speech_final`` from firing.
"""

from __future__ import annotations

import asyncio
import json
import logging
from contextlib import suppress
from typing import Any, Protocol
from urllib.parse import urlencode

from pydantic import SecretStr
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed, WebSocketException

from server.stt.base import SpeechToText, SpeechToTextError, SpeechToTextStream

log = logging.getLogger(__name__)

_LISTEN_URL = "wss://api.deepgram.com/v1/listen"

#: Seconds allowed for the connection to Deepgram to open.
_OPEN_TIMEOUT = 5.0

#: Seconds allowed for the last results after the stream is closed early.
_DRAIN_TIMEOUT = 3.0

#: Seconds allowed for the closing handshake, which runs in the background.
_CLOSE_TIMEOUT = 2.0

#: Deepgram's shortest accepted value for the word-timing end signal.
_UTTERANCE_END_MS = 1000

#: Connections being closed in the background. Held here so that the tasks
#: are not garbage-collected before they finish.
_closing: set[asyncio.Task[None]] = set()


class _Connection(Protocol):
    """The part of a WebSocket connection the stream uses."""

    def __aiter__(self) -> Any: ...

    async def send(self, message: str | bytes) -> None: ...

    async def close(self) -> None: ...


class DeepgramSpeechToText(SpeechToText):
    """Deepgram live transcription."""

    def __init__(
        self,
        api_key: SecretStr,
        *,
        model: str,
        language: str,
        endpointing_ms: int,
    ) -> None:
        self._api_key = api_key
        self._model = model
        self._language = language
        self._endpointing_ms = endpointing_ms

    async def open(self, sample_rate: int) -> SpeechToTextStream:
        query = urlencode(
            {
                "model": self._model,
                "language": self._language,
                "encoding": "linear16",
                "sample_rate": sample_rate,
                "channels": 1,
                # Interim results show that speech has begun before the first
                # final one arrives, and UtteranceEnd depends on them.
                "interim_results": "true",
                "endpointing": self._endpointing_ms,
                "utterance_end_ms": _UTTERANCE_END_MS,
                # Punctuation and capitals, so that Claude reads a sentence.
                "smart_format": "true",
            }
        )
        try:
            connection = await connect(
                f"{_LISTEN_URL}?{query}",
                additional_headers={
                    "Authorization": f"Token {self._api_key.get_secret_value()}"
                },
                open_timeout=_OPEN_TIMEOUT,
                close_timeout=_CLOSE_TIMEOUT,
            )
        except (OSError, TimeoutError, WebSocketException) as exc:
            # Only the kind of failure is kept: the exception text of a
            # rejected handshake can repeat the request, headers included.
            raise SpeechToTextError(
                f"could not connect to Deepgram ({type(exc).__name__})"
            ) from None
        return DeepgramStream(connection)


class DeepgramStream(SpeechToTextStream):
    """One utterance on an open Deepgram connection."""

    def __init__(self, connection: _Connection) -> None:
        super().__init__()
        self._connection = connection
        self._finishing = False
        self._closed = False
        # Whether every word heard so far is in a final result. False while
        # an interim result holds words that no final one has confirmed.
        self._settled = True
        self._reader = asyncio.create_task(self._read())

    async def _read(self) -> None:
        """Turn Deepgram's events into the tracker's state, until it closes."""
        try:
            async for raw in self._connection:
                if isinstance(raw, bytes):
                    continue
                self.handle(json.loads(raw))
        except ConnectionClosed:
            pass
        except (WebSocketException, ValueError) as exc:
            self.tracker.fail(
                SpeechToTextError(f"Deepgram stream failed ({type(exc).__name__})")
            )
            return
        if not self._finishing:
            self.tracker.fail(SpeechToTextError("Deepgram closed the stream early"))

    def handle(self, event: dict[str, Any]) -> None:
        """Apply one event from Deepgram."""
        kind = event.get("type")
        if kind == "Results":
            alternatives = event.get("channel", {}).get("alternatives") or [{}]
            text = alternatives[0].get("transcript", "")
            if event.get("is_final"):
                self.tracker.add_final(text)
                self._settled = True
            elif text.strip():
                self.tracker.heard()
                self._settled = False
            if event.get("speech_final"):
                self.tracker.end_of_speech()
        elif kind == "UtteranceEnd":
            self.tracker.end_of_speech()
        elif kind == "Error":
            log.warning("Deepgram reported an error on the stream")
            self.tracker.fail(SpeechToTextError("Deepgram reported an error"))

    async def send_audio(self, pcm: bytes) -> None:
        try:
            await self._connection.send(pcm)
        except WebSocketException as exc:
            error = SpeechToTextError(
                f"could not send audio to Deepgram ({type(exc).__name__})"
            )
            self.tracker.fail(error)
            raise error from None

    async def finish(self) -> str:
        self.tracker.raise_if_failed()
        self._finishing = True
        if not (self.tracker.ended and self._settled):
            # Either a timeout stopped the capture before Deepgram heard an
            # ending, or the ending came from UtteranceEnd while the last
            # words were still only in an interim result. Ask Deepgram to
            # flush what it holds and wait for those results. After an
            # ending on a final result there is nothing left to wait for.
            with suppress(WebSocketException):
                await self._connection.send(json.dumps({"type": "CloseStream"}))
            with suppress(TimeoutError):
                async with asyncio.timeout(_DRAIN_TIMEOUT):
                    await asyncio.shield(self._reader)
        await self.aclose()
        return self.tracker.transcript

    async def aclose(self) -> None:
        if self._closed:
            return
        self._closed = self._finishing = True
        self._reader.cancel()
        with suppress(asyncio.CancelledError):
            await self._reader
        # The closing handshake waits on Deepgram and nothing waits on it,
        # so it is left to finish by itself.
        task = asyncio.create_task(self._close_connection())
        _closing.add(task)
        task.add_done_callback(_closing.discard)

    async def _close_connection(self) -> None:
        with suppress(WebSocketException, OSError):
            await self._connection.close()
