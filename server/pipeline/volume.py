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

How a request is read:

1. It is cut into clauses at the punctuation speech-to-text writes, since
   people repeat or correct themselves: "Volume three. Volume three.",
   "Louder, volume nine."
2. Each clause is one or more commands. A clause that is not exactly one is
   split into words and searched for a run of commands ("louder louder").
3. The commands are applied in order, noting whether a step met the end of
   the scale and whether a level off the scale was ignored. The reply is
   worded from those facts and the final level.

The phrases themselves come from one table per direction (``_Direction``),
filled into shared patterns, so that what turns the volume up can always be
said, mirrored, to turn it down; and every pattern accepts the same nouns
(``_NOUN``), so that "sound" and "voice" work wherever "volume" does.
Correction words ("no, I mean") are deliberately not handled: such a
request goes to the model.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from functools import cache

from server.protocol import VOLUME_MAX, VOLUME_MIN
from server.spoken import NUMBER_NAMES, spoken_number

#: Levels moved by one "louder" or "quieter".
_STEP = 1

_NUMBER_VALUES = {name: value for value, name in enumerate(NUMBER_NAMES)}

# Words around a command, taken off both ends of each clause.
_FILLER = (
    r"please|okay|ok|hey|claude|so|now|just|thanks|thank you"
    r"|can you|could you|would you|will you"
    r"|a bit|a little|a little bit|a tad|a notch|one notch|slightly"
)
_LEADING = re.compile(rf"^(?:(?:{_FILLER})\s+)+")
_ONLY_FILLER = re.compile(rf"(?:(?:{_FILLER})\s*)+")
_TRAILING = re.compile(rf"(?:\s+(?:{_FILLER}))+$")

#: What is made louder or quieter: "the volume", "your voice".
_NOUN = r"(?:(?:the |your )?(?:volume|sound|voice))"


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
    #: "too quiet" asks for this direction: the complaints that do.
    complaints: tuple[str, ...]
    #: "maximum volume"
    extremes: tuple[str, ...]
    #: Phrasings with no mirror in the other direction, such as "speak up".
    extra: tuple[str, ...] = ()


def _alternatives(words: tuple[str, ...]) -> str:
    return "(?:" + "|".join(words) + ")"


def _step_pattern(d: _Direction) -> re.Pattern[str]:
    comparative = _alternatives(d.comparatives)
    particle = d.particle
    phrases = [
        rf"(?:(?:speak|be|talk|get) |make (?:it|yourself|{_NOUN}) )?{comparative}",
        rf"(?:turn|crank|bring|put) (?:it|yourself|{_NOUN}) {particle}",
        rf"turn {particle} {_NOUN}",
        rf"{_alternatives(d.verbs)} {_NOUN}",
        rf"{_NOUN} {particle}",
        rf"{d.amount} {_NOUN}",
        rf"(?:you are |you're |it's |it is |that's |{_NOUN} is )?too"
        rf" {_alternatives(d.complaints)}",
        *d.extra,
    ]
    return re.compile("|".join(phrases))


