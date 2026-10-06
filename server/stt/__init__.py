"""Speech-to-text: the adapter interface and its implementations."""

from server.stt.base import (
    EndpointTracker,
    SpeechToText,
    SpeechToTextError,
    SpeechToTextStream,
)

__all__ = [
    "EndpointTracker",
    "SpeechToText",
    "SpeechToTextError",
    "SpeechToTextStream",
]
