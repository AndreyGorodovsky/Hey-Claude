"""Volume requests, recognised from the transcript without the model.

"Louder", "turn it down", "volume five" and the like are answered here,
before the request would go to Claude: they are quicker to answer this way,
cost nothing, and never reach the conversation. Only a request that is about
the volume and nothing else is taken. Anything longer, such as "how do I turn
up the volume on my television", is a question and goes to the model, whose
system prompt says how the volume is changed (``llm/prompt.py``).

The device keeps its own level, states it with each request, and decides
what each level sounds like; this module works out the new level and the
sentence that confirms it. It holds no state.

The phrases are built from one table per direction (``_Direction``), filled
into shared patterns, so that what can be said to turn the volume up can
always be said, mirrored, to turn it down. A request may hold several in a
row, as people repeat or correct themselves ("volume three, volume three",
"louder, volume nine"); they are applied in order.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from server.protocol import VOLUME_MAX, VOLUME_MIN

#: Levels moved by one "louder" or "quieter".
_STEP = 1

#: Numbers as speech recognition may write them out, by value.
_NUMBER_NAMES = (
    "zero one two three four five six seven eight nine ten eleven twelve"
    " thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty"
).split()
# Every level must have a name, to be confirmed aloud. Checked when the
# server starts, so that a change to the scale fails there and not in a turn.
assert VOLUME_MAX < len(_NUMBER_NAMES), "a volume level has no spoken name"
_NUMBER_VALUES = {name: value for value, name in enumerate(_NUMBER_NAMES)}


def _name(level: int) -> str:
    return _NUMBER_NAMES[level]


# Words around the request, taken off both ends before it is matched.
_FILLER = (
    r"please|okay|ok|hey|claude|so|now|just|thanks|thank you"
    r"|can you|could you|would you|will you"
    r"|a bit|a little|a little bit|a tad|a notch|one notch|slightly"
)
_LEADING = re.compile(rf"^(?:(?:{_FILLER})\s+)+")
_TRAILING = re.compile(rf"(?:\s+(?:{_FILLER}))+$")


@dataclass(frozen=True)
class _Direction:
    """The words for one direction of a change in volume."""

    #: "louder"
    comparatives: tuple[str, ...]
    #: "turn it up"
    particle: str
    #: "raise the volume"
    verbs: tuple[str, ...]
    #: "more volume"
    amount: str
    #: "too quiet" asks for the other direction's opposite: these are the
    #: complaints that ask for this direction.
    complaints: tuple[str, ...]
    #: "maximum volume"
    extremes: tuple[str, ...]
    #: Phrasings with no mirror in the other direction, such as "speak up".
    extra: tuple[str, ...] = ()


def _alternatives(words: tuple[str, ...]) -> str:
    return "(?:" + "|".join(words) + ")"


#: What is made louder or quieter: "the volume", "your voice".
_NOUN = r"(?:(?:the |your )?(?:volume|sound|voice))"


def _step_pattern(d: _Direction) -> re.Pattern[str]:
    comparative = _alternatives(d.comparatives)
    particle = d.particle
    phrases = [
        rf"(?:(?:speak|be|talk|get) |make (?:it|yourself|{_NOUN}) )?{comparative}",
        rf"(?:turn|crank|bring|put) (?:it|yourself|{_NOUN}) {particle}",
        rf"turn {particle} {_NOUN}",
        rf"{_alternatives(d.verbs)} {_NOUN}",
        rf"{_NOUN} {particle}",
        rf"{d.amount} volume",
        rf"(?:you are |you're |it's |it is |that's |{_NOUN} is )?too"
        rf" {_alternatives(d.complaints)}",
        *d.extra,
    ]
    return re.compile("|".join(phrases))


def _extreme_pattern(d: _Direction) -> re.Pattern[str]:
    extreme = _alternatives(d.extremes)
    phrases = [
        rf"{extreme} volume",
        rf"(?:set |turn |put )?(?:the )?volume (?:to )?(?:the )?{extreme}",
        rf"turn (?:it|the volume|yourself) all the way {d.particle}",
    ]
    return re.compile("|".join(phrases))


_UP = _Direction(
    comparatives=("louder",),
    particle="up",
    verbs=("increase", "raise", "boost"),
    amount="more",
    complaints=("quiet", "soft", "faint"),
    extremes=("max", "maximum", "full", "top", "highest", "loudest"),
    extra=("speak up", "i can't hear you", "i cannot hear you"),
)
_DOWN = _Direction(
    comparatives=("quieter", "softer"),
    particle="down",
    verbs=("decrease", "lower", "reduce"),
    amount="less",
    complaints=("loud",),
    extremes=("min", "minimum", "lowest", "quietest", "bottom"),
    extra=("speak (?:more )?(?:quietly|softly)",),
)

_STEPS = ((_step_pattern(_UP), +_STEP), (_step_pattern(_DOWN), -_STEP))
_EXTREMES = (
    (_extreme_pattern(_UP), VOLUME_MAX),
    (_extreme_pattern(_DOWN), VOLUME_MIN),
)

_NUMBER = r"(?:\d+|" + "|".join(_NUMBER_NAMES) + r")"
_SET = re.compile(
    r"(?:(?:set|change|put|turn) )?(?:the )?volume (?:level )?(?:up |down )?"
    rf"(?:to |at |on )?(?P<level>{_NUMBER})(?: out of {_name(VOLUME_MAX)})?"
    rf"|(?:set|turn|put) (?:it|yourself) (?:up |down )?to (?P<level_it>{_NUMBER})"
)


@dataclass(frozen=True)
class VolumeChange:
    """What to do about a volume request."""

    #: The level to send to the device; the same as before when nothing moves.
    level: int
    #: The sentence spoken in answer, at the new level.
    reply: str


#: Longest request read as a run of volume commands, in words. Anything
#: longer is a sentence about something else, and is not searched.
_MAX_WORDS = 16


def _normalise(transcript: str) -> str:
    text = transcript.lower().replace("-", " ").replace("’", "'")
    # Digits and apostrophes are kept: "volume 5", "can't hear you".
    text = re.sub(r"[^a-z0-9' ]+", " ", text)
    return " ".join(text.split())


def _trim(text: str) -> str:
    return _TRAILING.sub("", _LEADING.sub("", text))


def _command(text: str) -> tuple[str, int] | None:
    """One volume command: ("step", +1 or -1) or ("set", a level, which may
    be off the scale). None if ``text`` is not exactly one command."""
    text = _trim(text)
    for pattern, step in _STEPS:
        if pattern.fullmatch(text):
            return ("step", step)
    for pattern, level in _EXTREMES:
        if pattern.fullmatch(text):
            return ("set", level)
    if match := _SET.fullmatch(text):
        said = match["level"] or match["level_it"]
        return ("set", int(said) if said.isdigit() else _NUMBER_VALUES[said])
    return None


def _commands(words: list[str]) -> list[tuple[str, int]] | None:
    """Split ``words`` into one or more commands, or None if they are not.

    Tries the shortest first command first, so that "louder louder" is two
    steps, and backtracks when the rest cannot be split. The requests are a
    few words long, so the search is small.
    """
    if not words:
        return []
    for end in range(1, len(words) + 1):
        first = _command(" ".join(words[:end]))
        if first is None:
            continue
        rest = _commands(words[end:])
        if rest is not None:
            return [first, *rest]
    return None


def _confirm(level: int) -> VolumeChange:
    return VolumeChange(level, f"Volume {_name(level)}.")


def volume_request(transcript: str, current: int | None) -> VolumeChange | None:
    """Return the change ``transcript`` asks for, or None if it is not one.

    ``current`` is the level the device stated. Without a valid one, from an
    older device, nothing is taken as a volume request: the device would not
    understand the answer.
    """
    if current is None or not VOLUME_MIN <= current <= VOLUME_MAX:
        return None
    words = _trim(_normalise(transcript)).split()
    if len(words) > _MAX_WORDS:
        return None
    commands = _commands(words)
    if not commands:
        return None

    level = current
    for kind, value in commands:
        if kind == "step":
            level = min(max(level + value, VOLUME_MIN), VOLUME_MAX)
        elif VOLUME_MIN <= value <= VOLUME_MAX:
            level = value
    # The answer is about the last command, which is what was meant last
    kind, value = commands[-1]
    if kind == "set" and not VOLUME_MIN <= value <= VOLUME_MAX:
        return VolumeChange(
            level,
            f"The volume goes from {_name(VOLUME_MIN)} to {_name(VOLUME_MAX)}.",
        )
    if kind == "step" and level == current:
        end = "loud" if value > 0 else "quiet"
        return VolumeChange(level, f"That's as {end} as I go.")
    return _confirm(level)
