import json
import logging

import pytest
from pydantic import SecretStr, ValidationError

from server.config import RedactingFormatter, Settings
from server.conversation import Exchange
from server.llm.claude import build_messages
from server.protocol import (
    Error,
    ErrorCode,
    ProtocolError,
    Ready,
    ReplyStart,
    UtteranceStart,
    encode,
    frame_bytes,
    parse_device_message,
)
from server.stt import EndpointTracker
from server.transport.auth import authenticate

TOKENS = {"kitchen": SecretStr("a" * 32)}


def test_utterance_start_parses_and_ignores_unknown_fields():
    message = parse_device_message('{"type": "utterance_start", "later": 1}')
    assert isinstance(message, UtteranceStart)


@pytest.mark.parametrize(
    "text", ["not json", "[]", "{}", '{"type": "reply_end"}', '{"type": 5}']
)
def test_bad_device_messages_are_refused(text):
    with pytest.raises(ProtocolError):
        parse_device_message(text)


def test_server_messages_encode_to_the_documented_shape():
    assert json.loads(encode(Ready())) == {"type": "ready", "protocol": 1}
    assert json.loads(encode(ReplyStart(sample_rate=24000))) == {
        "type": "reply_start",
        "format": "pcm_s16le",
        "sample_rate": 24000,
        "channels": 1,
    }
    assert json.loads(encode(Error(code=ErrorCode.STT_FAILED, message="x"))) == {
        "type": "error",
        "code": "stt_failed",
        "message": "x",
    }


def test_frame_sizes():
    assert frame_bytes(16000, 20) == 640
    assert frame_bytes(24000, 40) == 1920


def test_right_token_authenticates():
    assert authenticate("Bearer " + "a" * 32, "kitchen", TOKENS) == "kitchen"
    assert authenticate("bearer " + "a" * 32, "kitchen", TOKENS) == "kitchen"


@pytest.mark.parametrize(
    ("authorization", "device_id"),
    [
        ("Bearer " + "b" * 32, "kitchen"),  # wrong token
        ("Bearer " + "a" * 32, "garage"),  # unknown device
        ("Bearer " + "-" * 32, "garage"),  # the stand-in for no token
        ("Basic " + "a" * 32, "kitchen"),  # wrong scheme
        ("a" * 32, "kitchen"),
        (None, "kitchen"),
        ("Bearer " + "a" * 32, None),
        ("Bearer ", "kitchen"),
    ],
)
def test_everything_else_is_rejected(authorization, device_id):
    assert authenticate(authorization, device_id, TOKENS) is None


def test_request_puts_history_first_and_caches_up_to_the_new_request():
    messages = build_messages([Exchange("q1", "a1"), Exchange("q2", "a2")], "q3")
    assert [m["role"] for m in messages] == [
        "user", "assistant", "user", "assistant", "user",
    ]
    # Stored exchanges are sent as plain text, byte for byte as stored, so
    # the prefix matches what the previous request cached.
    assert messages[0]["content"] == "q1"
    assert messages[3]["content"] == "a2"
    assert messages[-1]["content"] == [
        {"type": "text", "text": "q3", "cache_control": {"type": "ephemeral"}}
    ]


def settings(**values) -> Settings:
    return Settings(
        _env_file=None,
        anthropic_api_key="k" * 30,
        deepgram_api_key="k" * 30,
        **values,
    )


@pytest.mark.parametrize(
    "tokens",
    [
        {"kitchen": "s3cret-value-" + "x" * 30, "new": "too-short-token"},
        {"": "s3cret-value-" + "x" * 30},
        {"d" * 33: "s3cret-value-" + "x" * 30},
        {"kitchen": "s3cret-value-" + "x" * 60},
    ],
)
def test_a_rejected_token_setting_never_shows_a_token(tokens):
    with pytest.raises(ValidationError) as raised:
        settings(device_tokens=tokens)

    shown = str(raised.value) + repr(raised.value)
    assert "s3cret-value" not in shown
    assert "too-short-token" not in shown


def test_the_server_capture_limit_must_stay_below_the_device_limit():
    assert settings().max_capture_seconds < 15
    with pytest.raises(ValidationError):
        settings(max_capture_seconds=15)


def format_record(message: str, *args, exc_info=None) -> str:
    record = logging.LogRecord("t", logging.ERROR, __file__, 1, message, args, exc_info)
    return RedactingFormatter("%(message)s").format(record)


def test_log_redaction_keeps_number_formats():
    assert format_record("took %.2f s on port %d", 1.2345, 8765) == (
        "took 1.23 s on port 8765"
    )


def test_log_redaction_covers_arguments_and_tracebacks():
    secret = "x" * 32
    assert secret not in format_record("header %s %s", "Bearer", secret)
    try:
        raise RuntimeError(f"request failed, Authorization: Token {secret}")
    except RuntimeError:
        import sys

        rendered = format_record("turn failed", exc_info=sys.exc_info())
    assert "turn failed" in rendered and "RuntimeError" in rendered
    assert secret not in rendered


def test_end_of_speech_needs_speech_first_but_not_kept_words():
    silent = EndpointTracker()
    silent.end_of_speech()
    assert not silent.ended

    # Words reported, then withdrawn: the request still ends, empty.
    retracted = EndpointTracker()
    retracted.heard()
    retracted.add_final("")
    retracted.end_of_speech()
    assert retracted.ended
    assert retracted.transcript == ""
