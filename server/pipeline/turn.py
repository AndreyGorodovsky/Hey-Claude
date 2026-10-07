"""One turn: a spoken request in, a spoken reply out.

A turn runs as one task, started when a device sends ``utterance_start``. It
goes through two phases:

1. Capture. Audio from the device is passed to speech-to-text until the
   speaker finishes, nothing is said, or the capture limit is reached. The
   device is then told to stop sending.
2. Reply. The transcript goes to the language model with the day's earlier
   exchanges. The reply is cut into sentences as it is generated; each is
   synthesised while the next is still being written, and sent while the
   next is being synthesised.

Once started, a turn runs to its end; nothing a device sends interrupts it.
An exchange is recorded in the conversation only if its turn completed, so
the stored history always alternates between the person and the assistant.

This module decides what happens and in what order. It does not know how
any of it is put on the wire: it reads audio from a queue and reports each
step through ``TurnIO``, which the connection turns into protocol messages.
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections import deque
from collections.abc import Callable
from contextlib import suppress
from dataclasses import dataclass, field
from datetime import datetime
from typing import Protocol

from server.conversation import ConversationStore, Exchange
from server.llm import LanguageModel, LanguageModelError, ReplyRefused
from server.pipeline.sentences import SentenceSplitter
from server.protocol import (
    CAPTURE_SAMPLE_RATE,
    FIRST_AUDIO_LIMIT_SECONDS,
    SAMPLE_WIDTH_BYTES,
    ErrorCode,
)
from server.stt import SpeechToText, SpeechToTextError, SpeechToTextStream
from server.tts import TextToSpeech, TextToSpeechError

log = logging.getLogger(__name__)

#: Spoken when the model declines a request without saying anything itself.
_REFUSAL_LINE = "Sorry, I can't help with that one."


class ConnectionLost(Exception):
    """The device went away while a turn was reporting to it."""


class TurnIO(Protocol):
    """What a turn reports to the device, in the order it happens.

    Every method may raise ``ConnectionLost``.
    """

    async def capture_done(self) -> None:
        """The request is over; the device should stop sending audio."""

    async def reply_audio(self, pcm: bytes) -> None:
        """Reply audio, in pieces of any size, at the playback rate.

        Returns when the device can take more, which may be much later:
        this is where a reply is slowed to the speed of playback.
        """

    async def reply_done(self) -> None:
        """The turn is over. Called with no audio when nothing was said."""

    async def fail(self, code: ErrorCode, message: str) -> None:
        """The turn was abandoned."""


@dataclass(frozen=True)
class TurnLimits:
    """How long a turn waits at each point where it can wait."""

    #: Longest a request may run, from ``utterance_start``.
    max_capture_seconds: float = 13.0
    #: Longest wait for the first recognised word.
    no_speech_timeout_seconds: float = 5.0
    #: Audio that may wait to be sent before the next sentence is held back
    #: from the synthesiser.
    synthesis_ahead_seconds: float = 5.0
    #: Longest wait for the first reply audio, from the end of the capture.
    first_audio_seconds: float = FIRST_AUDIO_LIMIT_SECONDS


@dataclass(frozen=True)
class TurnDeps:
    """Everything a turn needs that outlives it."""

    stt: SpeechToText
    llm: LanguageModel
    tts: TextToSpeech
    store: ConversationStore
    playback_sample_rate: int
    limits: TurnLimits = field(default_factory=TurnLimits)
    log_transcripts: bool = False
    #: The current time; replaced in tests.
    now: Callable[[], datetime] = lambda: datetime.now().astimezone()


@dataclass
class _Timings:
    """Moments of one turn, on the monotonic clock, for the latency log."""

    started: float
    capture_done: float | None = None
    first_text: float | None = None
    first_audio: float | None = None


async def run_turn(
    device_id: str,
    audio: asyncio.Queue[bytes],
    io: TurnIO,
    deps: TurnDeps,
) -> None:
    """Run one turn to its end, reporting any failure to the device.

    ``audio`` supplies the device's capture frames. Returns normally whatever
    happened, except when cancelled.
    """
    timings = _Timings(started=time.monotonic())
    try:
        transcript = await _capture(audio, io, deps, timings)
        if deps.log_transcripts:
            log.info("device %s: heard %r", device_id, transcript)
        if not transcript:
            # The wake word fired and nothing was said.
            await io.reply_done()
            return
        await _reply(device_id, transcript, io, deps, timings)
    except ConnectionLost:
        # Ordinary: the device lost power or WiFi. There is nobody to tell.
        log.info("device %s: disconnected during a turn", device_id)
    except SpeechToTextError as exc:
        log.error("device %s: speech-to-text failed: %s", device_id, exc)
        await _fail(io, ErrorCode.STT_FAILED, "Speech recognition failed.")
    except LanguageModelError as exc:
        log.error("device %s: language model failed: %s", device_id, exc)
        await _fail(io, ErrorCode.LLM_FAILED, "The assistant failed to answer.")
    except TextToSpeechError as exc:
        log.error("device %s: text-to-speech failed: %s", device_id, exc)
        await _fail(io, ErrorCode.TTS_FAILED, "Speech synthesis failed.")
    except Exception:
        log.exception("device %s: unexpected failure in a turn", device_id)
        await _fail(io, ErrorCode.INTERNAL, "The server failed.")
    else:
        _log_timings(device_id, timings)


async def _fail(io: TurnIO, code: ErrorCode, message: str) -> None:
    # The device may be the thing that failed; a turn must still end quietly.
    with suppress(ConnectionLost):
        await io.fail(code, message)


def _log_timings(device_id: str, timings: _Timings) -> None:
    if (
        timings.capture_done is None
        or timings.first_text is None
        or timings.first_audio is None
    ):
        return
    log.info(
        "device %s: capture %.2f s; from end of speech, first text %.2f s,"
        " first audio %.2f s",
        device_id,
        timings.capture_done - timings.started,
        timings.first_text - timings.capture_done,
        timings.first_audio - timings.capture_done,
    )


async def _pump(audio: asyncio.Queue[bytes], stream: SpeechToTextStream) -> None:
    """Pass the device's audio to speech-to-text until cancelled."""
    try:
        while True:
            await stream.send_audio(await audio.get())
    except SpeechToTextError:
        # The stream fails its waiters as well, which is where the error is
        # reported from; this task has nothing more to do.
        pass


