"""The Claude Code client, against a stand-in for the tool.

In three parts. What is made of the tool's events is tested by feeding
events to the reader, with no process. Starting, timing and stopping a
process are tested with the stand-in, ``fake_claude_cli.py``, run as a real
process. The last part covers what the process is started with, the prompt
and the settings.

No test here runs the real tool or needs a login. What the real tool
prints, and whether its options lock it down as intended, is checked by
hand with ``server/tools/claude_cli_bench.py``.
"""

import asyncio
import json
import os
import sys
from pathlib import Path

import pytest
from pydantic import ValidationError

from server.config import Settings
from server.conversation import Exchange
from server.llm import LanguageModelError, ReplyRefused
from server.llm import claude_code
from server.llm.claude_code import (
    ClaudeCodeLanguageModel,
    FetchPolicy,
    build_command,
    build_prompt,
    child_environment,
    lockdown_problem,
)
from server.llm.claude_code_prompt import system_prompt_with_search
from server.llm.prompt import SYSTEM_PROMPT
from server.pipeline.sentences import SentenceSplitter
from server.tests.fake_claude_cli import (
    block_start,
    block_stop,
    init_event,
    result_event,
    stream_event,
    text_block,
    text_delta,
    thinking_delta,
    tool_call,
)

FAKE = Path(__file__).with_name("fake_claude_cli.py")

SEARCH_ONLY = FetchPolicy()
ANY_SITE = FetchPolicy(any_site=True)


# --- What is made of the tool's events ---------------------------------------


def read(*events: dict, policy: FetchPolicy = SEARCH_ONLY) -> str:
    """Feed events to a reader; return all it would have spoken."""
    reader = claude_code._ReplyReader(policy.tools())
    spoken = "".join(reader.feed(event) for event in events) + reader.finish()
    if reader.result is not None:
        reader.raise_for_outcome()
    return spoken


def test_text_is_spoken_and_nothing_else_is():
    spoken = read(
        init_event(),
        block_start("thinking"),
        thinking_delta("Easy."),
        block_stop(),
        *text_block("Paris. ", "It is on ", "the Seine."),
        # The tool repeats each finished message whole; not to be said twice.
        {"type": "assistant", "message": {"content": [{"type": "text"}]}},
        result_event(),
    )
    # The line break is the client's: it marks the end of a block of text.
    assert spoken == "Paris. It is on the Seine.\n"


def test_text_either_side_of_a_tool_call_is_kept_apart():
    spoken = read(
        init_event(),
        *text_block("Let me check."),
        *tool_call(),
        *text_block("It is sunny."),
        result_event(),
    )
    assert spoken == "Let me check.\nIt is sunny.\n"


def test_what_is_said_before_a_search_is_ended_at_once():
    # Seen on the device on 2026-10-12: "Let me check." was spoken only
    # when the answer came, because nothing ended its sentence until then.
    reader = claude_code._ReplyReader(SEARCH_ONLY.tools())
    splitter = SentenceSplitter()
    sentences: list[str] = []
    for event in [init_event(), *text_block("Let me check."), block_start("tool_use")]:
        sentences += splitter.feed(reader.feed(event))
    assert sentences == ["Let me check."]


def test_a_block_with_no_text_adds_nothing():
    # A bare line break would count as reply text further on.
    assert read(init_event(), block_start("text"), block_stop()) == ""
    assert read(init_event(), *tool_call()) == ""


def test_text_from_a_helper_of_the_tool_is_not_the_reply():
    spoken = read(
        init_event(),
        text_delta("A summary of the page.", parent="toolu_1"),
        *text_block("It is sunny."),
    )
    assert spoken == "It is sunny.\n"


def test_a_closing_list_of_sources_is_not_spoken():
    spoken = read(
        init_event(),
        # Cut as the real tool may cut it, inside the word, after a reply
        # whose own first word starts with the same letters.
        *text_block("So it is", " sunny.\n", "\nSou", "rces: [A](https://a.example)"),
        result_event(),
    )
    assert spoken == "So it is sunny.\n\n"


def test_an_answer_after_a_list_of_sources_and_a_search_is_spoken():
    spoken = read(
        init_event(),
        *text_block("Let me check.\n\nSources: [A](https://a.example)"),
        *tool_call(),
        *text_block("It is sunny."),
    )
    assert spoken == "Let me check.\n\nIt is sunny.\n"


