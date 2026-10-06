"""The text-to-speech interface the turn pipeline is written against."""

from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import AsyncIterator


class TextToSpeechError(Exception):
    """The text-to-speech service failed or could not be reached."""


class TextToSpeech(ABC):
    """A text-to-speech provider."""

    @abstractmethod
    def synthesize(self, text: str, sample_rate: int) -> AsyncIterator[bytes]:
        """Yield ``text`` as 16-bit mono PCM at ``sample_rate``, as it arrives.

        Pieces may be of any size and may split a sample in two; the caller
        cuts them into frames.
        """

    async def aclose(self) -> None:
        """Release any connection held open between requests."""