async def _capture(
    audio: asyncio.Queue[bytes], io: TurnIO, deps: TurnDeps, timings: _Timings
) -> str:
    """Listen until the request is over and return its transcript."""
    limits = deps.limits
    # The limit runs from the start of the turn, not from here, so that the
    # time taken to reach speech-to-text does not push it past the device's
    # own limit. Audio sent meanwhile waits in the queue.
    deadline = (
        asyncio.get_running_loop().time()
        + limits.max_capture_seconds
        - (time.monotonic() - timings.started)
    )
    stream = await deps.stt.open(CAPTURE_SAMPLE_RATE)
    pump = asyncio.create_task(_pump(audio, stream))
    try:
        # Reaching the limit is not a failure: what was heard is answered.
        with suppress(TimeoutError):
            async with asyncio.timeout_at(deadline):
                heard_speech = True
                try:
                    async with asyncio.timeout(limits.no_speech_timeout_seconds):
                        await stream.speech_started()
                except TimeoutError:
                    heard_speech = False
                if heard_speech:
                    await stream.speech_ended()
        # Reported before the transcript is collected: the device can stop
        # its microphone while speech-to-text finishes.
        timings.capture_done = time.monotonic()
        await io.capture_done()
        return await stream.finish()
    finally:
        pump.cancel()
        with suppress(asyncio.CancelledError):
            await pump
        await stream.aclose()


class _SpokenAudio:
    """Synthesised audio waiting to be sent.

    Synthesis and sending run at different speeds: sending is paced to
    playback, synthesis is several times faster. This sits between them, so
    that the next sentence is being synthesised while the current one is
    still being sent, and no silence falls between sentences.
    """

    def __init__(self, room_bytes: int) -> None:
        self._room_bytes = room_bytes
        self._pieces: deque[bytes] = deque()
        self._bytes = 0
        self._closed = False
        self._changed = asyncio.Condition()

    async def wait_for_room(self) -> None:
        """Return once less than the allowed amount is waiting.

        Checked between sentences, never within one: a sentence, once
        requested, is read to its end, so that the synthesiser is not left
        with a response nobody is reading.
        """
        async with self._changed:
            await self._changed.wait_for(lambda: self._bytes < self._room_bytes)

    async def put(self, piece: bytes) -> None:
        async with self._changed:
            self._pieces.append(piece)
            self._bytes += len(piece)
            self._changed.notify_all()

    async def close(self) -> None:
        """Mark the end: no more audio will be added."""
        async with self._changed:
            self._closed = True
            self._changed.notify_all()

    async def get(self) -> bytes | None:
        """Return the next piece, or None once closed and empty."""
        async with self._changed:
            await self._changed.wait_for(lambda: self._pieces or self._closed)
            if not self._pieces:
                return None
            piece = self._pieces.popleft()
            self._bytes -= len(piece)
            self._changed.notify_all()
            return piece


