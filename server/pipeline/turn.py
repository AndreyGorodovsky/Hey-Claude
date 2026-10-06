"""One turn: a spoken request in, a spoken reply out.

A turn runs as one task, started when a device sends ``utterance_start``. It
goes through two phases:

1. Capture. Audio from the device is passed to speech-to-text until the
   speaker finishes, nothing is said, or the capture limit is reached. The
   device is then told to stop sending.
2. Reply. The transcript goes to the language model with the day's earlier
   exchanges. The reply is cut into sentences as it is generated, and each
   sentence is synthesised and sent while the next is still being written.

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
from collections.abc import Callable
from contextlib import suppress
from dataclasses import dataclass, field
from datetime import datetime
from typing import Protocol

from server.conversation import ConversationStore, Exchange
from server.llm import LanguageModel, LanguageModelError, ReplyRefused
from server.pipeline.sentences import SentenceSplitter
from server.protocol import CAPTURE_SAMPLE_RATE, ErrorCode
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
        """Reply audio, in pieces of any size, at the playback rate."""

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


async def _reply(
    device_id: str,
    transcript: str,
    io: TurnIO,
    deps: TurnDeps,
    timings: _Timings,
) -> None:
    """Generate, speak and record the reply to ``transcript``."""
    now = deps.now()
    history = await deps.store.history(device_id, now)

    # The model writes into this queue while earlier sentences are being
    # synthesised and sent. None marks the end of the reply.
    sentences: asyncio.Queue[str | None] = asyncio.Queue()
    reply_parts: list[str] = []

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

    sent_audio = False

    async def say(sentence: str) -> None:
        nonlocal sent_audio
        async for piece in deps.tts.synthesize(sentence, deps.playback_sample_rate):
            if not sent_audio:
                sent_audio = True
                timings.first_audio = time.monotonic()
            await io.reply_audio(piece)

    generator = asyncio.create_task(generate())
    refused = False
    try:
        while (sentence := await sentences.get()) is not None:
            await say(sentence)
        try:
            # Collects the model's outcome, once every sentence it did
            # produce has been spoken.
            await generator
        except ReplyRefused:
            refused = True
            if not "".join(reply_parts).strip():
                await say(_REFUSAL_LINE)
    finally:
        generator.cancel()
        with suppress(asyncio.CancelledError, LanguageModelError):
            await generator

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
