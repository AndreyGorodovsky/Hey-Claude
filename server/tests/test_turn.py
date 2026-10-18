"""The turn pipeline, driven directly with stand-in services."""

import asyncio
import time

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


async def test_a_completed_turn_logs_where_its_wait_went(tmp_path, caplog):
    caplog.set_level("INFO", logger="server.pipeline.turn")
    await run(make_deps(tmp_path))

    lines = [r.getMessage() for r in caplog.records if "first audio" in r.getMessage()]
    assert len(lines) == 1
    for part in ("speech-to-text reached", "first text", "first sentence"):
        assert part in lines[0]


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


async def run_with_volume(deps, volume: int | None) -> RecordingIO:
    await deps.store.initialise()
    audio: asyncio.Queue[bytes] = asyncio.Queue()
    for _ in range(50):
        audio.put_nowait(FRAME)
    io = RecordingIO()
    await run_turn("dev", audio, io, deps, volume)
    return io


async def test_a_volume_request_is_answered_without_the_model(tmp_path):
    llm, tts = FakeLlm(), FakeTts()
    stt = FakeStt(transcript="Louder, please.")
    deps = make_deps(tmp_path, stt=stt, llm=llm, tts=tts)
    io = await run_with_volume(deps, 7)

    # The new level reaches the device before the reply that confirms it.
    assert io.steps == ["capture_done", "set_volume", "reply_audio", "reply_done"]
    assert io.volume == 8
    assert tts.spoken == ["Volume eight."]
    assert llm.calls == []
    assert await deps.store.history("dev", NOON) == []


async def test_a_volume_request_at_the_end_of_the_scale_sends_no_level(tmp_path):
    deps = make_deps(tmp_path, stt=FakeStt(transcript="Louder."))
    io = await run_with_volume(deps, 10)

    assert io.steps == ["capture_done", "reply_audio", "reply_done"]
    assert io.volume is None


async def test_a_device_that_states_no_volume_is_answered_by_the_model(tmp_path):
    llm = FakeLlm()
    deps = make_deps(tmp_path, stt=FakeStt(transcript="Louder."), llm=llm)
    io = await run_with_volume(deps, None)

    assert "set_volume" not in io.steps
    assert llm.calls == [([], "Louder.")]


async def test_a_volume_confirmation_that_cannot_be_spoken_fails_the_turn(tmp_path):
    tts = FakeTts(error=TextToSpeechError("down"))
    deps = make_deps(tmp_path, stt=FakeStt(transcript="Quieter."), tts=tts)
    io = await run_with_volume(deps, 5)

    # The level was sent and is kept; only the confirmation is lost.
    assert io.steps == ["capture_done", "set_volume", "fail"]
    assert io.failure is ErrorCode.TTS_FAILED


# --- Interrupted by the person ----------------------------------------------


class Interrupts(RecordingIO):
    """Sets the turn's interrupt at a chosen step, and holds the reply at
    its first audio, as pacing to playback would."""

    def __init__(self, at: str) -> None:
        super().__init__()
        self.at = at
        self.interrupt = asyncio.Event()

    async def capture_done(self) -> None:
        await super().capture_done()
        if self.at == "capture_done":
            self.interrupt.set()

    async def reply_audio(self, pcm: bytes) -> None:
        await super().reply_audio(pcm)
        if self.at == "reply_audio":
            self.interrupt.set()
            await asyncio.Event().wait()


async def interrupted_turn(deps, at: str, volume: int | None = None) -> Interrupts:
    await deps.store.initialise()
    audio: asyncio.Queue[bytes] = asyncio.Queue()
    for _ in range(50):
        audio.put_nowait(FRAME)
    io = Interrupts(at)
    async with asyncio.timeout(5):
        await run_turn("dev", audio, io, deps, volume, io.interrupt)
    return io


async def test_an_interrupted_reply_is_recorded_as_far_as_it_was_sent(tmp_path):
    deps = make_deps(tmp_path)
    io = await interrupted_turn(deps, "reply_audio")

    # The second sentence was synthesised, perhaps, but never sent
    assert await deps.store.history("dev", NOON) == [
        Exchange(QUESTION, "Paris. [interrupted]")
    ]
    assert io.steps == ["capture_done", "reply_audio", "interrupted"]