def test_the_sources_filter_holds_back_only_what_could_be_the_word():
    sources = claude_code._SourcesFilter()
    assert sources.feed("Sou") == ""
    assert sources.feed("th of here. ") == "South of here. "
    # Not at the start of a line: an ordinary use of the word.
    assert sources.feed("Sources: differ.") == "Sources: differ."
    assert sources.feed("\n  S") == "\n"
    assert sources.flush() == "  S"
    assert sources.dropped == 0

    sources = claude_code._SourcesFilter()
    assert sources.feed("Done.\n\nSources:\n- x") == "Done.\n\n"
    assert sources.feed("more") == ""
    assert sources.flush() == ""
    assert sources.dropped == len("Sources:\n- xmore")


def test_a_failure_the_tool_reports_is_a_failure():
    with pytest.raises(LanguageModelError, match="Not logged in"):
        read(init_event(), result_event(is_error=True, result="Not logged in"))
    with pytest.raises(LanguageModelError, match="error_max_turns"):
        read(init_event(), result_event(subtype="error_max_turns"))


def test_a_failure_after_text_is_still_a_failure():
    reader = claude_code._ReplyReader(SEARCH_ONLY.tools())
    events = [init_event(), *text_block("Paris."), result_event(is_error=True)]
    assert "".join(reader.feed(event) for event in events) == "Paris.\n"
    with pytest.raises(LanguageModelError):
        reader.raise_for_outcome()


def test_a_declined_request_is_reported_as_declined():
    declined = stream_event({"type": "message_delta", "delta": {"stop_reason": "refusal"}})
    with pytest.raises(ReplyRefused):
        read(init_event(), declined, result_event())
    with pytest.raises(ReplyRefused):
        read(init_event(), result_event(stop_reason="refusal"))


@pytest.mark.parametrize(
    "event",
    [
        {"type": "stream_event", "event": "not an object"},
        stream_event({"type": "content_block_delta", "delta": "not an object"}),
        stream_event({"type": "content_block_start", "content_block": 7}),
        {"type": "system", "subtype": "init", "tools": [{}], "mcp_servers": []},
    ],
)
def test_an_event_of_another_shape_is_a_failure_of_the_model(event):
    # Not an unexpected failure of the server: the tool has changed.
    with pytest.raises(LanguageModelError):
        read(init_event(), event)


@pytest.mark.parametrize(
    "first",
    [
        init_event(tools=["WebSearch", "Bash"]),
        init_event(tools=["WebSearch", "WebFetch"]),
        init_event(servers=[{"name": "mail", "status": "connected"}]),
        init_event(mode="default"),
        # No first event at all: nothing is known of what it started with.
        block_start("text"),
    ],
)
def test_nothing_is_spoken_by_a_process_that_is_not_locked_down(first):
    reader = claude_code._ReplyReader(SEARCH_ONLY.tools())
    with pytest.raises(LanguageModelError):
        for event in [first, *text_block("This must not be spoken.")]:
            assert reader.feed(event) == ""


def test_the_page_reading_tool_is_accepted_when_pages_may_be_read():
    events = [init_event(tools=["WebSearch", "WebFetch"]), *text_block("It is sunny.")]
    assert read(*events, policy=ANY_SITE) == "It is sunny.\n"
    assert read(*events, policy=FetchPolicy(hosts=("wttr.in",))) == "It is sunny.\n"


def test_what_the_process_started_with_is_judged():
    def problem(allowed=SEARCH_ONLY.tools(), **changes):
        init = {"tools": ["WebSearch"], "mcp_servers": [], "permissionMode": "dontAsk"}
        return lockdown_problem({**init, **changes}, allowed)

    assert problem() is None
    assert problem(tools=[]) is None
    assert "Bash" in problem(tools=["Bash"])
    assert "WebFetch" in problem(tools=["WebSearch", "WebFetch"])
    assert problem(ANY_SITE.tools(), tools=["WebSearch", "WebFetch"]) is None
    assert "mail" in problem(mcp_servers=[{"name": "mail"}])
    # Only in this mode is a page from an unlisted site refused.
    assert "default" in problem(permissionMode="default")
    assert "bypassPermissions" in problem(permissionMode="bypassPermissions")
    # A first event that does not say proves nothing.
    assert lockdown_problem({}, ANY_SITE.tools()) is not None
    assert lockdown_problem({"tools": [], "mcp_servers": []}, ANY_SITE.tools()) is not None


# --- The process -------------------------------------------------------------


def make(scenario: str, report: Path | None = None) -> ClaudeCodeLanguageModel:
    command = [sys.executable, str(FAKE), scenario]
    if report is not None:
        command.append(str(report))
    return ClaudeCodeLanguageModel(
        model="test-model", effort="low", max_tokens=1234, command=command
    )


