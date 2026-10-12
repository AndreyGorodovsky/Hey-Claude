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
        """Yield the reply as pieces of text, in order, as they are produced.

        The reply is spoken sentence by sentence, and a sentence is taken
        to be over only when something follows it. So when the text pauses
        with more to come, as it does while a model uses a tool, the piece
        before the pause must end in whitespace or a line break; otherwise
        its last sentence waits, unspoken, until the pause is over.
        """

    async def aclose(self) -> None:
        """Release any connection held open between requests."""
