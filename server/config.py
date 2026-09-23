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


class Settings(BaseSettings):
    """Server configuration, populated from the environment."""

    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        # Unrelated variables in the ambient environment are ignored rather
        # than rejected, so the server stays startable on any machine.
        extra="ignore",
    )

    # --- Credentials -----------------------------------------------------
    # Deliberately without defaults: absence is a startup failure.

    anthropic_api_key: SecretStr
    deepgram_api_key: SecretStr

    # --- Transport -------------------------------------------------------

    bind_host: str = "0.0.0.0"
    bind_port: int = Field(default=8765, ge=1, le=65535)

    # --- Conversation ----------------------------------------------------

    timezone: str = "UTC"
    day_rollover_hour: int = Field(default=4, ge=0, le=23)
    claude_model: str = "claude-opus-5"
    claude_effort: str = "low"

    # --- Storage ---------------------------------------------------------

    db_path: Path = Path("data/hey_claude.sqlite3")

    # --- Audio -----------------------------------------------------------

    capture_sample_rate: int = 16_000
    playback_sample_rate: int = 24_000
    max_capture_seconds: int = Field(default=15, ge=1, le=120)

    # --- Logging ---------------------------------------------------------

    log_level: str = "INFO"

    #: Transcripts are personal data. Off by default; enable only for local
    #: debugging where logs are not retained.
    log_transcripts: bool = False

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
# client. This filter catches those before they are written.

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


class RedactingFilter(logging.Filter):
    """Strip credential-shaped substrings from log records."""

    def filter(self, record: logging.LogRecord) -> bool:
        record.msg = redact(str(record.msg))
        if record.args:
            if isinstance(record.args, dict):
                record.args = {
                    key: redact(str(value)) for key, value in record.args.items()
                }
            else:
                record.args = tuple(redact(str(arg)) for arg in record.args)
        return True


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
        logging.Formatter("%(asctime)s %(levelname)-8s %(name)s: %(message)s")
    )
    handler.addFilter(RedactingFilter())

    root = logging.getLogger()
    root.handlers.clear()
    root.addHandler(handler)
    root.setLevel(settings.log_level.upper())
