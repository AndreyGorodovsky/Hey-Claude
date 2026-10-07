"""Deepgram text-to-speech.

One HTTP request per sentence, with the audio read as it is produced. The
client is kept open between requests, so that only the first sentence of a
session pays for a new TLS connection.
"""

from __future__ import annotations

from collections.abc import AsyncIterator

import httpx
from pydantic import SecretStr

from server.protocol import SERVICE_CONNECT_SECONDS, TTS_SILENCE_SECONDS
from server.tts.base import TextToSpeech, TextToSpeechError

_SPEAK_URL = "https://api.deepgram.com/v1/speak"

#: Seconds to connect, and seconds of silence tolerated mid-response.
_TIMEOUT = httpx.Timeout(TTS_SILENCE_SECONDS, connect=SERVICE_CONNECT_SECONDS)


class DeepgramTextToSpeech(TextToSpeech):
    """Deepgram's speak endpoint, returning raw PCM."""

    def __init__(self, api_key: SecretStr, *, model: str) -> None:
        self._model = model
        self._client = httpx.AsyncClient(
            headers={"Authorization": f"Token {api_key.get_secret_value()}"},
            timeout=_TIMEOUT,
        )

    async def synthesize(self, text: str, sample_rate: int) -> AsyncIterator[bytes]:
        params = {
            "model": self._model,
            "encoding": "linear16",
            "sample_rate": sample_rate,
            # No WAV header: the stream is samples from the first byte.
            "container": "none",
        }
        try:
            async with self._client.stream(
                "POST", _SPEAK_URL, params=params, json={"text": text}
            ) as response:
                if response.status_code != httpx.codes.OK:
                    raise TextToSpeechError(
                        f"Deepgram returned status {response.status_code}"
                    )
                async for piece in response.aiter_bytes():
                    if piece:
                        yield piece
        except httpx.HTTPError as exc:
            # Only the kind of failure is kept; the text of an HTTP error can
            # repeat the request, which holds what is being said.
            raise TextToSpeechError(
                f"Deepgram request failed ({type(exc).__name__})"
            ) from None

    async def aclose(self) -> None:
        await self._client.aclose()
