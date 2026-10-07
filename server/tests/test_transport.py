"""The WebSocket endpoint, through a test client, with stand-in services."""

import asyncio
import json
import time

import pytest
from fastapi.testclient import TestClient
from pydantic import SecretStr
from starlette.websockets import WebSocketDisconnect

from server.protocol import CLOSE_REPLACED, STREAM_PATH
from server.tests.fakes import FRAME, NOON, FakeLlm, FakeStt, FakeTts, make_deps
from server.transport import create_app

TOKEN = "t" * 32
HEADERS = {"Authorization": f"Bearer {TOKEN}", "X-Device-Id": "dev"}
WHOLE_TURN = ["stop_capture", "reply_start", "reply_end"]


def client_for(deps) -> TestClient:
    return TestClient(create_app(deps, {"dev": SecretStr(TOKEN)}))


def ask(ws, frames: int = 10) -> None:
    ws.send_json({"type": "utterance_start"})
    for _ in range(frames):
        ws.send_bytes(FRAME)


def read_turn(ws) -> tuple[list[str], list[bytes]]:
    """Read to the end of a turn: the message types, and the audio chunks."""
    types, audio = [], []
    while True:
        message = ws.receive()
        if message.get("bytes") is not None:
            audio.append(message["bytes"])
            continue
        kind = json.loads(message["text"])["type"]
        types.append(kind)
        if kind in ("reply_end", "error"):
            return types, audio


def wait_until(condition, timeout: float = 2.0) -> None:
    deadline = time.monotonic() + timeout
    while not condition():
        assert time.monotonic() < deadline, "condition not reached"
        time.sleep(0.01)


def test_health_check(tmp_path):
    with client_for(make_deps(tmp_path)) as client:
        assert client.get("/healthz").json() == {"status": "ok"}


@pytest.mark.parametrize(
    "headers",
    [
        {},
        {"X-Device-Id": "dev"},
        {"Authorization": "Bearer " + "x" * 32, "X-Device-Id": "dev"},
        {"Authorization": f"Bearer {TOKEN}", "X-Device-Id": "other"},
    ],
)
def test_unauthenticated_connections_are_refused(tmp_path, headers):
    with client_for(make_deps(tmp_path)) as client:
        with pytest.raises(WebSocketDisconnect):
            with client.websocket_connect(STREAM_PATH, headers=headers):
                pass


