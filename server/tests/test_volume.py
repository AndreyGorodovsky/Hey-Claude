"""Volume requests recognised from the transcript."""

import pytest

from server.pipeline.volume import volume_request


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
    ],
)
def test_quieter_lowers_the_level_by_one(transcript):
    change = volume_request(transcript, 5)
    assert change is not None
    assert change.level == 4


@pytest.mark.parametrize(
    ("transcript", "level"),
    [
        ("Volume five.", 5),
        ("Volume 5.", 5),
        ("Set the volume to 3.", 3),
        ("Set volume to ten", 10),
        ("Volume seven out of ten.", 7),
        ("Maximum volume.", 10),
        ("Volume max", 10),
        ("Volume to the max.", 10),
        ("Turn it all the way up.", 10),
        ("Minimum volume", 1),
        ("Volume to lowest", 1),
        ("Turn it up to five.", 5),
        ("Set it to two.", 2),
    ],
)
def test_a_level_can_be_named(transcript, level):
    change = volume_request(transcript, 5)
    assert change is not None
    assert change.level == level


def test_the_ends_of_the_scale_are_said_and_not_passed():
    loudest = volume_request("Louder.", 10)
    quietest = volume_request("Quieter.", 1)
    assert loudest is not None and loudest.level == 10
    assert "loud" in loudest.reply
    assert quietest is not None and quietest.level == 1
    assert "quiet" in quietest.reply


@pytest.mark.parametrize(
    "transcript", ["Volume 11.", "Volume 0", "Set the volume to 20"]
)
def test_a_level_off_the_scale_changes_nothing(transcript):
    change = volume_request(transcript, 4)
    assert change is not None
    assert change.level == 4
    assert "one to ten" in change.reply


@pytest.mark.parametrize(
    "transcript",
    [
        "How do I turn up the volume on my television?",
        "Why is the music louder than the speech?",
        "What does volume mean?",
        "Volume",
        "Five.",
        "Turn it up to eleven, the song is great.",
        "What is the capital of France?",
        "",
    ],
)
def test_anything_else_goes_to_the_model(transcript):
    assert volume_request(transcript, 5) is None


@pytest.mark.parametrize("current", [None, 0, 11])
def test_without_a_valid_level_from_the_device_nothing_is_taken(current):
    assert volume_request("Louder.", current) is None


@pytest.mark.parametrize(
    ("transcript", "current", "level"),
    [
        ("Volume three volume three", 9, 3),
        ("Volume three. Volume three.", 9, 3),
        ("Set volume nine volume nine", 1, 9),
        ("Louder volume nine.", 1, 9),
        ("Louder, louder.", 5, 7),
        ("Volume two, no, volume four.", 5, None),
        ("Make voice louder.", 5, 6),
        ("Make your voice quieter.", 5, 4),
        ("Your voice is too quiet.", 5, 6),
    ],
)
def test_repeated_and_corrected_commands_end_on_the_last(transcript, current, level):
    change = volume_request(transcript, current)
    if level is None:
        assert change is None
    else:
        assert change is not None
        assert change.level == level


def test_a_number_spoken_as_words_is_understood():
    # Speech-to-text writes numbers as words (stt/deepgram.py, punctuate)
    change = volume_request("Set the volume to five.", 8)
    assert change is not None and change.level == 5
