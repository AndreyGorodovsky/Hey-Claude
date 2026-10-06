"""Configuration for the Hey Claude server.

Values are read from the process environment, optionally seeded by a local
``.env`` file. Two rules hold throughout, because this repository is public:

1. No credential has a default. A missing key fails loudly at startup rather
   than falling back to something that happens to work on one machine.
2. No credential is ever rendered. Secrets are held as ``SecretStr``, whose
   ``repr`` is masked, so an exception traceback or a debug log that dumps the
   settings object cannot expose them.

See SECRETS.md at the repository root.
"""

from __future__ import annotations

import logging
import re
from functools import lru_cache
from pathlib import Path
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from pydantic import Field, SecretStr, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict

from server.protocol import (
    DEVICE_CAPTURE_LIMIT_SECONDS,
    MAX_DEVICE_ID_LENGTH,
    MAX_DEVICE_TOKEN_LENGTH,
)


#: The directory holding this file. The ``.env`` file and relative storage
#: paths are resolved against it, so the server behaves the same whatever
#: directory it is started from.
SERVER_DIR = Path(__file__).resolve().parent

#: Shortest device token accepted. A token is the only thing standing between
#: the local network and the API keys' spend, so a short one is refused.
MIN_DEVICE_TOKEN_LENGTH = 24


class Settings(BaseSettings):
    """Server configuration, populated from the environment."""

    model_config = SettingsConfigDict(
        env_file=SERVER_DIR / ".env",
        env_file_encoding="utf-8",
        # Unrelated variables in the ambient environment are ignored rather
        # than rejected, so the server stays startable on any machine.
        extra="ignore",
        # A validation error normally quotes the rejected value. For
        # DEVICE_TOKENS that value is the tokens themselves, so no error
        # from these settings ever quotes its input.
        hide_input_in_errors=True,
    )

    # --- Credentials -----------------------------------------------------
    # Deliberately without defaults: absence is a startup failure.

    anthropic_api_key: SecretStr
    deepgram_api_key: SecretStr

    #: Device identifier -> shared secret, as a JSON object. Empty by default:
    #: with no entry, no device can connect.
    device_tokens: dict[str, SecretStr] = Field(default_factory=dict)

    # --- Transport -------------------------------------------------------

    bind_host: str = "0.0.0.0"
    bind_port: int = Field(default=8765, ge=1, le=65535)

    #: Advertise the server on the local network by mDNS, so that devices
    #: find it without a configured address.
    mdns_enabled: bool = True

    # --- Conversation ----------------------------------------------------

    timezone: str = "UTC"
    day_rollover_hour: int = Field(default=4, ge=0, le=23)
    claude_model: str = "claude-opus-5"
    claude_effort: str = "low"

    #: Ceiling on one reply, thinking included. A spoken reply is short; the
    #: ceiling only stops a runaway one from being synthesised for minutes.
    claude_max_tokens: int = Field(default=8000, ge=256, le=64_000)

    # --- Speech ----------------------------------------------------------

    stt_model: str = "nova-3"
    stt_language: str = "en"
    tts_model: str = "aura-2-thalia-en"

    #: Silence after speech that counts as the end of the utterance. Shorter
    #: answers sooner but cuts off a speaker who pauses mid-sentence.
    endpointing_ms: int = Field(default=400, ge=100, le=2000)

    #: How long to wait for the first recognised word after the wake word
    #: before giving up on the utterance.
    no_speech_timeout_seconds: float = Field(default=5.0, ge=1.0, le=30.0)

    # --- Storage ---------------------------------------------------------

    db_path: Path = Path("data/hey_claude.sqlite3")

    # --- Audio -----------------------------------------------------------

    #: Rate of the reply audio, announced to the device with each reply. The
    #: capture rate is not a setting: the protocol fixes it.
    playback_sample_rate: int = 24_000

    #: Longest a request may run before the server stops it and answers what
    #: was heard. Kept below the device's own limit, so that the server's
    #: orderly ending always comes first.
    max_capture_seconds: float = Field(
        default=13.0, ge=1.0, le=DEVICE_CAPTURE_LIMIT_SECONDS - 1
    )

    # --- Logging ---------------------------------------------------------

    log_level: str = "INFO"

    #: Transcripts are personal data. Off by default; enable only for local
    #: debugging where logs are not retained.
    log_transcripts: bool = False

    @field_validator("device_tokens")
    @classmethod
    def _tokens_must_be_long(
        cls, value: dict[str, SecretStr]
    ) -> dict[str, SecretStr]:
        for device_id, token in value.items():
            # The messages name the device and never the token.
            if not 0 < len(device_id.strip()) <= MAX_DEVICE_ID_LENGTH:
                raise ValueError(
                    "DEVICE_TOKENS has a device identifier that is empty or "
                    f"longer than {MAX_DEVICE_ID_LENGTH} characters"
                )
            length = len(token.get_secret_value())
            if not MIN_DEVICE_TOKEN_LENGTH <= length <= MAX_DEVICE_TOKEN_LENGTH:
                raise ValueError(
                    f"DEVICE_TOKENS entry for {device_id!r} must be "
                    f"{MIN_DEVICE_TOKEN_LENGTH} to {MAX_DEVICE_TOKEN_LENGTH} "
                    "characters long"
                )
        return value

    @field_validator("timezone")
    @classmethod
    def _timezone_must_resolve(cls, value: str) -> str:
        try:
            ZoneInfo(value)
        except (ZoneInfoNotFoundError, ValueError) as exc:
            raise ValueError(
                f"TIMEZONE {value!r} is not a valid IANA timezone name"
            ) from exc
        return value

    @field_validator("claude_effort")
    @classmethod
    def _effort_must_be_known(cls, value: str) -> str:
        allowed = {"low", "medium", "high", "xhigh", "max"}
        if value not in allowed:
            raise ValueError(
                f"CLAUDE_EFFORT {value!r} is not one of {sorted(allowed)}"
            )
        return value

    @property
    def tz(self) -> ZoneInfo:
        """The configured timezone, resolved."""
        return ZoneInfo(self.timezone)

    @property
    def db_file(self) -> Path:
        """The database path, with a relative one placed under the server."""
        if self.db_path.is_absolute():
            return self.db_path
        return SERVER_DIR / self.db_path