def _extreme_pattern(d: _Direction) -> re.Pattern[str]:
    extreme = _alternatives(d.extremes)
    phrases = [
        rf"{extreme} {_NOUN}",
        rf"(?:set |turn |put )?{_NOUN} (?:to )?(?:the )?{extreme}",
        rf"turn (?:it|yourself|{_NOUN}) all the way {d.particle}",
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

_NUMBER = r"(?:\d+|" + "|".join(NUMBER_NAMES) + r")"
_SET = re.compile(
    rf"(?:(?:set|change|put|turn) )?{_NOUN} (?:level )?(?:up |down )?"
    rf"(?:to |at |on )?(?P<level>{_NUMBER})(?: out of {spoken_number(VOLUME_MAX)})?"
    rf"|(?:set|turn|put) (?:it|yourself) (?:up |down )?to (?P<level_it>{_NUMBER})"
)


# --- Commands --------------------------------------------------------------


@dataclass(frozen=True)
class _Step:
    """Louder or quieter by ``delta`` levels."""

    delta: int

    def __str__(self) -> str:
        return f"{self.delta:+d}"


@dataclass(frozen=True)
class _Set:
    """A named level, which may be off the scale."""

    level: int

    @property
    def on_scale(self) -> bool:
        return VOLUME_MIN <= self.level <= VOLUME_MAX

    def __str__(self) -> str:
        return f"={self.level}"


_Command = _Step | _Set


def _command(text: str) -> _Command | None:
    """``text`` as exactly one command, or None."""
    for pattern, step in _STEPS:
        if pattern.fullmatch(text):
            return _Step(step)
    for pattern, level in _EXTREMES:
        if pattern.fullmatch(text):
            return _Set(level)
    if match := _SET.fullmatch(text):
        said = match["level"] or match["level_it"]
        return _Set(int(said) if said.isdigit() else _NUMBER_VALUES[said])
    return None


def _clause_commands(clause: str) -> tuple[_Command, ...] | None:
    """One clause as one or more commands, or None if it is not."""
    # A clause of filler alone, as in "Louder, please.", holds no command
    if not clause or _ONLY_FILLER.fullmatch(clause):
        return ()
    clause = _TRAILING.sub("", _LEADING.sub("", clause))
    if (single := _command(clause)) is not None:
        return (single,)
    words = tuple(clause.split())

    # A run without punctuation between, such as "louder louder". Each
    # starting word is searched once (``cache``), so the search costs at
    # most the square of the clause's length in calls to _command.
    @cache
    def run_from(start: int) -> tuple[_Command, ...] | None:
        if start == len(words):
            return ()
        for end in range(start + 1, len(words) + 1):
            first = _command(" ".join(words[start:end]))
            if first is not None and (rest := run_from(end)) is not None:
                return (first, *rest)
        return None

    return run_from(0)


def _clauses(transcript: str) -> list[str]:
    """The transcript in lower case, cut at its punctuation into clauses of
    plain words."""
    text = transcript.lower().replace("’", "'").replace("-", " ")
    # A word in another script becomes a token no pattern matches, so that
    # it cannot ride along with a command unseen.
    text = re.sub(r"[^\x00-\x7f]+", " ~ ", text)
    clauses = []
    for clause in re.split(r"[.,;:!?]+", text):
        # Digits and apostrophes are kept: "volume 5", "can't hear you".
        clauses.append(" ".join(re.sub(r"[^a-z0-9'~ ]+", " ", clause).split()))
    return clauses


# --- The answer --------------------------------------------------------------


@dataclass(frozen=True)
class VolumeChange:
    """What to do about a volume request."""

    #: The level to send to the device; the same as before when nothing moves.
    level: int
    #: The sentence spoken in answer, at the new level.
    reply: str
    #: The commands as understood, for the log: "+1 =9" is "louder, volume
    #: nine". Kept apart from the transcript, which is logged only when
    #: transcripts are.
    understood: str


def volume_request(transcript: str, current: int | None) -> VolumeChange | None:
    """Return the change ``transcript`` asks for, or None if it is not one.

    ``current`` is the level the device stated. Without a valid one, from an
    older device, nothing is taken as a volume request: the device would not
    understand the answer.
    """
    if current is None or not VOLUME_MIN <= current <= VOLUME_MAX:
        return None
    commands: list[_Command] = []
    for clause in _clauses(transcript):
        found = _clause_commands(clause)
        if found is None:
            return None
        commands.extend(found)
    if not commands:
        return None

    # Applied in order, noting what the reply must mention
    level = current
    met_end = 0             # +1 or -1: a step met that end of the scale
    off_scale = False       # a named level off the scale was ignored
    for command in commands:
        if isinstance(command, _Step):
            wanted = level + command.delta
            level = min(max(wanted, VOLUME_MIN), VOLUME_MAX)
            met_end = command.delta if wanted != level else met_end
        elif command.on_scale:
            level = command.level
        else:
            off_scale = True

    notes = []
    if met_end:
        notes.append(f"That's as {'loud' if met_end > 0 else 'quiet'} as I go.")
    if off_scale:
        notes.append(
            f"The volume goes from {spoken_number(VOLUME_MIN)}"
            f" to {spoken_number(VOLUME_MAX)}."
        )
    # The level is said whenever it moved, and when there is nothing else to say
    if level != current or not notes:
        notes.insert(0, f"Volume {spoken_number(level)}.")
    return VolumeChange(level, " ".join(notes), " ".join(map(str, commands)))
