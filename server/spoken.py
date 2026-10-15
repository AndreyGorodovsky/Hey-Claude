"""Numbers as they are spoken, for what the assistant says and hears.

Speech-to-text writes numbers as words (``stt/deepgram.py``), and the
synthesiser reads a word as surely as a figure, so the volume's levels are
heard and said by name. Shared by the volume grammar and the system prompt,
which both describe the volume's scale.
"""

from __future__ import annotations

from server.protocol import VOLUME_MAX, VOLUME_MIN

#: The names of the numbers from zero, by value.
NUMBER_NAMES = (
    "zero one two three four five six seven eight nine ten eleven twelve"
    " thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty"
).split()

# Every volume level must have a name, to be heard and confirmed aloud.
# Checked when the server starts, so that a change to the scale fails there
# and not in a turn.
assert VOLUME_MIN >= 0 and VOLUME_MAX < len(NUMBER_NAMES), "a level has no name"


def spoken_number(value: int) -> str:
    """The name of ``value``, which must be from zero to twenty."""
    return NUMBER_NAMES[value]
