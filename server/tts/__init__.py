"""Text-to-speech: the adapter interface and its implementations."""

from server.tts.base import TextToSpeech, TextToSpeechError

__all__ = ["TextToSpeech", "TextToSpeechError"]
