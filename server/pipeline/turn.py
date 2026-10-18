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

A request that is only about the volume ("louder", "volume five") skips the
model: the device is sent its new level and a short spoken confirmation, and
nothing is added to the conversation (``volume.py``).

A turn ends early in one of two ways. The person can interrupt it, with the
device's button: the connection sets the turn's ``interrupt`` event, and the
turn stops its work, tells the device at once, and records the request with
the reply as far as it was sent, marked as interrupted. Or the device goes
away: the connection cancels the turn's task, and nothing is recorded. The
turn decides what is recorded in every case, so the stored history always
alternates between the person and the assistant.

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
from typing import Any, Protocol

from server.conversation import INTERRUPTED_MARK, ConversationStore, Exchange
from server.llm import LanguageModel, LanguageModelError, ReplyRefused
from server.pipeline.sentences import SentenceSplitter
from server.pipeline.volume import volume_request
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

    async def set_volume(self, level: int) -> None:
        """The device should play at ``level`` from now on.

        Reported before the reply that confirms it, so that the confirmation
        is heard at the new level.
        """

    async def interrupted(self) -> None:
        """The person interrupted the turn. It is over: nothing more of it
        is reported, though its work may still be stopping."""


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
class _Progress:
    """How far a turn's reply has got, for recording it if it is
    interrupted. Written by the turn's work as it goes, and read by the
    turn once that work has stopped."""

    #: The request, once it is known to be one for the model. Stays None
    #: for a turn interrupted while capturing, and for a volume request,
    #: neither of which is recorded.
    transcript: str | None = None
    #: The time the history was read at, which the record is filed under.
    now: datetime | None = None
    #: The reply's sentences whose audio has begun to be sent, in order.
    #: Sent is not heard: the device may have held up to two seconds of it.
    sent: list[str] = field(default_factory=list)
    #: The model declined; such a reply is never recorded, whole or not.
    refused: bool = False
    #: The whole reply has been sent, and is being recorded whole.
    finished: bool = False


@dataclass
class _Timings:
    """Moments of one turn, on the monotonic clock, for the latency log."""

    started: float
    #: Speech-to-text was reached. Audio sent before this waited in the queue.
    stt_open: float | None = None
    capture_done: float | None = None
    first_text: float | None = None
    #: The model's first whole sentence, which is what synthesis waits for.
    first_sentence: float | None = None
    first_audio: float | None = None


async def run_turn(
    device_id: str,
    audio: asyncio.Queue[bytes],
    io: TurnIO,
    deps: TurnDeps,
    volume: int | None = None,
    interrupt: asyncio.Event | None = None,
) -> None:
    """Run one turn to its end, reporting any failure to the device.

    ``audio`` supplies the device's capture frames, and ``volume`` is the
    playback level the device stated, if it stated one. Setting
    ``interrupt`` ends the turn early, as the person asked. Returns normally
    whatever happened, except when cancelled.
    """
    progress = _Progress()
    work = asyncio.create_task(_work(device_id, audio, io, deps, volume, progress))
    waits: set[asyncio.Task[Any]] = {work}
    if interrupt is not None:
        waits.add(asyncio.create_task(interrupt.wait()))
    try:
        await asyncio.wait(waits, return_when=asyncio.FIRST_COMPLETED)
    except asyncio.CancelledError:
        # The device has gone. The work stops, and nothing is recorded.
        work.cancel()
        await asyncio.wait({work})
        raise
    finally:
        for task in waits - {work}:
            task.cancel()
    if work.done():
        work.result()   # nothing: the work ends quietly whatever happens
    else:
        await _interrupted(device_id, work, io, deps, progress)


async def _interrupted(
    device_id: str,
    work: asyncio.Task[None],
    io: TurnIO,
    deps: TurnDeps,
    progress: _Progress,
) -> None:
    """End a turn the person interrupted, and record it as far as it got."""
    log.info("device %s: turn interrupted by the person", device_id)
    work.cancel()
    # Told before the work has finished stopping: stopping Claude can take
    # a while, and the device allows a few seconds (DEVICE_CANCEL_LIMIT_SECONDS)
    with suppress(ConnectionLost):
        await io.interrupted()
    await asyncio.wait({work})
    if progress.transcript is None or progress.now is None:
        return
    if progress.refused or progress.finished:
        # Refused: never recorded. Finished: recorded whole by the work,
        # since a write already under way runs to its end in its thread.
        return
    reply = " ".join([*progress.sent, INTERRUPTED_MARK])
    try:
        await deps.store.append(
            device_id, Exchange(progress.transcript, reply), progress.now
        )
    except Exception:
        # As for a completed turn: the history is worth less than the turn
        log.exception(
            "device %s: could not record the interrupted exchange", device_id
        )


