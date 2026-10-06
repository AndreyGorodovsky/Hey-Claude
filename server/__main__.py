"""Entry point: ``python -m server`` from the repository root."""

from __future__ import annotations

import logging
import sys
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager

import uvicorn
from pydantic import ValidationError

from server.config import Settings, configure_logging, get_settings
from server.conversation import ConversationStore
from server.discovery import ServiceAdvertiser
from server.llm.claude import ClaudeLanguageModel
from server.pipeline import TurnDeps, TurnLimits
from server.protocol import PING_INTERVAL_SECONDS, PING_TIMEOUT_SECONDS
from server.stt.deepgram import DeepgramSpeechToText
from server.transport import create_app
from server.tts.deepgram import DeepgramTextToSpeech

log = logging.getLogger("server")


def build_deps(settings: Settings) -> TurnDeps:
    """Assemble the turn pipeline from the configured providers."""
    return TurnDeps(
        stt=DeepgramSpeechToText(
            settings.deepgram_api_key,
            model=settings.stt_model,
            language=settings.stt_language,
            endpointing_ms=settings.endpointing_ms,
        ),
        llm=ClaudeLanguageModel(
            settings.anthropic_api_key,
            model=settings.claude_model,
            effort=settings.claude_effort,
            max_tokens=settings.claude_max_tokens,
        ),
        tts=DeepgramTextToSpeech(settings.deepgram_api_key, model=settings.tts_model),
        store=ConversationStore(
            settings.db_file, settings.tz, settings.day_rollover_hour
        ),
        playback_sample_rate=settings.playback_sample_rate,
        limits=TurnLimits(
            max_capture_seconds=settings.max_capture_seconds,
            no_speech_timeout_seconds=settings.no_speech_timeout_seconds,
        ),
        log_transcripts=settings.log_transcripts,
    )


def main() -> None:
    try:
        settings = get_settings()
    except ValidationError as exc:
        # Each problem is named by its setting. The rejected values are not
        # shown: some of them are credentials.
        problems = "\n".join(
            f"  {'.'.join(map(str, error['loc'])).upper()}: {error['msg']}"
            for error in exc.errors(include_input=False, include_url=False)
        )
        sys.exit(f"The server's configuration is not valid:\n{problems}")
    configure_logging(settings)
    if not settings.device_tokens:
        log.warning("DEVICE_TOKENS is empty: no device will be able to connect")

    deps = build_deps(settings)
    advertiser = ServiceAdvertiser(settings.bind_host, settings.bind_port)

    @asynccontextmanager
    async def background() -> AsyncIterator[None]:
        if settings.mdns_enabled:
            await advertiser.start()
        try:
            yield
        finally:
            await advertiser.stop()
            await deps.tts.aclose()
            await deps.llm.aclose()

    app = create_app(deps, settings.device_tokens, background=background)
    uvicorn.run(
        app,
        host=settings.bind_host,
        port=settings.bind_port,
        # Stated, not left to uvicorn's defaults: the device's own keepalive
        # is written against these.
        ws_ping_interval=PING_INTERVAL_SECONDS,
        ws_ping_timeout=PING_TIMEOUT_SECONDS,
        # Leave logging as configure_logging set it up, so that uvicorn's
        # lines pass through the same redaction as the server's own.
        log_config=None,
    )


if __name__ == "__main__":
    main()