def test_a_whole_turn_over_the_socket(tmp_path):
    tts = FakeTts(bytes_per_character=501)
    with client_for(make_deps(tmp_path, tts=tts)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            assert ws.receive_json() == {"type": "ready", "protocol": 1}
            ask(ws)
            types, audio = read_turn(ws)

    assert types == WHOLE_TURN
    # 40 ms at 24 kHz. Every chunk but the last is exactly that, and across
    # them only a trailing half sample may be lost.
    assert all(len(chunk) == 1920 for chunk in audio[:-1])
    assert 0 < len(audio[-1]) <= 1920
    assert all(len(chunk) % 2 == 0 for chunk in audio)
    synthesised = sum(map(len, tts.spoken)) * 501
    assert sum(map(len, audio)) == synthesised - synthesised % 2


def test_reply_start_states_the_format(tmp_path):
    with client_for(make_deps(tmp_path)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            ws.receive_json()
            ask(ws)
            assert ws.receive_json() == {"type": "stop_capture"}
            assert ws.receive_json() == {
                "type": "reply_start",
                "format": "pcm_s16le",
                "sample_rate": 24000,
                "channels": 1,
            }


def test_two_turns_share_the_conversation(tmp_path):
    llm = FakeLlm()
    with client_for(make_deps(tmp_path, llm=llm)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            ws.receive_json()
            for _ in range(2):
                ask(ws)
                assert read_turn(ws)[0] == WHOLE_TURN

    assert len(llm.calls) == 2
    assert len(llm.calls[1][0]) == 1


def test_a_request_during_a_turn_is_ignored_and_the_turn_finishes(tmp_path):
    stt = FakeStt()
    llm = FakeLlm(gate=asyncio.Event())
    with client_for(make_deps(tmp_path, stt=stt, llm=llm)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            ws.receive_json()
            ask(ws)
            assert ws.receive_json() == {"type": "stop_capture"}
            # The reply is held back, so this arrives mid-turn for certain.
            ask(ws)
            wait_until(lambda: len(llm.calls) == 1)
            client.portal.call(llm.gate.set)
            types, _ = read_turn(ws)

    assert types == ["reply_start", "reply_end"]
    assert len(stt.streams) == 1
    assert len(llm.calls) == 1


def test_audio_is_taken_only_while_capturing(tmp_path):
    stt = FakeStt(end_after=5)
    llm = FakeLlm(gate=asyncio.Event())
    with client_for(make_deps(tmp_path, stt=stt, llm=llm)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            ws.receive_json()
            ws.send_bytes(FRAME)  # before any request
            ask(ws, frames=5)
            assert ws.receive_json() == {"type": "stop_capture"}
            for _ in range(20):  # still in transit when the capture stopped
                ws.send_bytes(FRAME)
            client.portal.call(llm.gate.set)
            read_turn(ws)
            # A second turn starts only after those frames have been read,
            # so by its end they were either dropped or wrongly kept.
            ask(ws, frames=5)
            read_turn(ws)

    assert [len(stream.received) for stream in stt.streams] == [5, 5]


def test_malformed_audio_frames_are_dropped(tmp_path):
    stt = FakeStt(end_after=5)
    with client_for(make_deps(tmp_path, stt=stt)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            ws.receive_json()
            ws.send_json({"type": "utterance_start"})
            ws.send_bytes(bytes(641))  # half a sample
            ws.send_bytes(bytes(64_000))  # far too long
            for _ in range(5):
                ws.send_bytes(FRAME)
            read_turn(ws)

    assert stt.streams[0].received == [FRAME] * 5


def test_a_bad_message_is_not_answered_and_the_connection_stays_open(tmp_path):
    with client_for(make_deps(tmp_path)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            ws.receive_json()
            ws.send_text("nonsense")
            ws.send_text('{"type": "no_such_message"}')
            ask(ws)
            types, _ = read_turn(ws)

    # The first thing to come back is the turn, with no error before it.
    assert types == WHOLE_TURN


def test_a_failed_turn_leaves_the_connection_ready_for_the_next(tmp_path):
    stt = FakeStt(fail_on_open=True)
    with client_for(make_deps(tmp_path, stt=stt)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            ws.receive_json()
            ask(ws)
            assert read_turn(ws)[0] == ["error"]
            stt.fail_on_open = False
            ask(ws)
            assert read_turn(ws)[0] == WHOLE_TURN


def test_a_device_leaving_mid_turn_cancels_the_turn(tmp_path):
    stt = FakeStt()
    llm = FakeLlm(gate=asyncio.Event())
    deps = make_deps(tmp_path, stt=stt, llm=llm)
    with client_for(deps) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            ws.receive_json()
            ask(ws)
            assert ws.receive_json() == {"type": "stop_capture"}
            wait_until(lambda: len(llm.calls) == 1)
        # The device is gone; the reply it was waiting for is never made.
        wait_until(lambda: stt.streams[0].closed)
        client.portal.call(llm.gate.set)
        assert client.portal.call(deps.store.history, "dev", NOON) == []


def test_a_second_connection_replaces_the_first_mid_turn(tmp_path):
    llm = FakeLlm(gate=asyncio.Event())
    with client_for(make_deps(tmp_path, llm=llm)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as first:
            first.receive_json()
            ask(first)
            assert first.receive_json() == {"type": "stop_capture"}
            with client.websocket_connect(STREAM_PATH, headers=HEADERS) as second:
                assert second.receive_json()["type"] == "ready"
                with pytest.raises(WebSocketDisconnect) as closed:
                    first.receive_json()
                assert closed.value.code == CLOSE_REPLACED
                # The newer connection works, and the old turn's reply does
                # not arrive on it.
                client.portal.call(llm.gate.set)
                ask(second)
                types, _ = read_turn(second)

    assert types == WHOLE_TURN


def test_a_long_reply_is_never_sent_more_than_the_lead_ahead(tmp_path):
    # Two sentences of 1.5 s each: three seconds of audio, a two-second lead.
    tts = FakeTts(bytes_per_character=7200)
    llm = FakeLlm(pieces=("Aaaaaaaaa. ", "Bbbbbbbbb."))
    with client_for(make_deps(tmp_path, llm=llm, tts=tts)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            ws.receive_json()
            ask(ws)
            assert ws.receive_json() == {"type": "stop_capture"}
            assert ws.receive_json()["type"] == "reply_start"
            started = time.monotonic()
            received = 0
            furthest_ahead = 0.0
            while True:
                message = ws.receive()
                if message.get("bytes") is None:
                    break
                received += len(message["bytes"])
                ahead = received / 48000 - (time.monotonic() - started)
                furthest_ahead = max(furthest_ahead, ahead)
            took = time.monotonic() - started

    assert received == 20 * 7200
    # All three seconds arrive, the last of them about a second in.
    assert 0.9 < took < 2.0
    assert furthest_ahead < 2.15


def test_a_short_reply_is_not_slowed(tmp_path):
    with client_for(make_deps(tmp_path)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            ws.receive_json()
            started = time.monotonic()
            ask(ws)
            assert read_turn(ws)[0] == WHOLE_TURN

    assert time.monotonic() - started < 0.5


def test_a_pause_in_the_reply_is_not_counted_as_playback(tmp_path):
    # A quarter-second sentence, a pause of 1.2 s, then 3.5 s of audio. The
    # device has played out the first sentence and is silent when the second
    # arrives, so only 2 s of it may come at once. Counting the pause as
    # playback would let 3 s through.
    tts = FakeTts(bytes_per_character=1200, delay=1.2)
    llm = FakeLlm(pieces=("Aaaaaaaaa. ", "B" * 139 + "."))
    with client_for(make_deps(tmp_path, llm=llm, tts=tts)) as client:
        with client.websocket_connect(STREAM_PATH, headers=HEADERS) as ws:
            ws.receive_json()
            ask(ws)
            assert ws.receive_json() == {"type": "stop_capture"}
            assert ws.receive_json()["type"] == "reply_start"
            first = 10 * 1200
            received, resumed, burst = 0, None, 0
            while True:
                message = ws.receive()
                if message.get("bytes") is None:
                    break
                received += len(message["bytes"])
                if received > first:
                    resumed = resumed or time.monotonic()
                    if time.monotonic() - resumed < 0.3:
                        burst = received - first

    assert received == 150 * 1200
    # What arrived in the first moments after the pause: about 2 s, 96000
    # bytes, and well short of the 3.5 s that was ready.
    assert 80_000 < burst < 115_000
