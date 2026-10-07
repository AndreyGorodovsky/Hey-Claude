"""The turn pipeline, driven directly with stand-in services."""

import asyncio

from server.conversation import Exchange
from server.llm import LanguageModelError, ReplyRefused
from server.pipeline import ConnectionLost, run_turn
from server.protocol import ErrorCode
from server.tests.fakes import (
    FRAME,
    NOON,
    FakeLlm,
    FakeStt,
    FakeTts,
    RecordingIO,
    make_deps,
)
from server.tts import TextToSpeechError

QUESTION = "What is the capital of France?"
ANSWER = "Paris. It is on the Seine."


async def run(deps, frames: int = 50, io: RecordingIO | None = None) -> RecordingIO:
    """Run one turn with ``frames`` capture frames already waiting."""
    await deps.store.initialise()
    audio: asyncio.Queue[bytes] = asyncio.Queue()
    for _ in range(frames):
        audio.put_nowait(FRAME)
    io = io or RecordingIO()
    await run_turn("dev", audio, io, deps)
    return io


async def test_a_turn_reports_its_steps_in_order_and_is_recorded(tmp_path):
    tts = FakeTts()
    deps = make_deps(tmp_path, tts=tts)
    io = await run(deps)

    assert io.steps == ["capture_done", "reply_audio", "reply_done"]
    # One synthesis per sentence, in order, and all of its audio passed on.
    assert tts.spoken == ["Paris.", "It is on the Seine."]
    assert io.audio_bytes == sum(map(len, tts.spoken)) * 500
    assert await deps.store.history("dev", NOON) == [Exchange(QUESTION, ANSWER)]


async def test_earlier_exchanges_are_sent_to_the_model(tmp_path):
    llm = FakeLlm()
    deps = make_deps(tmp_path, llm=llm)
    await run(deps)
    await run(deps)

    assert llm.calls[0] == ([], QUESTION)
    assert llm.calls[1][0] == [Exchange(QUESTION, ANSWER)]


async def test_nothing_said_ends_the_turn_without_a_reply(tmp_path):
    llm = FakeLlm()
    deps = make_deps(
        tmp_path,
        stt=FakeStt(start_after=None, end_after=None),
        llm=llm,
        no_speech_timeout_seconds=0.05,
    )
    io = await run(deps)

    assert io.steps == ["capture_done", "reply_done"]
    assert llm.calls == []
    assert await deps.store.history("dev", NOON) == []


async def test_the_capture_limit_ends_a_request_that_never_ends(tmp_path):
    # Speech starts and is never heard to finish.
    deps = make_deps(tmp_path, stt=FakeStt(end_after=None), max_capture_seconds=0.05)
    io = await run(deps)

    assert io.steps == ["capture_done", "reply_audio", "reply_done"]


async def test_the_capture_limit_counts_from_the_start_of_the_turn(tmp_path):
    class SlowToOpen(FakeStt):
        async def open(self, sample_rate):
            await asyncio.sleep(0.2)
            return await super().open(sample_rate)

    # The limit has passed by the time speech-to-text is reached, so the
    # capture ends at once instead of running a further full limit.
    deps = make_deps(
        tmp_path, stt=SlowToOpen(end_after=None), max_capture_seconds=0.1
    )
    await deps.store.initialise()
    started = asyncio.get_running_loop().time()
    io = RecordingIO()
    await run_turn("dev", asyncio.Queue(), io, deps)

    assert io.steps[0] == "capture_done"
    assert asyncio.get_running_loop().time() - started < 0.29


async def test_speech_to_text_failing_to_connect_is_reported(tmp_path):
    io = await run(make_deps(tmp_path, stt=FakeStt(fail_on_open=True)))

    assert io.steps == ["fail"]
    assert io.failure is ErrorCode.STT_FAILED


async def test_speech_to_text_failing_mid_stream_is_reported_at_once(tmp_path):
    stt = FakeStt(fail_after=3, start_after=None, end_after=None)
    # Long timeouts: the failure must end the wait, not a timeout.
    deps = make_deps(
        tmp_path, stt=stt, max_capture_seconds=30, no_speech_timeout_seconds=30
    )
    io = await asyncio.wait_for(run(deps), timeout=2)

    assert io.steps == ["fail"]
    assert io.failure is ErrorCode.STT_FAILED
    assert stt.streams[0].closed


async def test_model_failure_after_a_sentence_ends_in_a_failure(tmp_path):
    deps = make_deps(tmp_path, llm=FakeLlm(error=LanguageModelError("down")))
    io = await run(deps)

    assert io.steps == ["capture_done", "reply_audio", "fail"]
    assert io.failure is ErrorCode.LLM_FAILED
    # A failed turn leaves no trace in the conversation.
    assert await deps.store.history("dev", NOON) == []


async def test_synthesis_failure_is_reported_before_any_audio(tmp_path):
    deps = make_deps(tmp_path, tts=FakeTts(error=TextToSpeechError("down")))
    io = await run(deps)

    assert io.steps == ["capture_done", "fail"]
    assert io.failure is ErrorCode.TTS_FAILED
    assert await deps.store.history("dev", NOON) == []


