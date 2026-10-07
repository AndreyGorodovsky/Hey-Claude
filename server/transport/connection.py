"""One connected device.

A ``DeviceConnection`` owns a device's WebSocket for as long as it is open.
It reads what the device sends, starts a turn on ``utterance_start``, and
passes capture audio to that turn. In the other direction it is the turn's
``TurnIO``: it turns each step the turn reports into protocol messages, cuts
reply audio into chunks of the protocol's size, and paces them so that the
device is never sent more than it has room for.

Two facts are all the state there is. A turn is running or it is not: a
request is accepted only when none is, so a turn that has started always
runs to its end. And within a turn the capture is open or it is not: audio
is accepted only while it is, so frames already on their way when
``stop_capture`` was sent are dropped.
"""

from __future__ import annotations

import asyncio
import logging
import math
from array import array
from contextlib import suppress

from starlette.websockets import WebSocket, WebSocketDisconnect

from server.pipeline import ConnectionLost, TurnDeps, run_turn
from server.protocol import (
    CAPTURE_FRAME_MS,
    CLOSE_REPLACED,
    MAX_CAPTURE_FRAME_BYTES,
    REPLY_CHUNK_MS,
    REPLY_LEAD_MS,
    SAMPLE_WIDTH_BYTES,
    Error,
    ErrorCode,
    ProtocolError,
    Ready,
    ReplyEnd,
    ReplyStart,
    ServerMessage,
    StopCapture,
    UtteranceStart,
    encode,
    frame_bytes,
    parse_device_message,
)

log = logging.getLogger(__name__)

#: Capture frames a turn may fall behind before more are dropped: five
#: seconds of them at the protocol's frame length.
_AUDIO_BACKLOG_FRAMES = 5 * 1000 // CAPTURE_FRAME_MS


class _ReplyFramer:
    """Cuts reply audio of any piece size into chunks of one fixed size,

    and keeps count of how far ahead of playback the chunks sent so far run.
    """

    def __init__(self, sample_rate: int) -> None:
        self._chunk_bytes = frame_bytes(sample_rate, REPLY_CHUNK_MS)
        self._bytes_per_second = sample_rate * SAMPLE_WIDTH_BYTES
        self._pending = bytearray()
        self._sent_bytes = 0
        self._started: float | None = None

    def sent(self, chunk: bytes) -> float:
        """Note a chunk as sent; return the seconds to wait before the next.

        The device is taken to start playing at the first chunk and to play
        whatever it holds without pause. Playback has then reached the time
        since that chunk, and anything sent beyond it is still in the
        device's buffer.

        Playback cannot run ahead of what was sent, though. When the reply
        itself pauses, between a short sentence and a slow one, the device
        plays out what it has and waits in silence. That silence is not
        playback: counting it would let the next sentence be sent as far
        ahead again as the pause was long, into a buffer with no room for
        it. So when a chunk is sent to a device that has run dry, the start
        of playback is moved up to now.
        """
        now = asyncio.get_running_loop().time()
        already_sent = self._sent_bytes / self._bytes_per_second
        if self._started is None or now - self._started > already_sent:
            self._started = now - already_sent
        self._sent_bytes += len(chunk)
        ahead = self._sent_bytes / self._bytes_per_second - (now - self._started)
        return max(0.0, ahead - REPLY_LEAD_MS / 1000)

    def feed(self, data: bytes) -> list[bytes]:
        self._pending += data
        chunks = []
        while len(self._pending) >= self._chunk_bytes:
            chunks.append(bytes(self._pending[: self._chunk_bytes]))
            del self._pending[: self._chunk_bytes]
        return chunks

    def flush(self) -> bytes:
        """Return what is left, less any half sample."""
        whole = len(self._pending) - len(self._pending) % SAMPLE_WIDTH_BYTES
        rest = bytes(self._pending[:whole])
        self._pending.clear()
        return rest


class _Level:
    """The loudness of one request's audio, worked out as it arrives.

    Only two numbers are kept, never the audio. They answer a question that
    a bad transcript raises: did the microphone deliver speech at a usable
    level, or something too quiet or clipped to recognise? Logged for every
    request, because the right microphone gain is still an open question
    (KNOWN-ISSUES R12) and these are the figures that will settle it.
    """

    def __init__(self) -> None:
        self._peak = 0
        self._squares = 0
        self._samples = 0

    def add(self, pcm: bytes) -> None:
        samples = array("h", pcm)
        self._peak = max(self._peak, max(map(abs, samples), default=0))
        self._squares += sum(sample * sample for sample in samples)
        self._samples += len(samples)

    def describe(self) -> str:
        """Peak and average level in dBFS: decibels below full scale, where

        0 is the loudest a sample can be and each 6 less is half as loud.
        """
        if not self._samples or not self._peak:
            return "no signal"
        peak = 20 * math.log10(self._peak / 32768)
        rms = 10 * math.log10(self._squares / self._samples / 32768**2)
        return f"peak {peak:.0f} dBFS, average {rms:.0f} dBFS"


