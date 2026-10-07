"""The device protocol: every message that crosses the WebSocket.

This module is the single definition of the protocol described in
ARCHITECTURE.md. Control messages are JSON text frames, modelled below; audio
travels as binary frames of raw little-endian signed 16-bit mono PCM and has
no model.
"""

from __future__ import annotations

import json
from enum import StrEnum
from typing import Literal

from pydantic import BaseModel, ConfigDict, ValidationError

PROTOCOL_VERSION = 1

#: Path of the WebSocket endpoint.
STREAM_PATH = "/v1/stream"

#: mDNS service type under which the server is advertised.
MDNS_SERVICE_TYPE = "_hey-claude._tcp.local."

#: Bytes per sample of 16-bit PCM, in both directions.
SAMPLE_WIDTH_BYTES = 2

#: Rate of the audio a device sends. Fixed by the device's microphone and
#: wake-word model, and so not announced in any message.
CAPTURE_SAMPLE_RATE = 16_000

#: Length of the audio frames a device sends while capturing.
CAPTURE_FRAME_MS = 20

#: Largest binary frame accepted from a device: two capture frames.
MAX_CAPTURE_FRAME_BYTES = 1280

#: The device stops capturing by itself after this long, as a guard against
#: a server that never answers. The server's own limit is set below it.
DEVICE_CAPTURE_LIMIT_SECONDS = 15

#: Length of the reply audio chunks the server sends, at the rate announced
#: in ``reply_start``. The last chunk of a reply may be shorter.
REPLY_CHUNK_MS = 40

#: The server sends reply audio at most this far ahead of where playback
#: has reached, counting from the first chunk and assuming the device plays
#: at the stated rate from then on.
REPLY_LEAD_MS = 2000

#: Reply audio a device must be able to hold: the lead, with as much again
#: for a stall on the network or a late start to playback.
DEVICE_REPLY_BUFFER_MS = 4000

# --- Time limits ------------------------------------------------------------
#
# The server's limits on its own services, and the device's deadlines that
# must outlast them, so that the server's ``error`` message reaches a device
# before the device gives up by itself. The firmware holds the same numbers
# in firmware/components/protocol/include/protocol.h; a test compares the
# two files and checks the ordering.

#: Seconds allowed to connect to a service.
SERVICE_CONNECT_SECONDS = 5

#: Seconds of silence tolerated from Claude while a reply is being generated.
LLM_SILENCE_SECONDS = 20

#: Seconds of silence tolerated from text-to-speech during one sentence.
TTS_SILENCE_SECONDS = 10

#: Longest the server takes, from ``stop_capture``, to send the first reply
#: audio or an ``error``. One limit over the whole wait, whatever the
#: services' own limits and retries add up to.
FIRST_AUDIO_LIMIT_SECONDS = 30

#: How long a device waits in its thinking state before giving up.
DEVICE_THINKING_LIMIT_SECONDS = 40

#: How long a device lets a reply stay silent, once begun, before giving up.
DEVICE_STALL_LIMIT_SECONDS = 40

#: Longest device identifier and token, as the firmware stores them.
MAX_DEVICE_ID_LENGTH = 32
MAX_DEVICE_TOKEN_LENGTH = 64

#: Seconds between the server's WebSocket pings, and how long it waits for
#: the pong before dropping the connection.
PING_INTERVAL_SECONDS = 20
PING_TIMEOUT_SECONDS = 20

#: WebSocket close code sent to a connection whose device has connected
#: again; the newer connection takes over.
CLOSE_REPLACED = 4000


class ErrorCode(StrEnum):
    """Why a turn was abandoned. Sent to the device in an ``error`` message."""

    STT_FAILED = "stt_failed"
    LLM_FAILED = "llm_failed"
    TTS_FAILED = "tts_failed"
    INTERNAL = "internal"


class _Message(BaseModel):
    # Unknown fields are ignored, so that a newer peer can add one without
    # breaking an older one.
    model_config = ConfigDict(extra="ignore", frozen=True)


# --- Device to server ------------------------------------------------------


class UtteranceStart(_Message):
    """The wake word fired; audio frames follow."""

    type: Literal["utterance_start"] = "utterance_start"


DeviceMessage = UtteranceStart

_DEVICE_MESSAGES: dict[str, type[_Message]] = {
    "utterance_start": UtteranceStart,
}


# --- Server to device ------------------------------------------------------


class Ready(_Message):
    """The session is open and a request can be made."""

    type: Literal["ready"] = "ready"
    protocol: int = PROTOCOL_VERSION


class StopCapture(_Message):
    """The speaker has finished; stop sending audio."""

    type: Literal["stop_capture"] = "stop_capture"


class ReplyStart(_Message):
    """Reply audio follows, in the stated format."""

    type: Literal["reply_start"] = "reply_start"
    format: Literal["pcm_s16le"] = "pcm_s16le"
    sample_rate: int
    channels: Literal[1] = 1


class ReplyEnd(_Message):
    """The turn is over. Sent with no ``reply_start`` when nothing was said."""

    type: Literal["reply_end"] = "reply_end"


class Error(_Message):
    """The turn was abandoned. The connection stays open and ready."""

    type: Literal["error"] = "error"
    code: ErrorCode
    message: str


ServerMessage = Ready | StopCapture | ReplyStart | ReplyEnd | Error


class ProtocolError(ValueError):
    """A text frame from a device was not a valid protocol message."""


def parse_device_message(text: str) -> DeviceMessage:
    """Parse a text frame received from a device.

    Raises ``ProtocolError`` for anything that is not a known message.
    """
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ProtocolError("message is not JSON") from exc
    if not isinstance(data, dict):
        raise ProtocolError("message is not a JSON object")
    model = _DEVICE_MESSAGES.get(data.get("type"))
    if model is None:
        raise ProtocolError("unknown message type")
    try:
        return model.model_validate(data)
    except ValidationError as exc:
        raise ProtocolError("message has invalid fields") from exc


def encode(message: ServerMessage) -> str:
    """Serialise a message for sending as a text frame."""
    return message.model_dump_json()


def frame_bytes(sample_rate: int, milliseconds: int) -> int:
    """The size in bytes of ``milliseconds`` of PCM at ``sample_rate``."""
    return sample_rate * milliseconds // 1000 * SAMPLE_WIDTH_BYTES
