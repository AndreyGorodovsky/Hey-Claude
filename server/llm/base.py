"""The language-model interface the turn pipeline is written against."""

from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import AsyncIterator, Sequence

from server.conversation import Exchange


class LanguageModelError(Exception):
    """The language model failed or could not be reached."""


class ReplyRefused(LanguageModelError):
    """The model declined the request. Raised after any text it did produce."""


class LanguageModel(ABC):
    """Produces the reply to one request, given the day's earlier exchanges."""

    @abstractmethod
    def stream_reply(
        self, history: Sequence[Exchange], user_text: str
    ) -> AsyncIterator[str]:
        """Yield the reply as pieces of text, in order, as they are produced."""

    async def aclose(self) -> None:
        """Release any connection held open between requests."""
