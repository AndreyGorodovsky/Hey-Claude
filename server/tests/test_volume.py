"""Volume requests recognised from the transcript."""

import pytest

from server.pipeline.volume import volume_request
from server.protocol import VOLUME_MAX, VOLUME_MIN
from server.spoken import spoken_number


@pytest.mark.parametrize(
    "transcript",
    [
        "Louder.",
        "Louder, please.",
        "Can you speak up?",
        "Turn it up a bit.",
        "Turn the volume up",
        "Volume up!",
        "Could you be louder please",
        "Increase the volume.",
        "A bit louder.",
        "Make it louder.",
        "Turn the sound up.",
        "It's too quiet.",
        "Claude, louder.",
        "I can't hear you.",
        "Make voice louder.",
        "Make your voice louder.",
        "Your voice is too quiet.",
        "More sound.",
    ],
)
def test_louder_raises_the_level_by_one(transcript):
    change = volume_request(transcript, 5)
    assert change is not None
    assert change.level == 6
    assert change.reply == "Volume six."


@pytest.mark.parametrize(
    "transcript",
    [
        "Quieter.",
        "Softer please",
        "Turn it down.",
        "Volume down",
        "Lower the volume.",
        "Make it quieter please.",
        "Too loud.",
        "Turn it down a notch.",
        "Turn the sound down.",
        "Make your voice quieter.",
        "Less sound.",
    ],
)
def test_quieter_lowers_the_level_by_one(transcript):
    change = volume_request(transcript, 5)
    assert change is not None
    assert change.level == 4


@pytest.mark.parametrize(
    ("transcript", "level"),
    [
        # Speech-to-text writes numbers as words (stt/deepgram.py); digits
        # are understood as well.
        ("Volume five.", 5),
        ("Volume 5.", 5),
        ("Set the volume to 3.", 3),
        ("Set volume to ten", 10),
        ("Volume seven out of ten.", 7),
        ("Turn it up to five.", 5),
        ("Set it to two.", 2),
        # Every family of phrases takes "sound" and "voice" as well
        ("Set the sound to four.", 4),
        ("Maximum volume.", 10),
        ("Volume max", 10),
        ("Volume to the max.", 10),
        ("Sound to the max.", 10),
        ("Turn it all the way up.", 10),
        ("Turn the sound all the way up.", 10),
        ("Minimum volume", 1),
        ("Volume to lowest", 1),
    ],
)
def test_a_level_can_be_named(transcript, level):
    change = volume_request(transcript, 5)
    assert change is not None
    assert change.level == level


@pytest.mark.parametrize(
    ("transcript", "current", "level", "reply"),
    [
        ("Volume three volume three", 9, 3, "Volume three."),
        ("Volume three. Volume three.", 9, 3, "Volume three."),
        ("Set volume nine volume nine", 1, 9, "Volume nine."),
        ("Louder, volume nine.", 1, 9, "Volume nine."),
        ("Louder, louder.", 5, 7, "Volume seven."),
        ("Louder louder", 5, 7, "Volume seven."),
        # A run that comes back where it started is not an end of the scale
        ("Louder, quieter.", 5, 5, "Volume five."),
        ("Volume five, quieter.", 4, 4, "Volume four."),
        # A step that meets the end is said, after the level it reached
        ("Louder, louder.", 9, 10, "Volume ten. That's as loud as I go."),
    ],
)
def test_a_run_of_commands_is_applied_in_order(transcript, current, level, reply):
    change = volume_request(transcript, current)
    assert change is not None
    assert (change.level, change.reply) == (level, reply)


def test_the_ends_of_the_scale_are_said_and_not_passed():
    loudest = volume_request("Louder.", VOLUME_MAX)
    quietest = volume_request("Quieter.", VOLUME_MIN)
    assert loudest is not None and loudest.level == VOLUME_MAX
    assert loudest.reply == "That's as loud as I go."
    assert quietest is not None and quietest.level == VOLUME_MIN
    assert quietest.reply == "That's as quiet as I go."


SCALE = (
    f"The volume goes from {spoken_number(VOLUME_MIN)}"
    f" to {spoken_number(VOLUME_MAX)}."
)


@pytest.mark.parametrize(
    "transcript",
    ["Volume 11.", "Volume 0", "Set the volume to 20", "Turn it up to eleven."],
)
def test_a_level_off_the_scale_changes_nothing(transcript):
    change = volume_request(transcript, 4)
    assert change is not None
    assert change.level == 4
    assert change.reply == SCALE


def test_a_level_off_the_scale_does_not_hide_a_change_before_it():
    change = volume_request("Louder, volume eleven.", 5)
    assert change is not None
    assert change.level == 6
    assert change.reply == f"Volume six. {SCALE}"


def test_the_commands_are_described_for_the_log():
    change = volume_request("Louder, volume nine.", 5)
    assert change is not None
    assert change.understood == "+1 =9"


@pytest.mark.parametrize(
    "transcript",
    [
        "How do I turn up the volume on my television?",
        "Why is the music louder than the speech?",
        "What does volume mean?",
        "Is it too loud?",
        "Volume",
        "Volume one hundred",
        "Five.",
        "Please.",
        "Turn it up to eleven, the song is great.",
        "What is the capital of France?",
        "",
        # Correction words are deliberately not handled (volume.py)
        "Volume two, no, volume four.",
        # A word in another script cannot ride along with a command
        "Louder, 李.",
    ],
)
def test_anything_else_goes_to_the_model(transcript):
    assert volume_request(transcript, 5) is None


@pytest.mark.parametrize("current", [None, 0, 11])
def test_without_a_valid_level_from_the_device_nothing_is_taken(current):
    assert volume_request("Louder.", current) is None