async def _work(
    device_id: str,
    audio: asyncio.Queue[bytes],
    io: TurnIO,
    deps: TurnDeps,
    volume: int | None,
    progress: _Progress,
) -> None:
    """The turn itself: capture, then the reply. Ends quietly whatever
    happens, except when cancelled."""
    timings = _Timings(started=time.monotonic())
    try:
        transcript = await _capture(audio, io, deps, timings)
        if deps.log_transcripts:
            log.info("device %s: heard %r", device_id, transcript)
        if not transcript:
            # The wake word fired and nothing was said.
            await io.reply_done()
            return
        change = volume_request(transcript, volume)
        if change is not None:
            # The commands as understood, which explain a level the person
            # did not expect; the words themselves are logged above, only
            # when transcripts are.
            log.info(
                "device %s: volume %s to %d, understood as %s",
                device_id,
                volume,
                change.level,
                change.understood,
            )
            if change.level != volume:
                await io.set_volume(change.level)
            await _speak(change.reply, io, deps, timings)
            return
        await _reply(device_id, transcript, io, deps, timings, progress)
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
    finally:
        # Whatever the outcome: the turns that fail or run long are the
        # ones whose timings are wanted most.
        _log_timings(device_id, timings)


async def _fail(io: TurnIO, code: ErrorCode, message: str) -> None:
    # The device may be the thing that failed; a turn must still end quietly.
    with suppress(ConnectionLost):
        await io.fail(code, message)