async def _reply(
    device_id: str,
    transcript: str,
    io: TurnIO,
    deps: TurnDeps,
    timings: _Timings,
) -> None:
    """Generate, speak and record the reply to ``transcript``.

    Three things run at once, each feeding the next: the model writes
    sentences, the synthesiser turns them into audio, and this function
    sends the audio at the pace the connection allows.
    """
    now = deps.now()
    history = await deps.store.history(device_id, now)
    rate = deps.playback_sample_rate

    # None marks the end of the reply.
    sentences: asyncio.Queue[str | None] = asyncio.Queue()
    reply_parts: list[str] = []
    audio = _SpokenAudio(
        int(deps.limits.synthesis_ahead_seconds * rate) * SAMPLE_WIDTH_BYTES
    )
    refused = False
    sent_audio = False

    async def generate() -> None:
        splitter = SentenceSplitter()
        try:
            async for text in deps.llm.stream_reply(history, transcript):
                if timings.first_text is None:
                    timings.first_text = time.monotonic()
                reply_parts.append(text)
                for sentence in splitter.feed(text):
                    sentences.put_nowait(sentence)
            for sentence in splitter.flush():
                sentences.put_nowait(sentence)
        finally:
            sentences.put_nowait(None)

    async def say(sentence: str) -> None:
        await audio.wait_for_room()
        async for piece in deps.tts.synthesize(sentence, rate):
            if timings.first_audio is None:
                timings.first_audio = time.monotonic()
            await audio.put(piece)

    async def synthesise(generator: asyncio.Task[None]) -> None:
        nonlocal refused
        try:
            while (sentence := await sentences.get()) is not None:
                await say(sentence)
            try:
                # Collects the model's outcome, once every sentence it did
                # produce has been synthesised.
                await generator
            except ReplyRefused:
                refused = True
                if not "".join(reply_parts).strip():
                    await say(_REFUSAL_LINE)
        finally:
            # Whatever happened, the sender must not be left waiting.
            await asyncio.shield(audio.close())

    generator = asyncio.create_task(generate())
    synthesiser = asyncio.create_task(synthesise(generator))
    try:
        # The services each have their limits, and the model's client may
        # try twice; together those can exceed what a device waits for. One
        # limit over the whole wait for the first audio keeps the server's
        # failure ahead of the device's own deadline.
        waited = time.monotonic() - (timings.capture_done or time.monotonic())
        try:
            async with asyncio.timeout(
                max(0.0, deps.limits.first_audio_seconds - waited)
            ):
                piece = await audio.get()
        except TimeoutError:
            if reply_parts:
                raise TextToSpeechError("no reply audio in time") from None
            raise LanguageModelError("no reply text in time") from None
        while piece is not None:
            sent_audio = True
            await io.reply_audio(piece)
            piece = await audio.get()
        # Audio made before a failure has been sent by now; the failure
        # itself is raised here.
        await synthesiser
    finally:
        for task in (synthesiser, generator):
            task.cancel()
        for task in (synthesiser, generator):
            with suppress(asyncio.CancelledError, Exception):
                await task

    reply = "".join(reply_parts).strip()
    if not reply and not refused:
        raise LanguageModelError("the model returned an empty reply")
    if not sent_audio:
        raise TextToSpeechError("synthesis produced no audio")
    if deps.log_transcripts:
        log.info("device %s: replied %r", device_id, reply)

    if not refused:
        try:
            await deps.store.append(device_id, Exchange(transcript, reply), now)
        except Exception:
            # The reply has been spoken in full. Losing it from the history
            # is the smaller harm than reporting a failed turn.
            log.exception("device %s: could not record the exchange", device_id)
    await io.reply_done()
