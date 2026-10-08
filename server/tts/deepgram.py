"""Deepgram text-to-speech.

One HTTP request per sentence, with the audio read as it is produced. The
client is kept open between requests. Its connection is not opened ahead of
need: measured on 2026-10-08, the first audio of a sentence came 0.5 to
0.6 s after the request whether the connection was new, opened beforehand
or reused, so the wait is the synthesiser's own.
"""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator

import httpx
from pydantic import SecretStr

from server.protocol import SERVICE_CONNECT_SECONDS, TTS_SILENCE_SECONDS
from server.tts.base import TextToSpeech, TextToSpeechError

_SPEAK_URL = "https://api.deepgram.com/v1/speak"

#: Seconds one try at connecting may take in each of its two steps, the TCP
#: connection and the TLS handshake, and seconds of silence tolerated once
#: the response is arriving. A connection normally opens in a fraction of a
#: second. The first one after the server starts has been seen to hang and
#: the next to succeed, so a try is cut short at half the time allowed for
#: reaching the service, which leaves room for the second.
_TIMEOUT = httpx.Timeout(TTS_SILENCE_SECONDS, connect=SERVICE_CONNECT_SECONDS / 2)

#: Further tries at connecting after a first that fails. Done by the HTTP
#: library, which retries only failures to connect: nothing has been sent
#: by then, so a sentence cannot be spoken twice.
_CONNECT_RETRIES = 1


class DeepgramTextToSpeech(TextToSpeech):
    """Deepgram's speak endpoint, returning raw PCM."""

    def __init__(
        self,
        api_key: SecretStr,
        *,
        model: str,
        transport: httpx.AsyncBaseTransport | None = None,
    ) -> None:
        """``transport`` replaces the network, for tests."""
        self._model = model
        self._client = httpx.AsyncClient(
            headers={"Authorization": f"Token {api_key.get_secret_value()}"},
            timeout=_TIMEOUT,
            transport=transport
            or httpx.AsyncHTTPTransport(retries=_CONNECT_RETRIES),
        )

    async def synthesize(self, text: str, sample_rate: int) -> AsyncIterator[bytes]:
        request = self._client.build_request(
            "POST",
            _SPEAK_URL,
            params={
                "model": self._model,
                "encoding": "linear16",
                "sample_rate": sample_rate,
                # No WAV header: the stream is samples from the first byte.
                "container": "none",
            },
            json={"text": text},
        )
        try:
            # One limit over everything up to the start of the response:
            # both tries at connecting, each with its two steps, and the
            # synthesiser's own time to begin answering. The library's
            # limits apply to each step separately and would add up to more
            # than the protocol allows for reaching a service.
            async with asyncio.timeout(SERVICE_CONNECT_SECONDS):
                response = await self._client.send(request, stream=True)
        except TimeoutError:
            raise TextToSpeechError("Deepgram did not answer in time") from None
        except httpx.HTTPError as exc:
            # Only the kind of failure is kept; the text of an HTTP error
            # can repeat the request, which holds what is being said.
            raise TextToSpeechError(
                f"Deepgram request failed ({type(exc).__name__})"
            ) from None
        try:
            if response.status_code != httpx.codes.OK:
                raise TextToSpeechError(
                    f"Deepgram returned status {response.status_code}"
                )
            async for piece in response.aiter_bytes():
                if piece:
                    yield piece
        except httpx.HTTPError as exc:
            raise TextToSpeechError(
                f"Deepgram request failed ({type(exc).__name__})"
            ) from None
        finally:
            await response.aclose()

    async def aclose(self) -> None:
        await self._client.aclose()