async def test_a_request_interrupted_before_any_reply_is_recorded_with_the_mark(tmp_path):
    deps = make_deps(tmp_path, llm=FakeLlm(gate=asyncio.Event()))
    # Set at the end of the capture, read once the model is waiting
    io = await interrupted_turn(deps, "capture_done")

    assert io.steps[-1] == "interrupted"
    history = await deps.store.history("dev", NOON)
    # Interrupted before the transcript was taken to the model, or after
    assert history in ([], [Exchange(QUESTION, "[interrupted]")])


async def test_a_request_interrupted_while_capturing_is_not_recorded(tmp_path):
    stt = FakeStt(start_after=None, end_after=None)
    deps = make_deps(
        tmp_path, stt=stt, max_capture_seconds=30, no_speech_timeout_seconds=30
    )
    await deps.store.initialise()
    io = RecordingIO()
    interrupt = asyncio.Event()
    asyncio.get_running_loop().call_later(0.05, interrupt.set)
    await run_turn("dev", asyncio.Queue(), io, deps, None, interrupt)

    assert io.steps == ["interrupted"]
    assert await deps.store.history("dev", NOON) == []
    assert stt.streams[0].closed


async def test_an_interrupt_during_the_captures_cleanup_is_not_lost(tmp_path):
    # The capture is over and its audio pump is slow to stop: an interrupt
    # landing then must stop the turn, not be swallowed by the cleanup
    stt = FakeStt(slow_stop_after_end=0.2)
    llm = FakeLlm()
    deps = make_deps(tmp_path, stt=stt, llm=llm)
    io = await interrupted_turn(deps, "capture_done")

    assert llm.calls == []
    assert io.steps == ["capture_done", "interrupted"]


async def test_the_device_is_told_before_the_work_has_finished_stopping(tmp_path):
    class SlowToStop(FakeLlm):
        async def stream_reply(self, history, user_text):
            try:
                async for piece in super().stream_reply(history, user_text):
                    yield piece
            finally:
                # As stopping the Claude Code process can take a while
                await asyncio.shield(asyncio.sleep(1.0))

    llm = SlowToStop(gate=asyncio.Event())
    deps = make_deps(tmp_path, llm=llm)
    await deps.store.initialise()
    audio: asyncio.Queue[bytes] = asyncio.Queue()
    for _ in range(50):
        audio.put_nowait(FRAME)
    told: list[float] = []

    class Timed(RecordingIO):
        async def interrupted(self) -> None:
            told.append(time.monotonic())

    interrupt = asyncio.Event()
    turn = asyncio.create_task(run_turn("dev", audio, Timed(), deps, None, interrupt))
    while not llm.calls:
        await asyncio.sleep(0.01)
    started = time.monotonic()
    interrupt.set()
    await turn

    assert told and told[0] - started < 0.2
    assert time.monotonic() - started >= 0.9


async def test_an_interrupted_refusal_is_not_recorded(tmp_path):
    llm = FakeLlm(pieces=(), error=ReplyRefused("no"))
    deps = make_deps(tmp_path, llm=llm)
    await interrupted_turn(deps, "reply_audio")

    assert await deps.store.history("dev", NOON) == []


async def test_an_interrupted_volume_request_is_not_recorded(tmp_path):
    deps = make_deps(tmp_path, stt=FakeStt(transcript="Louder."))
    io = await interrupted_turn(deps, "reply_audio", volume=5)

    assert io.steps[-1] == "interrupted"
    assert await deps.store.history("dev", NOON) == []


async def test_an_interrupt_after_the_turn_has_ended_changes_nothing(tmp_path):
    deps = make_deps(tmp_path)
    await deps.store.initialise()
    audio: asyncio.Queue[bytes] = asyncio.Queue()
    for _ in range(50):
        audio.put_nowait(FRAME)
    io = RecordingIO()
    interrupt = asyncio.Event()
    await run_turn("dev", audio, io, deps, None, interrupt)
    interrupt.set()

    assert io.steps == ["capture_done", "reply_audio", "reply_done"]
    assert await deps.store.history("dev", NOON) == [Exchange(QUESTION, ANSWER)]