async def reply(
    scenario: str, report: Path | None = None, history: list[Exchange] | None = None
) -> list[str]:
    """Collect the pieces of one reply from the stand-in's ``scenario``."""
    llm = make(scenario, report)
    try:
        return [
            piece
            async for piece in llm.stream_reply(history or [], "What is the capital?")
        ]
    finally:
        await llm.aclose()


def process_is_running(pid: int) -> bool:
    if sys.platform == "win32":
        import ctypes

        # SYNCHRONIZE access is enough to ask whether the process has ended.
        handle = ctypes.windll.kernel32.OpenProcess(0x00100000, False, pid)
        if not handle:
            return False
        ended = ctypes.windll.kernel32.WaitForSingleObject(handle, 0) == 0
        ctypes.windll.kernel32.CloseHandle(handle)
        return not ended
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


@pytest.fixture
def started(monkeypatch) -> list[asyncio.subprocess.Process]:
    """The processes the client starts during a test."""
    processes: list[asyncio.subprocess.Process] = []
    real = asyncio.create_subprocess_exec

    async def spy(*args, **kwargs):
        process = await real(*args, **kwargs)
        processes.append(process)
        return process

    monkeypatch.setattr(asyncio, "create_subprocess_exec", spy)
    return processes


async def test_a_reply_arrives_in_pieces_as_the_process_prints_it(started):
    assert await reply("plain") == ["Paris. ", "It is on ", "the Seine.", "\n"]
    assert started[0].returncode == 0


async def test_a_reply_with_a_search_arrives_whole():
    assert "".join(await reply("search")) == "Let me check.\nIt is sunny.\n"


async def test_lines_that_are_not_events_are_skipped():
    assert "".join(await reply("noise")) == "Paris.\n"


async def test_the_request_and_history_arrive_on_standard_input(tmp_path):
    report = tmp_path / "report.json"
    history = [Exchange("Hello.", "Hello there.")]
    await reply("plain", report, history)
    stdin = json.loads(report.read_text(encoding="utf-8"))["stdin"]
    assert stdin == build_prompt(history, "What is the capital?")
    assert "Person: Hello." in stdin and "You: Hello there." in stdin
    assert stdin.endswith("What is the capital?")


def test_a_first_request_is_sent_bare():
    assert build_prompt([], "What is two plus two?") == "What is two plus two?"


async def test_a_process_that_is_not_locked_down_is_stopped_unheard(started):
    with pytest.raises(LanguageModelError, match="not locked down"):
        await reply("extra_tool")
    assert not process_is_running(started[0].pid)


async def test_a_failure_reported_by_the_process_is_a_failure():
    with pytest.raises(LanguageModelError, match="Not logged in"):
        await reply("error_result")


async def test_a_process_that_dies_is_a_failure_after_its_text():
    llm = make("dies")
    pieces: list[str] = []
    with pytest.raises(LanguageModelError, match=r"without a result \(exit 3\)"):
        async for piece in llm.stream_reply([], "Hello?"):
            pieces.append(piece)
    assert pieces == ["Paris.", "\n"]
    await llm.aclose()


async def test_a_process_that_cannot_run_says_why_in_the_failure():
    # As after an update of the tool that renamed one of its options.
    with pytest.raises(LanguageModelError, match="unknown option '--safe-mode'"):
        await reply("bad_option")


async def test_a_silent_process_is_stopped(monkeypatch, started):
    monkeypatch.setattr(claude_code, "_TEXT_SILENCE_SECONDS", 1.5)
    llm = make("hangs")
    pieces: list[str] = []
    with pytest.raises(LanguageModelError, match="said nothing"):
        async for piece in llm.stream_reply([], "Hello?"):
            pieces.append(piece)
    # The text came first, so it was the silence after it that was timed.
    assert pieces == ["Paris. ", "\n"]
    assert not process_is_running(started[0].pid)
    await llm.aclose()


async def test_a_process_that_prints_but_says_nothing_is_stopped(monkeypatch):
    # Lines alone do not count: a reply that searches for ever is silent.
    monkeypatch.setattr(claude_code, "_TEXT_SILENCE_SECONDS", 1.5)
    llm = make("busy_but_wordless")
    pieces: list[str] = []
    with pytest.raises(LanguageModelError, match="said nothing"):
        async for piece in llm.stream_reply([], "What is in the news?"):
            pieces.append(piece)
    assert pieces == ["Let me check.", "\n"]
    await llm.aclose()


