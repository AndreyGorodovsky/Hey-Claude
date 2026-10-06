"""The Claude client.

Sends the day's conversation and the new request, and yields the reply as it
is generated, so that speech synthesis can begin on the first sentence.

History is resent on every turn, so the request is built for prompt caching:
the system prompt never changes, earlier exchanges are sent exactly as they
were stored, and cache breakpoints sit at the end of the system prompt and at the
end of the new request. Each turn then reads everything before it from the
cache and pays in full only for what the last turn added.
"""

from __future__ import annotations

import logging
from collections.abc import AsyncIterator, Sequence
from typing import Any

import anthropic
import httpx2
from pydantic import SecretStr

from server.llm.base import LanguageModel, LanguageModelError, ReplyRefused
from server.llm.prompt import SYSTEM_PROMPT
from server.conversation import Exchange

log = logging.getLogger(__name__)

#: Opts the request in to server-side fallback: a request the model's safety
#: classifiers decline is run again on a model Anthropic chooses, inside the
#: same call. This form needs no maintained list of fallback models.
_FALLBACK_BETA = "server-side-fallback-2026-07-01"

_CACHE = {"type": "ephemeral"}

#: Seconds allowed to connect, and seconds of silence tolerated on the reply
#: stream. The SDK's own default tolerates ten minutes of silence, which
#: would hold a device in its thinking state for as long.
_TIMEOUT = anthropic.Timeout(30.0, connect=5.0, read=20.0)


def build_messages(history: Sequence[Exchange], user_text: str) -> list[dict[str, Any]]:
    """Build the ``messages`` of a request from stored exchanges and a new one."""
    messages: list[dict[str, Any]] = []
    for exchange in history:
        messages.append({"role": "user", "content": exchange.user_text})
        messages.append({"role": "assistant", "content": exchange.assistant_text})
    messages.append(
        {
            "role": "user",
            "content": [
                {"type": "text", "text": user_text, "cache_control": _CACHE}
            ],
        }
    )
    return messages


class ClaudeLanguageModel(LanguageModel):
    """Replies from Claude, streamed."""

    def __init__(
        self, api_key: SecretStr, *, model: str, effort: str, max_tokens: int
    ) -> None:
        self._client = anthropic.AsyncAnthropic(
            api_key=api_key.get_secret_value(),
            timeout=_TIMEOUT,
            # One retry, not the default two: each waits out a timeout while
            # the person waits for an answer.
            max_retries=1,
        )
        self._model = model
        self._effort = effort
        self._max_tokens = max_tokens

    async def stream_reply(
        self, history: Sequence[Exchange], user_text: str
    ) -> AsyncIterator[str]:
        try:
            async with self._client.beta.messages.stream(
                model=self._model,
                max_tokens=self._max_tokens,
                betas=[_FALLBACK_BETA],
                fallbacks="default",
                # Thinking stays on, as the project requires; effort is the
                # control for how much of it there is, and so for latency.
                thinking={"type": "adaptive"},
                output_config={"effort": self._effort},
                system=[
                    {"type": "text", "text": SYSTEM_PROMPT, "cache_control": _CACHE}
                ],
                messages=build_messages(history, user_text),
            ) as stream:
                async for text in stream.text_stream:
                    yield text
                message = await stream.get_final_message()
        except anthropic.RateLimitError:
            raise LanguageModelError("Claude rate limit reached") from None
        except anthropic.APIStatusError as exc:
            raise LanguageModelError(
                f"Claude returned status {exc.status_code}"
            ) from None
        except anthropic.APIConnectionError:
            raise LanguageModelError("could not reach Claude") from None
        except (anthropic.APIError, httpx2.HTTPError) as exc:
            # A reply that breaks off part-way, or one the SDK cannot read.
            raise LanguageModelError(
                f"Claude reply failed ({type(exc).__name__})"
            ) from None

        usage = message.usage
        log.info(
            "claude: model=%s stop=%s input=%s cache_read=%s cache_write=%s output=%s",
            message.model,
            message.stop_reason,
            usage.input_tokens,
            usage.cache_read_input_tokens,
            usage.cache_creation_input_tokens,
            usage.output_tokens,
        )
        if message.stop_reason == "refusal":
            raise ReplyRefused("Claude declined the request")
        if message.stop_reason == "max_tokens":
            log.warning("claude: reply cut off at the token ceiling")

    async def aclose(self) -> None:
        await self._client.close()