def _log_timings(device_id: str, timings: _Timings) -> None:
    """Log where a turn's time went, as far as the turn got.

    The last three figures are each counted from the end of speech, not
    from the one before: the last is the whole wait for the reply, and the
    differences are the model's share, the rest of its first sentence, and
    the synthesiser's. A mark the turn never reached is shown as "-".
    """
    if timings.stt_open is None or timings.capture_done is None:
        # The turn ended before a request was heard: nothing to time.
        return

    def after_speech(mark: float | None) -> str:
        return "-" if mark is None else f"{mark - timings.capture_done:.2f} s"

    log.info(
        "device %s: speech-to-text reached in %.2f s, capture %.2f s; from end"
        " of speech, first text %s, first sentence %s, first audio %s",
        device_id,
        timings.stt_open - timings.started,
        timings.capture_done - timings.started,
        after_speech(timings.first_text),
        after_speech(timings.first_sentence),
        after_speech(timings.first_audio),
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
    timings.stt_open = time.monotonic()
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
        try:
            # Waited for, not awaited: awaiting it with CancelledError
            # suppressed would also swallow a cancellation of this turn
            # that arrived meanwhile, and the turn would carry on.
            await asyncio.wait({pump})
        finally:
            await stream.aclose()


async def _speak(line: str, io: TurnIO, deps: TurnDeps, timings: _Timings) -> None:
    """Say one fixed sentence as the whole reply, with no model involved.

    Under the same limit on the wait for the first audio as a reply from the
    model (``_reply``), so that the server's failure comes before the
    device's own deadline.
    """
    waited = time.monotonic() - (timings.capture_done or time.monotonic())
    try:
        async with asyncio.timeout(
            max(0.0, deps.limits.first_audio_seconds - waited)
        ) as limit:
            async for piece in deps.tts.synthesize(line, deps.playback_sample_rate):
                if timings.first_audio is None:
                    timings.first_audio = time.monotonic()
                    # The limit is on the wait for the first audio only
                    limit.reschedule(None)
                await io.reply_audio(piece)
    except TimeoutError:
        raise TextToSpeechError("no reply audio in time") from None
    if timings.first_audio is None:
        raise TextToSpeechError("synthesis produced no audio")
    await io.reply_done()


class _SpokenAudio:
    """Synthesised audio waiting to be sent.

    Synthesis and sending run at different speeds: sending is paced to
    playback, synthesis is several times faster. This sits between them, so
    that the next sentence is being synthesised while the current one is
    still being sent, and no silence falls between sentences.

    Each piece carries the number of the sentence it belongs to, so that
    the sender knows which sentences it has begun to send.
    """

    def __init__(self, room_bytes: int) -> None:
        self._room_bytes = room_bytes
        self._pieces: deque[tuple[int, bytes]] = deque()
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

    async def put(self, sentence: int, piece: bytes) -> None:
        """Add a piece of the audio of sentence number ``sentence``."""
        async with self._changed:
            self._pieces.append((sentence, piece))
            self._bytes += len(piece)
            self._changed.notify_all()

    async def close(self) -> None:
        """Mark the end: no more audio will be added."""
        async with self._changed:
            self._closed = True
            self._changed.notify_all()

    async def get(self) -> tuple[int, bytes] | None:
        """Return the next piece, with its sentence's number, or None once
        closed and empty."""
        async with self._changed:
            await self._changed.wait_for(lambda: self._pieces or self._closed)
            if not self._pieces:
                return None
            item = self._pieces.popleft()
            self._bytes -= len(item[1])
            self._changed.notify_all()
            return item


async def _reply(
    device_id: str,
    transcript: str,
    io: TurnIO,
    deps: TurnDeps,
    timings: _Timings,
    progress: _Progress,
) -> None:
    """Generate, speak and record the reply to ``transcript``.

    Three things run at once, each feeding the next: the model writes
    sentences, the synthesiser turns them into audio, and this function
    sends the audio at the pace the connection allows.
    """
    now = deps.now()
    progress.transcript, progress.now = transcript, now
    history = await deps.store.history(device_id, now)
    rate = deps.playback_sample_rate

    # None marks the end of the reply.
    sentences: asyncio.Queue[str | None] = asyncio.Queue()
    reply_parts: list[str] = []
    audio = _SpokenAudio(
        int(deps.limits.synthesis_ahead_seconds * rate) * SAMPLE_WIDTH_BYTES
    )
    # Every sentence given to the synthesiser, by number
    said: list[str] = []
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
        if timings.first_sentence is None:
            timings.first_sentence = time.monotonic()
        await audio.wait_for_room()
        number = len(said)
        said.append(sentence)
        async for piece in deps.tts.synthesize(sentence, rate):
            if timings.first_audio is None:
                timings.first_audio = time.monotonic()
            await audio.put(number, piece)

    async def synthesise(generator: asyncio.Task[None]) -> None:
        try:
            while (sentence := await sentences.get()) is not None:
                await say(sentence)
            try:
                # Collects the model's outcome, once every sentence it did
                # produce has been synthesised.
                await generator
            except ReplyRefused:
                progress.refused = True
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
                item = await audio.get()
        except TimeoutError:
            if reply_parts:
                raise TextToSpeechError("no reply audio in time") from None
            raise LanguageModelError("no reply text in time") from None
        while item is not None:
            number, piece = item
            if number == len(progress.sent):
                # The first audio of the next sentence
                progress.sent.append(said[number])
            sent_audio = True
            await io.reply_audio(piece)
            item = await audio.get()
        # Audio made before a failure has been sent by now; the failure
        # itself is raised here.
        await synthesiser
    finally:
        for task in (synthesiser, generator):
            task.cancel()
        # Waited for, not awaited, for the reason given in _capture. Their
        # outcomes are collected so that none is reported as never
        # retrieved: a failure has been raised above already, or is of no
        # account after the one that ended the reply.
        await asyncio.wait({synthesiser, generator})
        for task in (synthesiser, generator):
            if not task.cancelled():
                task.exception()

    reply = "".join(reply_parts).strip()
    if not reply and not progress.refused:
        raise LanguageModelError("the model returned an empty reply")
    if not sent_audio:
        raise TextToSpeechError("synthesis produced no audio")
    if deps.log_transcripts:
        log.info("device %s: replied %r", device_id, reply)

    # Sent in full: from here an interruption leaves the record to this
    progress.finished = True
    if not progress.refused:
        try:
            await deps.store.append(device_id, Exchange(transcript, reply), now)
        except Exception:
            # The reply has been spoken in full. Losing it from the history
            # is the smaller harm than reporting a failed turn.
            log.exception("device %s: could not record the exchange", device_id)
    await io.reply_done()