class DeviceConnection:
    """The server's side of one device's connection."""

    def __init__(self, websocket: WebSocket, device_id: str, deps: TurnDeps) -> None:
        self._websocket = websocket
        self._device_id = device_id
        self._deps = deps
        #: The running turn, or None when a request can be accepted.
        self._turn: asyncio.Task[None] | None = None
        #: Where capture audio goes, or None when it is not wanted.
        self._capture: asyncio.Queue[bytes] | None = None
        self._dropped_frames = 0
        self._level = _Level()
        #: The reply being sent, or None before its first audio.
        self._reply: _ReplyFramer | None = None
        # The receive loop and the turn task both send; a lock keeps their
        # frames from interleaving.
        self._send_lock = asyncio.Lock()

    # --- Sending -----------------------------------------------------------

    async def _send(self, payload: ServerMessage | bytes) -> None:
        try:
            async with self._send_lock:
                if isinstance(payload, bytes):
                    await self._websocket.send_bytes(payload)
                else:
                    await self._websocket.send_text(encode(payload))
        except (WebSocketDisconnect, RuntimeError):
            # Starlette raises RuntimeError for a send on a connection that
            # has already closed.
            raise ConnectionLost from None

    # --- TurnIO: what the running turn reports ------------------------------

    async def capture_done(self) -> None:
        # From here on, audio still in transit is no longer wanted.
        self._capture = None
        log.info(
            "device %s: request audio %s", self._device_id, self._level.describe()
        )
        await self._send(StopCapture())

    async def reply_audio(self, pcm: bytes) -> None:
        if self._reply is None:
            # Announced only once there is audio to send, so a reply that
            # fails before its first sound never starts playback.
            rate = self._deps.playback_sample_rate
            self._reply = _ReplyFramer(rate)
            await self._send(ReplyStart(sample_rate=rate))
        for chunk in self._reply.feed(pcm):
            await self._send_reply_chunk(chunk)

    async def _send_reply_chunk(self, chunk: bytes) -> None:
        assert self._reply is not None
        await self._send(chunk)
        wait = self._reply.sent(chunk)
        if wait:
            # Holding back here holds the turn back with it, which is the
            # point: nothing queues up in the server either.
            await asyncio.sleep(wait)

    async def reply_done(self) -> None:
        if self._reply is not None:
            rest = self._reply.flush()
            if rest:
                await self._send_reply_chunk(rest)
        # Sent as soon as the last audio is: the device still has up to the
        # lead to play, and returns to idle when it has.
        await self._send(ReplyEnd())

    async def fail(self, code: ErrorCode, message: str) -> None:
        await self._send(Error(code=code, message=message))

    # --- The connection ----------------------------------------------------

    async def run(self) -> None:
        """Serve the device until it disconnects."""
        try:
            await self._send(Ready())
            while True:
                message = await self._websocket.receive()
                if message["type"] == "websocket.disconnect":
                    break
                if message.get("bytes") is not None:
                    self._on_audio(message["bytes"])
                elif message.get("text") is not None:
                    await self._on_text(message["text"])
        except (ConnectionLost, WebSocketDisconnect, RuntimeError):
            pass
        finally:
            await self._cancel_turn()

    async def close_replaced(self) -> None:
        """Close the connection because the device has connected again."""
        with suppress(WebSocketDisconnect, RuntimeError):
            await self._websocket.close(code=CLOSE_REPLACED)

    async def _on_text(self, text: str) -> None:
        try:
            message = parse_device_message(text)
        except ProtocolError as exc:
            # Logged and not answered. An error message means a turn was
            # abandoned, and nothing was: any running turn carries on.
            log.warning("device %s: bad message: %s", self._device_id, exc)
            return
        if isinstance(message, UtteranceStart):
            self._on_utterance_start()

    def _on_utterance_start(self) -> None:
        if self._turn is not None:
            # A correct device asks only when idle. The turn in progress is
            # left alone and this request gets no answer. If that turn is
            # still capturing, the audio that follows simply continues it.
            log.warning(
                "device %s: utterance_start ignored during a turn", self._device_id
            )
            return
        self._capture = asyncio.Queue(maxsize=_AUDIO_BACKLOG_FRAMES)
        self._dropped_frames = 0
        self._level = _Level()
        self._reply = None
        self._turn = asyncio.create_task(self._run_turn(self._capture))

    def _on_audio(self, pcm: bytes) -> None:
        if self._capture is None:
            return
        if len(pcm) % SAMPLE_WIDTH_BYTES or len(pcm) > MAX_CAPTURE_FRAME_BYTES:
            log.warning(
                "device %s: dropped a malformed audio frame of %d bytes",
                self._device_id,
                len(pcm),
            )
            return
        self._level.add(pcm)
        try:
            self._capture.put_nowait(pcm)
        except asyncio.QueueFull:
            # Speech-to-text is not keeping up. Dropping keeps memory
            # bounded; one warning per turn is enough to explain a bad
            # transcript.
            if self._dropped_frames == 0:
                log.warning("device %s: capture audio is backing up", self._device_id)
            self._dropped_frames += 1

    async def _run_turn(self, audio: asyncio.Queue[bytes]) -> None:
        try:
            await run_turn(self._device_id, audio, self, self._deps)
        finally:
            self._capture = None
            self._turn = None

    async def _cancel_turn(self) -> None:
        turn = self._turn
        if turn is None:
            return
        turn.cancel()
        with suppress(asyncio.CancelledError):
            await turn


class ConnectionRegistry:
    """The live connection of each device: at most one."""

    def __init__(self) -> None:
        self._connections: dict[str, DeviceConnection] = {}

    async def claim(self, device_id: str, connection: DeviceConnection) -> None:
        """Make ``connection`` the device's own, closing any earlier one.

        The newer connection wins: a device that lost power or WiFi comes
        back before the server has noticed the old connection is dead, and
        must not be locked out by it.
        """
        previous = self._connections.get(device_id)
        self._connections[device_id] = connection
        if previous is not None:
            log.info("device %s: connected again, closing the older one", device_id)
            await previous.close_replaced()

    def release(self, device_id: str, connection: DeviceConnection) -> None:
        """Forget ``connection``, unless a newer one has already replaced it."""
        if self._connections.get(device_id) is connection:
            del self._connections[device_id]