async def test_refusal_with_no_text_speaks_a_fixed_line_and_is_not_recorded(tmp_path):
    tts = FakeTts()
    deps = make_deps(
        tmp_path, llm=FakeLlm(pieces=(), error=ReplyRefused("declined")), tts=tts
    )
    io = await run(deps)

    assert io.steps == ["capture_done", "reply_audio", "reply_done"]
    assert len(tts.spoken) == 1
    assert await deps.store.history("dev", NOON) == []


async def test_empty_reply_is_a_model_failure(tmp_path):
    io = await run(make_deps(tmp_path, llm=FakeLlm(pieces=("  ",))))

    assert io.steps == ["capture_done", "fail"]
    assert io.failure is ErrorCode.LLM_FAILED


async def test_unexpected_failure_is_reported_as_internal(tmp_path):
    io = await run(make_deps(tmp_path, llm=FakeLlm(error=RuntimeError("bug"))))

    assert io.failure is ErrorCode.INTERNAL


async def test_a_lost_connection_ends_the_turn_quietly(tmp_path, caplog):
    class GoneAfterCapture(RecordingIO):
        async def reply_audio(self, pcm: bytes) -> None:
            raise ConnectionLost

    stt = FakeStt()
    deps = make_deps(tmp_path, stt=stt)
    io = await run(deps, io=GoneAfterCapture())

    # Nothing further is reported, nothing is recorded, nothing is an error.
    assert io.steps == ["capture_done"]
    assert await deps.store.history("dev", NOON) == []
    assert stt.streams[0].closed
    assert not [r for r in caplog.records if r.levelname == "ERROR"]


async def test_a_cancelled_turn_releases_its_stream(tmp_path):
    stt = FakeStt(start_after=None, end_after=None)
    deps = make_deps(
        tmp_path, stt=stt, max_capture_seconds=30, no_speech_timeout_seconds=30
    )
    await deps.store.initialise()
    task = asyncio.create_task(run_turn("dev", asyncio.Queue(), RecordingIO(), deps))
    await asyncio.sleep(0.05)
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)

    assert task.cancelled()
    assert stt.streams[0].closed


async def test_the_next_sentence_is_synthesised_while_the_first_is_being_sent(tmp_path):
    class HeldUp(RecordingIO):
        def __init__(self) -> None:
            super().__init__()
            self.release = asyncio.Event()

        async def reply_audio(self, pcm: bytes) -> None:
            await super().reply_audio(pcm)
            await self.release.wait()

    tts = FakeTts()
    io = HeldUp()
    deps = make_deps(tmp_path, tts=tts)
    turn = asyncio.create_task(run(deps, io=io))
    # The first piece of the first sentence is still being sent; the second
    # sentence is synthesised regardless.
    async with asyncio.timeout(2):
        while len(tts.spoken) < 2:
            await asyncio.sleep(0.01)
    assert io.steps == ["capture_done", "reply_audio"]

    io.release.set()
    await turn
    assert io.steps == ["capture_done", "reply_audio", "reply_done"]


async def test_synthesis_waits_when_enough_audio_is_already_waiting(tmp_path):
    class Stopped(RecordingIO):
        def __init__(self) -> None:
            super().__init__()
            self.release = asyncio.Event()

        async def reply_audio(self, pcm: bytes) -> None:
            await super().reply_audio(pcm)
            await self.release.wait()

    # Each sentence is a second of audio; a second may wait.
    tts = FakeTts(bytes_per_character=4800)
    llm = FakeLlm(pieces=("Aaaaaaaaa. ", "Bbbbbbbbb. ", "Ccccccccc. ", "Ddddddddd."))
    io = Stopped()
    deps = make_deps(tmp_path, llm=llm, tts=tts, synthesis_ahead_seconds=1.0)
    turn = asyncio.create_task(run(deps, io=io))
    await asyncio.sleep(0.2)
    # Nothing is being sent, so synthesis stops once a second is waiting.
    assert 2 <= len(tts.spoken) < 4

    io.release.set()
    await turn
    assert len(tts.spoken) == 4
    assert io.audio_bytes == 40 * 4800


async def test_a_reply_that_does_not_start_in_time_is_a_failure(tmp_path):
    # The model never produces anything.
    llm = FakeLlm(gate=asyncio.Event())
    deps = make_deps(tmp_path, llm=llm, first_audio_seconds=0.1)
    io = await asyncio.wait_for(run(deps), timeout=2)

    assert io.steps == ["capture_done", "fail"]
    assert io.failure is ErrorCode.LLM_FAILED


async def test_text_without_audio_in_time_is_a_synthesis_failure(tmp_path):
    class Stuck(FakeTts):
        async def synthesize(self, text, sample_rate):
            await asyncio.sleep(30)
            yield b""

    deps = make_deps(tmp_path, tts=Stuck(), first_audio_seconds=0.2)
    io = await asyncio.wait_for(run(deps), timeout=2)

    assert io.steps == ["capture_done", "fail"]
    assert io.failure is ErrorCode.TTS_FAILED