@lru_cache(maxsize=1)
def get_settings() -> Settings:
    """Return the process-wide settings, loaded once.

    Raises ``pydantic.ValidationError`` if a required credential is absent, so
    that misconfiguration surfaces at startup rather than on the first request.
    """
    return Settings()


# --- Log redaction -------------------------------------------------------
#
# Defence in depth. SecretStr already prevents settings objects from rendering
# their contents, but a credential can still reach a log through a third-party
# library, an echoed request header, or an exception message from an HTTP
# client. The formatter below catches those before they are written.

_REDACTION_PATTERNS: tuple[re.Pattern[str], ...] = (
    # Anthropic keys.
    re.compile(r"sk-ant-[A-Za-z0-9._\-]+"),
    # Bearer tokens and Deepgram's Token scheme, in headers or prose.
    re.compile(r"(?i)\b(bearer|token)\s+[A-Za-z0-9._\-]{16,}"),
    # Anything that looks like an assignment to a secret-shaped name.
    re.compile(
        r"(?i)\b([a-z_]*(?:api[_-]?key|secret|token|password|passphrase))"
        r"\s*[=:]\s*\S+"
    ),
)

_REDACTED = "[REDACTED]"


class RedactingFormatter(logging.Formatter):
    """Strip credential-shaped substrings from every line that is logged.

    Done in the formatter, not in a filter, so that it sees the finished
    text: the message with its arguments filled in, and any traceback with
    the exception's own message.
    """

    def format(self, record: logging.LogRecord) -> str:
        return redact(super().format(record))


def redact(text: str) -> str:
    """Replace credential-shaped substrings in ``text``."""
    for pattern in _REDACTION_PATTERNS:
        if pattern.groups:
            text = pattern.sub(rf"\1={_REDACTED}", text)
        else:
            text = pattern.sub(_REDACTED, text)
    return text


def configure_logging(settings: Settings | None = None) -> None:
    """Install the root log configuration, with redaction applied."""
    settings = settings or get_settings()

    handler = logging.StreamHandler()
    handler.setFormatter(
        RedactingFormatter("%(asctime)s %(levelname)-8s %(name)s: %(message)s")
    )

    root = logging.getLogger()
    root.handlers.clear()
    root.addHandler(handler)
    root.setLevel(settings.log_level.upper())

    # The HTTP clients log a line for every request at INFO, which buries
    # the server's own few lines per turn.
    for name in ("httpx", "httpx2", "httpcore", "httpcore2"):
        logging.getLogger(name).setLevel(logging.WARNING)
