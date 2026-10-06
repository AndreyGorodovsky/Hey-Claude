"""Cutting a reply into sentences while it is still being generated.

Each finished sentence is synthesised at once, so playback starts while the
rest of the reply is still on its way. A sentence is the unit because a
synthesiser needs a whole one to get the intonation right.

A missed boundary costs little: two sentences are synthesised as one. A
false one costs more, a request to synthesise a fragment, so the rules lean
towards not cutting.
"""

from __future__ import annotations

import re

#: A run of sentence-ending punctuation, any closing quotes or brackets, then
#: whitespace; or a line break. The whitespace is required: a full stop at
#: the very end of what has arrived may yet turn out to be inside "3.5".
_BOUNDARY = re.compile(r"[.!?…]+[\"'”’)\]]*\s+|\n+")

#: Words whose full stop does not end a sentence.
_ABBREVIATIONS = frozenset(
    {"mr", "mrs", "ms", "dr", "prof", "sr", "jr", "st", "vs", "etc", "approx"}
)

#: The last word before a closing full stop, with any dots inside it.
_LAST_WORD = re.compile(r"([A-Za-z0-9][A-Za-z0-9.]*)\.$")


def _full_stop_is_not_an_ending(text: str) -> bool:
    """Whether the full stop ending ``text`` belongs to its last word."""
    match = _LAST_WORD.search(text)
    if match is None:
        return False
    word = match.group(1)
    return (
        word.lower() in _ABBREVIATIONS
        # Dotted forms: "p.m.", "U.S.", "e.g.".
        or "." in word
        # A single capital is an initial, as in "J. K. Rowling".
        or (len(word) == 1 and word.isupper())
        # A number alone is a list marker, "1.", not a sentence.
        or text.strip() == word + "."
        and word.isdigit()
    )


def _speakable(text: str) -> bool:
    """Whether ``text`` holds anything a synthesiser can say."""
    return any(character.isalnum() for character in text)


class SentenceSplitter:
    """Collects streamed text and gives back whole sentences."""

    def __init__(self) -> None:
        self._buffer = ""

    def feed(self, text: str) -> list[str]:
        """Add ``text`` and return any sentences it completed."""
        self._buffer += text
        sentences: list[str] = []
        start = 0
        for boundary in _BOUNDARY.finditer(self._buffer):
            candidate = self._buffer[start : boundary.end()].strip()
            # Punctuation by itself, as in ". . .", stays joined to what
            # follows it.
            if not _speakable(candidate) or _full_stop_is_not_an_ending(candidate):
                continue
            sentences.append(candidate)
            start = boundary.end()
        self._buffer = self._buffer[start:]
        return sentences

    def flush(self) -> list[str]:
        """Return whatever is left once the reply has ended."""
        rest = self._buffer.strip()
        self._buffer = ""
        return [rest] if _speakable(rest) else []
