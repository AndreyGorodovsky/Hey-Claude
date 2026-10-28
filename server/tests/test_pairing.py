"""Pairing: the code, its limits, and the token it is traded for."""

import time

import pytest
from fastapi.testclient import TestClient
from pydantic import SecretStr
from starlette.websockets import WebSocketDisconnect

from server.pairing import PairedDevices
from server.protocol import (
    PAIR_CODE_LIFETIME_SECONDS,
    PAIR_CODE_TRIES,
    PAIR_PATH,
    STREAM_PATH,
)
from server.tests.fakes import make_deps
from server.transport import create_app
from server.transport.auth import authenticate

NOW = 1_000_000.0
OLD_TOKEN = "o" * 32


@pytest.fixture
def registry(tmp_path) -> PairedDevices:
    registry = PairedDevices(tmp_path / "devices.sqlite3")
    registry.initialise()
    return registry


def wrong(code: str) -> str:
    return f"{(int(code) + 1) % 1_000_000:06d}"


def bearer(token: str) -> str:
    return f"Bearer {token}"


def test_a_code_is_six_digits_and_works_once(registry):
    code = registry.new_code(NOW)
    assert len(code) == 6 and code.isdigit()
    token = registry.pair("dev", code, NOW)
    assert token is not None and 24 <= len(token) <= 64
    assert registry.pair("dev", code, NOW) is None


def test_a_code_expires(registry):
    code = registry.new_code(NOW)
    assert registry.pair("dev", code, NOW + PAIR_CODE_LIFETIME_SECONDS) is None


def test_wrong_codes_use_up_the_tries(registry):
    code = registry.new_code(NOW)
    for _ in range(PAIR_CODE_TRIES - 1):
        assert registry.pair("dev", wrong(code), NOW) is None
    assert registry.pair("dev", code, NOW) is not None

    code = registry.new_code(NOW)
    for _ in range(PAIR_CODE_TRIES):
        assert registry.pair("dev", wrong(code), NOW) is None
    assert registry.pair("dev", code, NOW) is None


def test_a_new_code_replaces_the_old(registry):
    first = registry.new_code(NOW)
    second = registry.new_code(NOW)
    if first != second:
        assert registry.pair("dev", first, NOW) is None
    assert registry.pair("dev", second, NOW) is not None


def test_only_hashes_are_stored(registry, tmp_path):
    code = registry.new_code(NOW)
    token = registry.pair("dev", code, NOW)
    data = (tmp_path / "devices.sqlite3").read_bytes()
    assert token.encode() not in data and code.encode() not in data


def test_a_paired_token_replaces_the_device_tokens_entry(registry):
    tokens = {"dev": SecretStr(OLD_TOKEN), "other": SecretStr(OLD_TOKEN)}
    assert authenticate(bearer(OLD_TOKEN), "dev", tokens, registry) == "dev"
    token = registry.pair("dev", registry.new_code(NOW), NOW)
    assert authenticate(bearer(token), "dev", tokens, registry) == "dev"
    assert authenticate(bearer(OLD_TOKEN), "dev", tokens, registry) is None
    # Another device's entry is untouched.
    assert authenticate(bearer(OLD_TOKEN), "other", tokens, registry) == "other"
    # Pairing again replaces the first token.
    again = registry.pair("dev", registry.new_code(NOW), NOW)
    assert authenticate(bearer(token), "dev", tokens, registry) is None
    assert authenticate(bearer(again), "dev", tokens, registry) == "dev"


def test_unpairing_withdraws_the_token(registry):
    tokens = {"dev": SecretStr(OLD_TOKEN)}
    token = registry.pair("dev", registry.new_code(NOW), NOW)
    assert registry.device_ids() == ["dev"]
    assert registry.unpair("dev") and not registry.unpair("dev")
    assert registry.device_ids() == []
    assert authenticate(bearer(token), "dev", tokens, registry) is None
    # The DEVICE_TOKENS entry is back in use.
    assert authenticate(bearer(OLD_TOKEN), "dev", tokens, registry) == "dev"


def test_the_endpoint_pairs_and_the_token_connects(registry, tmp_path):
    app = create_app(make_deps(tmp_path), {}, paired=registry)
    with TestClient(app) as client:
        code = registry.new_code(time.time())
        answer = client.post(PAIR_PATH, json={"device_id": "dev", "code": code})
        assert answer.status_code == 200
        token = answer.json()["token"]
        headers = {"Authorization": bearer(token), "X-Device-Id": "dev"}
        with client.websocket_connect(STREAM_PATH, headers=headers) as ws:
            assert ws.receive_json()["type"] == "ready"
        # The code is spent.
        again = client.post(PAIR_PATH, json={"device_id": "dev", "code": code})
        assert again.status_code == 403


@pytest.mark.parametrize(
    "body",
    [
        {"device_id": "dev"},
        {"device_id": "dev", "code": "12345"},
        {"device_id": "dev", "code": "12345a"},
        {"device_id": "", "code": "123456"},
        {"device_id": "d" * 33, "code": "123456"},
        {"device_id": 5, "code": "123456"},
        [],
    ],
)
def test_the_endpoint_refuses_malformed_requests(registry, tmp_path, body):
    app = create_app(make_deps(tmp_path), {}, paired=registry)
    with TestClient(app) as client:
        assert client.post(PAIR_PATH, json=body).status_code == 400


def test_the_endpoint_refuses_a_large_body(registry, tmp_path):
    app = create_app(make_deps(tmp_path), {}, paired=registry)
    with TestClient(app) as client:
        body = {"device_id": "dev", "code": "123456", "pad": "x" * 1000}
        assert client.post(PAIR_PATH, json=body).status_code == 413


def test_a_wrong_code_is_refused_and_connects_nothing(registry, tmp_path):
    app = create_app(make_deps(tmp_path), {}, paired=registry)
    with TestClient(app) as client:
        code = registry.new_code(time.time())
        answer = client.post(PAIR_PATH, json={"device_id": "dev", "code": wrong(code)})
        assert answer.status_code == 403 and "token" not in answer.json()
        headers = {"Authorization": bearer(OLD_TOKEN), "X-Device-Id": "dev"}
        with pytest.raises(WebSocketDisconnect):
            with client.websocket_connect(STREAM_PATH, headers=headers):
                pass


def test_without_a_registry_there_is_no_pairing(tmp_path):
    with TestClient(create_app(make_deps(tmp_path), {})) as client:
        body = {"device_id": "dev", "code": "123456"}
        assert client.post(PAIR_PATH, json=body).status_code in (404, 405)