async def test_a_tool_that_cannot_be_started_is_a_failure(tmp_path):
    llm = ClaudeCodeLanguageModel(
        model="m", effort="low", max_tokens=1, command=[str(tmp_path / "absent")]
    )
    with pytest.raises(LanguageModelError, match="could not start"):
        async for _ in llm.stream_reply([], "Hello?"):
            pass
    await llm.aclose()


def test_a_tool_that_is_not_installed_is_refused_at_start():
    with pytest.raises(LanguageModelError, match="not found"):
        ClaudeCodeLanguageModel(
            model="m", effort="low", max_tokens=1, executable="no-such-tool-here"
        )


@pytest.mark.parametrize("scenario", ["hangs", "search_hangs"])
async def test_an_abandoned_reply_stops_its_process(scenario, started):
    llm = make(scenario)
    got_text = asyncio.Event()

    async def consume() -> None:
        async for _ in llm.stream_reply([], "Hello?"):
            got_text.set()

    # As a turn does when its device disconnects: cancel the reader.
    task = asyncio.create_task(consume())
    await asyncio.wait_for(got_text.wait(), 10)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert started[0].returncode is not None
    assert not process_is_running(started[0].pid)
    await llm.aclose()


async def test_ending_a_process_passes_a_cancellation_on_and_still_stops_it(started):
    # The process has given its result and is taking its time to go: a
    # cancellation then must stop it, and must not be swallowed.
    process = await asyncio.create_subprocess_exec(
        sys.executable, "-c", "import time; time.sleep(600)"
    )
    ending = asyncio.create_task(claude_code._end(process, expected=True))
    await asyncio.sleep(0.3)
    ending.cancel()
    with pytest.raises(asyncio.CancelledError):
        await ending
    async with asyncio.timeout(10):
        await process.wait()
    assert not process_is_running(process.pid)


async def test_the_process_gets_no_credentials_and_an_empty_directory(
    tmp_path, monkeypatch
):
    monkeypatch.setenv("ANTHROPIC_API_KEY", "not-a-real-key")
    monkeypatch.setenv("DEEPGRAM_API_KEY", "not-a-real-key")
    monkeypatch.setenv("DEVICE_TOKENS", "{}")
    monkeypatch.setenv("CLAUDE_CODE_OAUTH_TOKEN", "not-a-real-token")
    report_file = tmp_path / "report.json"
    await reply("plain", report_file)
    report = json.loads(report_file.read_text(encoding="utf-8"))

    names = {name.upper() for name in report["env"]}
    assert not names & {
        "ANTHROPIC_API_KEY",
        "DEEPGRAM_API_KEY",
        "DEVICE_TOKENS",
        "CLAUDE_CODE_OAUTH_TOKEN",
    }
    assert report["env"]["CLAUDE_CODE_MAX_OUTPUT_TOKENS"] == "1234"
    assert report["cwd_entries"] == []
    assert Path(report["cwd"]).resolve() != Path.cwd().resolve()
    # The directory goes when the client is closed.
    assert not Path(report["cwd"]).exists()


# --- What the process is started with ----------------------------------------


def test_only_listed_variables_are_passed_on():
    environment = child_environment(
        {"Path": "somewhere", "HOME": "/home/x", "SOME_API_KEY": "secret"},
        max_output_tokens=8000,
    )
    assert environment == {
        "Path": "somewhere",
        "HOME": "/home/x",
        "CLAUDE_CODE_MAX_OUTPUT_TOKENS": "8000",
    }


def command_for(policy: FetchPolicy) -> list[str]:
    return build_command(
        "claude", model="claude-opus-5", effort="low", system_prompt="P.", policy=policy
    )


def rules_of(command: list[str]) -> list[str]:
    start = command.index("--allowedTools") + 1
    return command[start : command.index("--permission-mode")]


def test_the_command_allows_search_alone_and_none_of_the_persons_set_up():
    command = command_for(SEARCH_ONLY)

    def value(option: str) -> str:
        return command[command.index(option) + 1]

    assert value("--tools") == "WebSearch"
    assert rules_of(command) == ["WebSearch"]
    assert not any("WebFetch" in argument for argument in command)
    assert value("--permission-mode") == "dontAsk"
    assert value("--permission-prompts") == "none"
    assert value("--model") == "claude-opus-5"
    assert value("--effort") == "low"
    assert value("--system-prompt") == "P."
    # The person's own settings files, and any rules in them, are left out.
    assert value("--setting-sources") == "project,local"
    for option in ("--safe-mode", "--strict-mcp-config", "--no-session-persistence"):
        assert option in command
    # Options that would loosen it must never appear.
    assert "--dangerously-skip-permissions" not in command
    assert "bypassPermissions" not in command


