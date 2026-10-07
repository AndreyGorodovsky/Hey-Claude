"""The firmware's copy of the protocol's numbers against the server's.

The device and the server are written in different languages and cannot
share a file, so each has its own list: ``server/protocol.py`` and
``firmware/components/protocol/include/protocol.h``. This test reads the
header and holds the two together, along with the rules about which limit
must outlast which.
"""

import re
from pathlib import Path

from server import protocol

HEADER = (
    Path(__file__).resolve().parents[2]
    / "firmware/components/protocol/include/protocol.h"
)


def defines() -> dict[str, int | str]:
    """Every ``#define NAME value`` in the header, numbers as int."""
    found: dict[str, int | str] = {}
    pattern = re.compile(r'^#define\s+(PROTO_\w+)\s+("[^"]*"|\d+)', re.MULTILINE)
    for name, value in pattern.findall(HEADER.read_text(encoding="utf-8")):
        found[name] = value.strip('"') if value.startswith('"') else int(value)
    return found


D = defines()


def test_the_header_was_read():
    assert len(D) > 20


def test_shared_numbers_agree():
    assert D["PROTO_VERSION"] == protocol.PROTOCOL_VERSION
    assert D["PROTO_STREAM_PATH"] == protocol.STREAM_PATH
    service = f'{D["PROTO_SERVICE_TYPE"]}.{D["PROTO_SERVICE_PROTO"]}.local.'
    assert service == protocol.MDNS_SERVICE_TYPE
    assert D["PROTO_CAPTURE_RATE_HZ"] == protocol.CAPTURE_SAMPLE_RATE
    assert D["PROTO_CAPTURE_FRAME_MS"] == protocol.CAPTURE_FRAME_MS
    assert D["PROTO_REPLY_CHUNK_MS"] == protocol.REPLY_CHUNK_MS
    assert D["PROTO_REPLY_LEAD_MS"] == protocol.REPLY_LEAD_MS
    assert D["PROTO_REPLY_BUFFER_MS"] == protocol.DEVICE_REPLY_BUFFER_MS
    assert D["PROTO_CLOSE_REPLACED"] == protocol.CLOSE_REPLACED
    assert D["PROTO_DEVICE_CAPTURE_LIMIT_MS"] == (
        protocol.DEVICE_CAPTURE_LIMIT_SECONDS * 1000
    )
    assert D["PROTO_DEVICE_THINKING_LIMIT_MS"] == (
        protocol.DEVICE_THINKING_LIMIT_SECONDS * 1000
    )
    assert D["PROTO_DEVICE_STALL_LIMIT_MS"] == (
        protocol.DEVICE_STALL_LIMIT_SECONDS * 1000
    )


def test_a_capture_frame_fits_what_the_server_accepts():
    frame = protocol.frame_bytes(D["PROTO_CAPTURE_RATE_HZ"], D["PROTO_CAPTURE_FRAME_MS"])
    assert frame <= protocol.MAX_CAPTURE_FRAME_BYTES


def test_the_device_has_room_for_more_than_the_server_sends_ahead():
    assert D["PROTO_REPLY_BUFFER_MS"] >= 2 * D["PROTO_REPLY_LEAD_MS"]


def test_the_servers_limits_come_before_the_devices():
    # Thinking: the server answers, with audio or an error, within its
    # limit on the first audio.
    assert protocol.DEVICE_THINKING_LIMIT_SECONDS > protocol.FIRST_AUDIO_LIMIT_SECONDS
    # Mid-reply: a silent model, then a sentence that is slow to synthesise.
    longest_silence = (
        protocol.LLM_SILENCE_SECONDS
        + protocol.SERVICE_CONNECT_SECONDS
        + protocol.TTS_SILENCE_SECONDS
    )
    assert protocol.DEVICE_STALL_LIMIT_SECONDS > longest_silence
    # Capture: the server stops a request before the device's guard does.
    assert protocol.DEVICE_CAPTURE_LIMIT_SECONDS * 1000 == D[
        "PROTO_DEVICE_CAPTURE_LIMIT_MS"
    ]


def test_each_side_notices_a_dead_connection_by_itself():
    assert D["PROTO_DEVICE_PONG_TIMEOUT_S"] > D["PROTO_DEVICE_PING_INTERVAL_S"]
    assert protocol.PING_TIMEOUT_SECONDS >= D["PROTO_DEVICE_PING_INTERVAL_S"]
