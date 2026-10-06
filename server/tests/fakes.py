"""Stand-ins for the three services, so the pipeline runs with no network.

Nothing here is recorded: audio is generated silence and every transcript
and reply is written into the test that uses it.
"""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator, Sequence
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

from server.conversation import ConversationStore, Exchange
from server.llm import LanguageModel
from server.pipeline import TurnDeps, TurnLimits
from server.protocol import CAPTURE_SAMPLE_RATE, ErrorCode
from server.stt import SpeechToText, SpeechToTextError, SpeechToTextStream
from server.tts import TextToSpeech

PLAYBACK_RATE = 24_000

#: One 20 ms capture frame of silence.
FRAME = bytes(640)

#: A zone with daylight saving, chosen for no other reason.
TZ = ZoneInfo("Europe/Berlin")
NOON = datetime(2026, 10, 6, 12, 0, tzinfo=TZ)


class FakeSttStream(SpeechToTextStream):
    def __init__(self, owner: FakeStt) -> None:
        super().__init__()
        self._owner = owner
        self.received: list[bytes] = []
        self.closed = False

    async def send_audio(self, pcm: bytes) -> None:
        self.received.append(pcm)
        owner, count = self._owner, len(self.received)
        if owner.fail_after is not None and count >= owner.fail_after:
            error = SpeechToTextError("fake failure")
            self.tracker.fail(error)
            raise error
        if owner.start_after is not None and count >= owner.start_after:
            self.tracker.heard()
        if owner.end_after is not None and count == owner.end_after:
            self.tracker.add_final(owner.transcript)
            self.tracker.end_of_speech()

    async def finish(self) -> str:
        self.tracker.raise_if_failed()
        if self.tracker.started and not self.tracker.ended:
            # Stopped mid-speech: as a provider does when asked to flush,
            # deliver what was said so far.
            self.tracker.add_final(self._owner.transcript)
        return self.tracker.transcript

    async def aclose(self) -> None:
        self.closed = True


@dataclass
class FakeStt(SpeechToText):
    """Hears ``transcript`` after a set number of frames."""

    transcript: str = "What is the capital of France?"
    #: Frame at which speech is first recognised; None for never.
    start_after: int | None = 2
    #: Frame at which the speaker is heard to finish; None for never.
    end_after: int | None = 5
    fail_after: int | None = None
    fail_on_open: bool = False
    streams: list[FakeSttStream] = field(default_factory=list)

    async def open(self, sample_rate: int) -> SpeechToTextStream:
        assert sample_rate == CAPTURE_SAMPLE_RATE
        if self.fail_on_open:
            raise SpeechToTextError("fake failure to connect")
        stream = FakeSttStream(self)
        self.streams.append(stream)
        return stream


@dataclass
class FakeLlm(LanguageModel):
    """Replies with fixed pieces of text, then optionally fails."""

    pieces: Sequence[str] = ("Paris. ", "It is on ", "the Seine.")
    error: Exception | None = None
    #: Set to hold every reply back until released, to act mid-reply.
    gate: asyncio.Event | None = None
    calls: list[tuple[list[Exchange], str]] = field(default_factory=list)

    async def stream_reply(
        self, history: Sequence[Exchange], user_text: str
    ) -> AsyncIterator[str]:
        self.calls.append((list(history), user_text))
        if self.gate is not None:
            await self.gate.wait()
        for piece in self.pieces:
            await asyncio.sleep(0)
            yield piece
        if self.error is not None:
            raise self.error


@dataclass
class FakeTts(TextToSpeech):
    """Returns silence, in pieces that split samples to exercise the framing."""

    bytes_per_character: int = 500
    error: Exception | None = None
    spoken: list[str] = field(default_factory=list)

    async def synthesize(self, text: str, sample_rate: int) -> AsyncIterator[bytes]:
        assert sample_rate == PLAYBACK_RATE
        self.spoken.append(text)
        if self.error is not None:
            raise self.error
        audio = bytes(len(text) * self.bytes_per_character)
        for start in range(0, len(audio), 333):
            yield audio[start : start + 333]


@dataclass
class RecordingIO:
    """Keeps what a turn reported, in order."""

    steps: list[str] = field(default_factory=list)
    audio_bytes: int = 0
    failure: ErrorCode | None = None

    async def capture_done(self) -> None:
        self.steps.append("capture_done")

    async def reply_audio(self, pcm: bytes) -> None:
        if not self.steps or self.steps[-1] != "reply_audio":
            self.steps.append("reply_audio")
        self.audio_bytes += len(pcm)

    async def reply_done(self) -> None:
        self.steps.append("reply_done")

    async def fail(self, code: ErrorCode, message: str) -> None:
        self.steps.append("fail")
        self.failure = code


def make_deps(
    tmp_path: Path,
    *,
    stt: FakeStt | None = None,
    llm: FakeLlm | None = None,
    tts: FakeTts | None = None,
    max_capture_seconds: float = 2.0,
    no_speech_timeout_seconds: float = 1.0,
) -> TurnDeps:
    return TurnDeps(
        stt=stt or FakeStt(),
        llm=llm or FakeLlm(),
        tts=tts or FakeTts(),
        store=ConversationStore(tmp_path / "test.sqlite3", TZ, 4),
        playback_sample_rate=PLAYBACK_RATE,
        limits=TurnLimits(
            max_capture_seconds=max_capture_seconds,
            no_speech_timeout_seconds=no_speech_timeout_seconds,
        ),
        now=lambda: NOON,
    )