def test_pages_may_be_read_from_named_sites_and_no_others():
    command = command_for(FetchPolicy(hosts=("wttr.in", "en.wikipedia.org")))
    assert command[command.index("--tools") + 1] == "WebFetch,WebSearch"
    assert rules_of(command) == [
        "WebSearch",
        "WebFetch(domain:wttr.in)",
        "WebFetch(domain:en.wikipedia.org)",
    ]


def test_pages_may_be_read_from_any_site():
    command = command_for(ANY_SITE)
    assert rules_of(command) == ["WebSearch", "WebFetch"]
    assert not any("domain:" in argument for argument in command)


def test_the_policy_is_read_from_the_three_forms_of_the_setting():
    assert FetchPolicy.parse(["*"]) == ANY_SITE
    assert FetchPolicy.parse([]) == SEARCH_ONLY
    assert FetchPolicy.parse(["wttr.in"]) == FetchPolicy(hosts=("wttr.in",))
    assert ANY_SITE.describe() == "any site"
    assert SEARCH_ONLY.describe() == "no site"
    assert FetchPolicy(hosts=("wttr.in", "apnews.com")).describe() == "wttr.in, apnews.com"


@pytest.mark.parametrize(
    "names",
    [["*", "wttr.in"], ["*.example.com"], ["example.com/path"], ["https://example.com"],
     ["Example.com"], ["localhost"], ["example.com)"], ["203.0.113.5"], [""]],
)  # fmt: skip
def test_a_site_that_is_not_a_plain_host_name_is_refused(names):
    with pytest.raises(ValueError):
        FetchPolicy.parse(names)


def test_a_policy_cannot_be_built_past_the_checks():
    # The command line trusts the policy, so the policy checks itself.
    with pytest.raises(ValueError):
        FetchPolicy(hosts=("*",))
    with pytest.raises(ValueError):
        FetchPolicy(any_site=True, hosts=("wttr.in",))


# --- The prompt and the settings ---------------------------------------------


def test_the_prompt_differs_from_the_apis_only_in_what_can_be_reached():
    with_search = system_prompt_with_search()
    assert "search the web" in with_search
    assert "access to the internet" not in with_search
    assert "You can also read" not in with_search
    assert "access to the internet" in SYSTEM_PROMPT
    start = SYSTEM_PROMPT.index("You do not have the current date")
    assert with_search.startswith(SYSTEM_PROMPT[:start])
    assert with_search.endswith(SYSTEM_PROMPT[-40:])


def test_the_prompt_names_the_sites_that_can_be_read():
    prompt = system_prompt_with_search(read_sites=("wttr.in", "en.wikipedia.org"))
    assert "only from these sites: wttr.in, en.wikipedia.org." in prompt
    assert "https://wttr.in/" in prompt
    # The weather site is mentioned only when it may be read.
    assert "https://wttr.in/" not in system_prompt_with_search(
        read_sites=("en.wikipedia.org",)
    )


def test_the_prompt_for_any_site_names_none():
    prompt = system_prompt_with_search(read_any_site=True)
    assert "read a web page" in prompt
    assert "only from these sites" not in prompt
    assert "https://wttr.in/" in prompt


def settings(**values: object) -> Settings:
    return Settings(_env_file=None, deepgram_api_key="k" * 30, **values)


def test_the_api_backend_needs_its_key(monkeypatch):
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    with pytest.raises(ValidationError, match="ANTHROPIC_API_KEY"):
        settings(llm_backend="api")
    # A key left blank, as in a copy of the example file, is no key.
    with pytest.raises(ValidationError, match="ANTHROPIC_API_KEY"):
        settings(llm_backend="api", anthropic_api_key="  ")
    assert settings(llm_backend="api", anthropic_api_key="k" * 30)


def test_claude_code_is_the_default_backend(monkeypatch):
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    monkeypatch.delenv("LLM_BACKEND", raising=False)
    assert settings().llm_backend == "claude_code"


def test_the_claude_code_backend_needs_no_key(monkeypatch):
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    assert settings(llm_backend="claude_code").anthropic_api_key is None
    blank = settings(llm_backend="claude_code", anthropic_api_key="")
    assert blank.anthropic_api_key is None


def test_an_unknown_backend_is_refused():
    with pytest.raises(ValidationError):
        settings(anthropic_api_key="k" * 30, llm_backend="something_else")


def test_any_site_is_the_default_setting_and_is_a_policy():
    default = settings(anthropic_api_key="k" * 30).claude_fetch_domains
    assert FetchPolicy.parse(default) == ANY_SITE
