"""The speech-to-text interface the turn pipeline is written against.

A provider does two jobs here: it transcribes, and it says when the speaker
has finished. Taking both from one service keeps them in agreement; see
ARCHITECTURE.md.
"""

from __future__ import annotations

import asyncio
from abc import ABC, abstractmethod


class SpeechToTextError(Exception):
    """The speech-to-text service failed or could not be reached."""


class EndpointTracker:
    """What has been heard so far in one utterance, and who is waiting on it.

    An implementation turns its provider's events into calls on this object;
    the waiting, and waking the waiters when the provider fails, is done
    here once for all of them.
    """

    def __init__(self) -> None:
        self._parts: list[str] = []
        self._started = asyncio.Event()
        self._ended = asyncio.Event()
        self._error: SpeechToTextError | None = None

    def heard(self) -> None:
        """Note that speech was recognised, in a result that may yet change."""
        self._started.set()

    def add_final(self, text: str) -> None:
        """Add a stretch of transcript that will not be revised."""
        text = text.strip()
        if text:
            self._parts.append(text)
            self._started.set()

    def end_of_speech(self) -> None:
        """Note that the provider heard the speaker stop.

        Ignored until something has been heard: silence after the wake word
        is the speaker drawing breath, not the end of a request. It does
        count once speech was reported, even if no words were kept. A
        provider can report words and then withdraw them; the request then
        ends empty, at once, instead of waiting out the capture limit.
        """
        if self._started.is_set():
            self._ended.set()

    def fail(self, error: SpeechToTextError) -> None:
        """Wake every waiter with ``error``."""
        if self._error is None:
            self._error = error
        self._started.set()
        self._ended.set()

    @property
    def started(self) -> bool:
        """Whether any speech has been recognised."""
        return self._started.is_set() and self._error is None

    @property
    def ended(self) -> bool:
        """Whether the end of the utterance has been heard."""
        return self._ended.is_set() and self._error is None

    @property
    def transcript(self) -> str:
        """Everything recognised so far."""
        return " ".join(self._parts)

    async def wait_started(self) -> None:
        await self._started.wait()
        self.raise_if_failed()

    async def wait_ended(self) -> None:
        await self._ended.wait()
        self.raise_if_failed()

    def raise_if_failed(self) -> None:
        if self._error is not None:
            raise self._error


class SpeechToTextStream(ABC):
    """One utterance being transcribed as its audio arrives.

    An implementation reports what its provider hears to ``self.tracker``
    and supplies the three methods that touch the provider. A stream that
    can no longer take audio must call ``self.tracker.fail``, so that the
    pipeline is woken with the error instead of waiting for a timeout.
    """

    def __init__(self) -> None:
        self.tracker = EndpointTracker()

    async def speech_started(self) -> None:
        """Return once any speech has been recognised."""
        await self.tracker.wait_started()

    async def speech_ended(self) -> None:
        """Return once the speaker has finished."""
        await self.tracker.wait_ended()

    @abstractmethod
    async def send_audio(self, pcm: bytes) -> None:
        """Feed 16-bit mono PCM at the rate the stream was opened with."""

    @abstractmethod
    async def finish(self) -> str:
        """Stop transcribing and return everything recognised."""

    @abstractmethod
    async def aclose(self) -> None:
        """Release the stream. Safe to call more than once, and after finish.

        Must return promptly: it runs between the end of speech and the
        request to the language model.
        """


class SpeechToText(ABC):
    """A speech-to-text provider."""

    @abstractmethod
    async def open(self, sample_rate: int) -> SpeechToTextStream:
        """Start transcribing one utterance."""
