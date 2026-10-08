"""The Deepgram synthesiser client, against a stand-in for Deepgram.

The second try at connecting is the HTTP library's own and is not tested
here: a stand-in transport takes the place of the one that retries.
"""

import asyncio
from collections.abc import Awaitable, Callable

import httpx
import pytest
from pydantic import SecretStr

from server.tts import TextToSpeechError
from server.tts import deepgram
from server.tts.deepgram import DeepgramTextToSpeech

AUDIO = bytes(960)

Handler = Callable[[httpx.Request], httpx.Response | Awaitable[httpx.Response]]


async def speak(handler: Handler) -> bytes:
    """Synthesise one sentence with ``handler`` in place of the network."""
    tts = DeepgramTextToSpeech(
        SecretStr("not-a-real-key"),
        model="test-voice",
        transport=httpx.MockTransport(handler),
    )
    try:
        return b"".join([piece async for piece in tts.synthesize("Hello.", 24_000)])
    finally:
        await tts.aclose()


async def test_audio_is_passed_on_and_the_request_names_the_voice():
    seen: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(request)
        return httpx.Response(200, content=AUDIO)

    assert await speak(handler) == AUDIO
    assert seen[0].url.params["model"] == "test-voice"
    assert seen[0].url.params["sample_rate"] == "24000"


async def test_an_unreachable_service_is_a_failure():
    def handler(request: httpx.Request) -> httpx.Response:
        raise httpx.ConnectError("refused", request=request)

    with pytest.raises(TextToSpeechError):
        await speak(handler)


async def test_a_refused_request_is_a_failure():
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(401)

    with pytest.raises(TextToSpeechError):
        await speak(handler)


async def test_a_service_that_does_not_answer_is_given_up_in_time(monkeypatch):
    # However the library's own limits add up, the wait for the start of
    # the response ends at the time the protocol allows.
    monkeypatch.setattr(deepgram, "SERVICE_CONNECT_SECONDS", 0.05)

    async def handler(request: httpx.Request) -> httpx.Response:
        await asyncio.sleep(5)
        return httpx.Response(200, content=AUDIO)

    with pytest.raises(TextToSpeechError):
        await asyncio.wait_for(speak(handler), timeout=2)
