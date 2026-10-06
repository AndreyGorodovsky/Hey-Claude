"""One connected device.

A ``DeviceConnection`` owns a device's WebSocket for as long as it is open.
It reads what the device sends, starts a turn on ``utterance_start``, and
passes capture audio to that turn. In the other direction it is the turn's
``TurnIO``: it turns each step the turn reports into protocol messages, and
cuts reply audio into chunks of the protocol's size.

Two facts are all the state there is. A turn is running or it is not: a
request is accepted only when none is, so a turn that has started always
runs to its end. And within a turn the capture is open or it is not: audio
is accepted only while it is, so frames already on their way when
``stop_capture`` was sent are dropped.
"""

from __future__ import annotations

import asyncio
import logging
from contextlib import suppress

from starlette.websockets import WebSocket, WebSocketDisconnect

from server.pipeline import ConnectionLost, TurnDeps, run_turn
from server.protocol import (
    CAPTURE_FRAME_MS,
    CLOSE_REPLACED,
    MAX_CAPTURE_FRAME_BYTES,
    REPLY_CHUNK_MS,
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
    """Cuts reply audio of any piece size into chunks of one fixed size."""

    def __init__(self, chunk_bytes: int) -> None:
        self._chunk_bytes = chunk_bytes
        self._pending = bytearray()

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
        await self._send(StopCapture())

    async def reply_audio(self, pcm: bytes) -> None:
        if self._reply is None:
            # Announced only once there is audio to send, so a reply that
            # fails before its first sound never starts playback.
            rate = self._deps.playback_sample_rate
            self._reply = _ReplyFramer(frame_bytes(rate, REPLY_CHUNK_MS))
            await self._send(ReplyStart(sample_rate=rate))
        for chunk in self._reply.feed(pcm):
            await self._send(chunk)

    async def reply_done(self) -> None:
        if self._reply is not None:
            rest = self._reply.flush()
            if rest:
                await self._send(rest)
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
