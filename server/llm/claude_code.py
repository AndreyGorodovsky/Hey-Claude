"""Claude reached through the Claude Code command-line tool.

Where ``claude.py`` calls the API with a key, this runs ``claude -p``, which
answers under the login of whoever is signed in to Claude Code on the
server's machine, and can search the web. One process is started for each
turn and ends with it.

The conversation stays in the server's own store. The process is given the
day's earlier exchanges as text with each request, and keeps no session of
its own, so there is one copy of the history and it is deleted with the day.

The process is locked down, because it is a general agent being used as a
voice: it gets web search and the reading of web pages, and nothing that
runs a command or touches a file; no settings, hooks, plugins, skills or MCP
servers; an empty directory to run in; and an environment holding none of
the server's credentials. Reading can be kept to a list of sites, or left
out, by configuration. The tools, the MCP servers and the permission mode
the process actually started with are checked on every run, from the first
line it prints, and a run with anything else is stopped before it can
answer.

The file is in four parts: what the process is started with (the reading
policy, the command line, the environment, the request); what is made of
what it prints (``_ReplyReader``, which never touches a process and is
tested without one); the client itself, which starts the process and reads
its lines against a clock; and the stopping of it.

The tool updates itself, and this was written against version 2.1.296. Its
options and the shape of its events are not a published interface in the
way the API is: after an update, ``server/tools/claude_cli_bench.py`` shows
in a minute whether both still hold.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from collections.abc import AsyncIterator, Awaitable, Mapping, Sequence
from contextlib import suppress
from dataclasses import dataclass
from typing import Any, TypeVar

from server.conversation import Exchange
from server.llm.base import LanguageModel, LanguageModelError, ReplyRefused
from server.llm.claude_code_prompt import system_prompt_with_search
from server.protocol import LLM_SILENCE_SECONDS

log = logging.getLogger(__name__)

_T = TypeVar("_T")

#: The tool that searches the web. It returns titles and short extracts.
SEARCH_TOOL = "WebSearch"

#: The tool that reads one page. A page can carry instructions written to
#: mislead a model, and an address it is told to open can carry the day's
#: conversation out. That is accepted for this assistant, which holds
#: nothing else and cannot act; a list of sites closes it for whoever
#: configures one (KNOWN-ISSUES R21).
FETCH_TOOL = "WebFetch"

#: Environment variables passed on to the process; every other one is
#: withheld. A list of what to pass, not of what to withhold: the server's
#: environment can hold its API keys and device tokens, and a key added
#: later would not be on a list of names to remove. These are what a program
#: needs to find the user's profile, where the login is kept, and to reach
#: the network. A login given to Claude Code in a variable of its own is
#: withheld with the rest: the tool must be signed in.
_PASSED_ENVIRONMENT = frozenset(
    {
        # Windows.
        "APPDATA", "COMSPEC", "HOMEDRIVE", "HOMEPATH", "LOCALAPPDATA",
        "NUMBER_OF_PROCESSORS", "OS", "PATHEXT", "PROCESSOR_ARCHITECTURE",
        "PROGRAMDATA", "PROGRAMFILES", "PROGRAMFILES(X86)", "SYSTEMDRIVE",
        "SYSTEMROOT", "TEMP", "TMP", "USERDOMAIN", "USERNAME", "USERPROFILE",
        "WINDIR",
        # Elsewhere.
        "HOME", "LANG", "LC_ALL", "LC_CTYPE", "LOGNAME", "SHELL", "TMPDIR",
        "USER", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
        # Both.
        "PATH",
        # A network that needs a proxy or its own certificates.
        "ALL_PROXY", "HTTP_PROXY", "HTTPS_PROXY", "NO_PROXY",
        "NODE_EXTRA_CA_CERTS", "SSL_CERT_DIR", "SSL_CERT_FILE",
        # Where Claude Code keeps its login, if not in the default place.
        "CLAUDE_CONFIG_DIR",
    }
)  # fmt: skip

#: Longest the reply may go without a word that can be spoken: from the
#: start of the process to its first text, and from any text to the next.
#: It is the protocol's limit on silence from Claude, and it has to be
#: counted in text. The process prints a line for every step of a search
#: too, so a count of lines would let a reply that searches again and again
#: stay silent for as long as it liked, past what a device waits through.
_TEXT_SILENCE_SECONDS = LLM_SILENCE_SECONDS

#: Longest line accepted from the process. A line is one event, and an event
#: carrying search results is far longer than asyncio's default of 64 KB.
_LINE_LIMIT = 16 * 1024 * 1024

#: Seconds allowed for the process to end by itself after its last line.
_EXIT_SECONDS = 3.0

#: Characters of the tool's own error text kept for the log.
_ERROR_TEXT_LIMIT = 200

#: Bytes of the process's diagnostics kept, the last of them, to explain a
#: process that ended without a result.
_STDERR_KEPT = 2000


# --- What the process is started with ----------------------------------------


@dataclass(frozen=True)
class FetchPolicy:
    """Which sites' pages the assistant may read: none, any, or those named.

    The one place that knows how the policy is written in a setting, and
    what it means for the tools, the permission rules and the wording.
    """

    any_site: bool = False
    hosts: tuple[str, ...] = ()

    #: In the setting, alone: any site.
    _ANY = "*"

    #: A host name: letters, digits, hyphens and dots, at least two parts.
    #: Nothing else, since each name is placed inside a permission rule.
    _HOST = re.compile(
        r"[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+"
    )

    def __post_init__(self) -> None:
        if self.any_site and self.hosts:
            raise ValueError("any site cannot be combined with named sites")
        for host in self.hosts:
            # A last part made of digits is a numeric address, which could
            # name a machine on the local network.
            if not self._HOST.fullmatch(host) or host.rsplit(".", 1)[-1].isdigit():
                raise ValueError(
                    "a site must be a plain host name, such as"
                    ' "en.wikipedia.org", in lower case and with no wildcard,'
                    " address or path"
                )

    @classmethod
    def parse(cls, names: Sequence[str]) -> FetchPolicy:
        """Read the policy from a setting: ``["*"]``, host names, or nothing.

        Raises ``ValueError``, which never quotes the names.
        """
        if list(names) == [cls._ANY]:
            return cls(any_site=True)
        return cls(hosts=tuple(names))

    @property
    def reads(self) -> bool:
        """Whether any page may be read at all."""
        return self.any_site or bool(self.hosts)

    def tools(self) -> frozenset[str]:
        """The tools the process may have."""
        return frozenset({SEARCH_TOOL, FETCH_TOOL} if self.reads else {SEARCH_TOOL})

    def rules(self) -> list[str]:
        """The permission rules: what the tools may do without asking.

        Searching freely, and reading either from anywhere, which is the
        tool's bare name, or from each named site and no other.
        """
        if self.any_site:
            return [SEARCH_TOOL, FETCH_TOOL]
        return [SEARCH_TOOL, *(f"{FETCH_TOOL}(domain:{host})" for host in self.hosts)]

    def describe(self) -> str:
        """The policy in words, for the log."""
        if self.any_site:
            return "any site"
        return ", ".join(self.hosts) or "no site"


def build_command(
    executable: str,
    *,
    model: str,
    effort: str,
    system_prompt: str,
    policy: FetchPolicy = FetchPolicy(),
) -> list[str]:
    """Return the command line for one locked-down, streamed reply.

    The request itself is not on the command line: it goes to the process's
    standard input, which keeps transcripts out of the machine's process
    list and is not limited in length.
    """
    return [
        executable,
        "--print",
        # One JSON event per line as the reply is generated, with the text
        # in pieces. The tool requires --verbose for this format.
        "--output-format", "stream-json",
        "--verbose",
        "--include-partial-messages",
        "--model", model,
        # Thinking stays on, as the project requires; effort is the control.
        "--effort", effort,
        # Replaces the tool's own prompt, which is about writing software.
        "--system-prompt", system_prompt,
        # The tools that exist at all, then what they may do without
        # asking. Nobody is there to answer a prompt, and in this mode
        # whatever no rule allows is refused: with named sites, a page
        # from any other.
        "--tools", ",".join(sorted(policy.tools())),
        "--allowedTools", *policy.rules(),
        "--permission-mode", "dontAsk",
        "--permission-prompts", "none",
        # Nothing of the signed-in person's own set-up. Their settings
        # files are not loaded: only those of the directory the process
        # runs in are named, and it is empty. Otherwise a permission rule
        # of theirs could allow reading from more sites than the policy.
        # Nor instruction files, hooks, plugins or skills, and no MCP
        # servers, which would otherwise include the account's connectors
        # to mail and files.
        "--setting-sources", "project,local",
        "--safe-mode",
        "--strict-mcp-config",
        "--disable-slash-commands",
        "--no-chrome",
        # No transcript of the conversation is written to the profile.
        "--no-session-persistence",
    ]  # fmt: skip


def child_environment(
    parent: Mapping[str, str], *, max_output_tokens: int
) -> dict[str, str]:
    """Return the environment the process runs in, built from ``parent``.

    Only the variables in ``_PASSED_ENVIRONMENT`` are passed on, so no API
    key or device token reaches the process, and with no API key it can only
    answer under the Claude Code login.
    """
    environment = {
        name: value
        for name, value in parent.items()
        if name.upper() in _PASSED_ENVIRONMENT
    }
    # The tool's own setting for the ceiling on one reply.
    environment["CLAUDE_CODE_MAX_OUTPUT_TOKENS"] = str(max_output_tokens)
    return environment


def build_prompt(history: Sequence[Exchange], user_text: str) -> str:
    """Return the request as the process reads it, earlier exchanges first."""
    if not history:
        return user_text
    lines = ["The conversation so far today, for context:", ""]
    for exchange in history:
        lines.append(f"Person: {exchange.user_text}")
        lines.append(f"You: {exchange.assistant_text}")
        lines.append("")
    lines.append("The person now says:")
    lines.append(user_text)
    return "\n".join(lines)


# --- What is made of what the process prints ---------------------------------


def lockdown_problem(init: Mapping[str, Any], allowed: frozenset[str]) -> str | None:
    """Say what is wrong with what the process started with, if anything.

    ``init`` is the first event the process prints, which lists the tools
    and MCP servers it has loaded and names its permission mode; ``allowed``
    is the tools it may have. The description names tools, servers and the
    mode and nothing else, so that it can be logged.

    Those three are all that is checked. The sites that may be read are not
    in that event; what is checked instead is the mode that makes the
    policy binding, since in any other a page from another site might be
    allowed or asked about instead of refused. Skills, plugins and the rest
    are kept out by the command line and are not looked for here.
    """
    tools = init.get("tools")
    servers = init.get("mcp_servers")
    if not isinstance(tools, list) or not isinstance(servers, list):
        return "the tool did not say what it started with"
    extra = sorted(str(tool) for tool in tools if tool not in allowed)
    if extra:
        return f"tools beyond the allowed ones: {', '.join(extra)}"
    if init.get("permissionMode") != "dontAsk":
        return f"permission mode is {init.get('permissionMode')!r}, not 'dontAsk'"
    if servers:
        names = sorted(
            str(server.get("name", "?")) if isinstance(server, dict) else "?"
            for server in servers
        )
        return f"MCP servers loaded: {', '.join(names)}"
    return None


class _SourcesFilter:
    """Passes reply text on, without the list of sources at its end.

    The search tool instructs the model to close its answer with a line
    "Sources:" and a list of links, which would be read aloud, addresses and
    all. The system prompt asks for none; this removes one that comes
    anyway: everything from a line that begins with "Sources:" until the
    model next uses a tool, or the reply ends. Text arrives in pieces, so
    the start of a line is held back for as long as it could still turn out
    to be that word, which is a few characters and never a whole sentence.

    It cannot tell a list from a sentence that happens to open a line with
    the word, and removes that too. ``dropped`` counts what it removed, for
    the log, so that use shows whether it is needed and what it costs.
    """

    _MARK = "Sources:"

    def __init__(self) -> None:
        self._held = ""
        self._at_line_start = True
        self._dropping = False
        #: Characters removed so far.
        self.dropped = 0

    def feed(self, text: str) -> str:
        """Add ``text`` and return what may be spoken of it so far."""
        if self._dropping:
            self.dropped += len(text)
            return ""
        data, self._held = self._held + text, ""
        passed: list[str] = []
        while (end := data.find("\n")) >= 0:
            line, data = data[: end + 1], data[end + 1 :]
            if self._at_line_start and line.lstrip().startswith(self._MARK):
                self._dropping = True
                self.dropped += len(line) + len(data)
                return "".join(passed)
            passed.append(line)
            self._at_line_start = True
        # What is left is a line still being written.
        if self._at_line_start:
            start = data.lstrip()
            if start.startswith(self._MARK):
                self._dropping = True
                self.dropped += len(data)
            elif self._MARK.startswith(start):
                self._held = data
            else:
                passed.append(data)
                self._at_line_start = False
        else:
            passed.append(data)
        return "".join(passed)

    def tool_used(self) -> None:
        """The model has gone on to use a tool: what follows is new text.

        A list of sources closes an answer. One that came before a further
        search must not take the real answer, after the search, with it.
        """
        self._dropping = False
        self._at_line_start = True

    def flush(self) -> str:
        """Return what was held back, once the reply has ended."""
        held, self._held = self._held, ""
        return "" if self._dropping else held


class _ReplyReader:
    """Turns the events of one reply into the text to speak, and its outcome.

    Fed one parsed event at a time; touches no process and no clock. The
    events are those of ``--output-format stream-json`` with partial
    messages: a first ``system``/``init`` event, ``stream_event`` wrappers
    around the API's own streaming events, and a last ``result``.
    """

    def __init__(self, allowed: frozenset[str]) -> None:
        self._allowed = allowed
        self._checked = False
        self._sources = _SourcesFilter()
        # Whether a block of text is being written, and has had any text.
        self._in_text = False
        self._block_has_text = False
        self._refused = False
        self._cut_off = False
        #: The tool's version, as its first event gives it.
        self.version: str | None = None
        #: Searches and pages read.
        self.tool_calls = 0
        #: The last event, once it has come.
        self.result: dict[str, Any] | None = None

    @property
    def dropped(self) -> int:
        """Characters of a sources list removed from the reply."""
        return self._sources.dropped

    def feed(self, event: Mapping[str, Any]) -> str:
        """Take one event; return the text it adds to what may be spoken.

        Raises ``LanguageModelError`` if the process is not locked down, or
        an event does not have the shape this was written against.
        """
        try:
            return self._feed(event)
        except (AttributeError, TypeError):
            # A field that should hold an object or a string holds
            # something else: the tool has changed under this client.
            raise LanguageModelError(
                "Claude Code printed an event of an unexpected shape"
            ) from None

    def _feed(self, event: Mapping[str, Any]) -> str:
        kind = event.get("type")
        if kind == "system" and event.get("subtype") == "init":
            problem = lockdown_problem(event, self._allowed)
            if problem is not None:
                raise LanguageModelError(f"Claude Code is not locked down: {problem}")
            self._checked = True
            version = event.get("claude_code_version")
            self.version = version if isinstance(version, str) else None
        elif kind == "result":
            self.result = dict(event)
        elif kind == "stream_event":
            if not self._checked:
                raise LanguageModelError(
                    "Claude Code began a reply before saying what it started with"
                )
            # Text written by a helper the tool starts for itself, such as
            # the one that condenses a page, is not the reply.
            if event.get("parent_tool_use_id") is None:
                return self._feed_stream(event.get("event") or {})
        return ""

    def _feed_stream(self, inner: Mapping[str, Any]) -> str:
        kind = inner.get("type")
        if kind == "content_block_start":
            block = (inner.get("content_block") or {}).get("type")
            self._in_text = block == "text"
            self._block_has_text = False
            if block == "tool_use":
                self.tool_calls += 1
                self._sources.tool_used()
        elif kind == "content_block_delta":
            delta = inner.get("delta") or {}
            text = delta.get("text")
            # Only text: a block of thinking, or of a tool's input, has
            # pieces of other types, which are never spoken.
            if delta.get("type") == "text_delta" and isinstance(text, str) and text:
                self._block_has_text = True
                return self._sources.feed(text)
        elif kind == "content_block_stop":
            if self._in_text and self._block_has_text:
                self._in_text = False
                # The end of a block of text, which the tool reports at
                # once. A line break here separates this block's words from
                # the next block's, which follow a tool call with nothing
                # between them. It is also what the interface asks for
                # (base.py): the reply now pauses, for a search or for the
                # process to finish, and the last sentence must not wait
                # through the pause to be spoken.
                return self._sources.feed("\n")
            self._in_text = False
        elif kind == "message_delta":
            stop = (inner.get("delta") or {}).get("stop_reason")
            self._refused = self._refused or stop == "refusal"
            self._cut_off = self._cut_off or stop == "max_tokens"
        return ""

    def finish(self) -> str:
        """Return any text still held back, once the events have ended."""
        return self._sources.flush()

    def raise_for_outcome(self) -> None:
        """Raise what the reply's last event calls for, if anything.

        ``ReplyRefused`` for a declined request, ``LanguageModelError`` for
        a failure the tool reports. Called once ``result`` has come.
        """
        result = self.result or {}
        if self._cut_off:
            log.warning("claude code: reply cut off at the token ceiling")
        if self._refused or result.get("stop_reason") == "refusal":
            raise ReplyRefused("Claude declined the request")
        if result.get("is_error") or result.get("subtype") != "success":
            # The tool's own words for what went wrong: not signed in, a
            # usage limit reached, a network failure. Cut short, since
            # nothing here guarantees what the field holds.
            detail = str(result.get("result") or result.get("subtype") or "")
            raise LanguageModelError(
                f"Claude Code failed: {detail[:_ERROR_TEXT_LIMIT]}"
            )


def _parse(line: bytes) -> dict[str, Any] | None:
    """Return the event on one line of output, or None if it is not one."""
    try:
        event = json.loads(line)
    except (ValueError, UnicodeDecodeError):
        return None
    return event if isinstance(event, dict) else None


# --- The client --------------------------------------------------------------


class ClaudeCodeLanguageModel(LanguageModel):
    """Replies from ``claude -p``, streamed, one process for each reply."""

    def __init__(
        self,
        *,
        model: str,
        effort: str,
        max_tokens: int,
        executable: str = "claude",
        policy: FetchPolicy = FetchPolicy(),
        command: Sequence[str] | None = None,
    ) -> None:
        """Find the tool and prepare the directory its processes run in.

        ``policy`` says which sites' pages the assistant may read.
        ``command`` replaces the whole command line, for tests, which run a
        stand-in for the tool; ``model``, ``effort`` and ``executable`` are
        then unused.
        """
        self._allowed = policy.tools()
        if command is None:
            found = shutil.which(executable)
            if found is None:
                raise LanguageModelError(
                    f"the Claude Code tool {executable!r} was not found"
                )
            if found.lower().endswith((".cmd", ".bat")):
                # A script wrapper runs through the Windows command
                # interpreter, which would reinterpret the quotes and line
                # breaks of the system prompt.
                raise LanguageModelError(
                    "the Claude Code tool must be a program, not a .cmd wrapper;"
                    " set CLAUDE_CLI_PATH to its native installation"
                )
            command = build_command(
                found,
                model=model,
                effort=effort,
                system_prompt=system_prompt_with_search(
                    read_any_site=policy.any_site, read_sites=policy.hosts
                ),
                policy=policy,
            )
        self._command = list(command)
        self._max_tokens = max_tokens
        self._version_logged = False
        # An empty directory of its own for the processes to run in, so
        # that there is no project around them to read. Removed in aclose;
        # a process still running there would keep Windows from removing
        # it, which is not worth failing a shutdown over.
        self._directory = tempfile.TemporaryDirectory(
            prefix="hey-claude-", ignore_cleanup_errors=True
        )

    async def stream_reply(
        self, history: Sequence[Exchange], user_text: str
    ) -> AsyncIterator[str]:
        started = time.monotonic()
        try:
            process = await asyncio.create_subprocess_exec(
                *self._command,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                cwd=self._directory.name,
                env=child_environment(
                    os.environ, max_output_tokens=self._max_tokens
                ),
                limit=_LINE_LIMIT,
                # On POSIX, a session of its own, so that the process and
                # anything it starts can be stopped together (_stop).
                start_new_session=sys.platform != "win32",
            )
        except OSError as exc:
            raise LanguageModelError(
                f"could not start Claude Code ({type(exc).__name__})"
            ) from None

        reader = _ReplyReader(self._allowed)
        diagnostics = asyncio.create_task(_tail(process.stderr))
        first_text: float | None = None
        # When the reply will have been silent for too long, on the event
        # loop's clock. Moved on by every piece of text, and by nothing
        # else the process prints.
        clock = asyncio.get_running_loop().time
        deadline = clock() + _TEXT_SILENCE_SECONDS
        try:
            assert process.stdin is not None and process.stdout is not None
            await _before(deadline, _send(process.stdin, history, user_text))
            while reader.result is None:
                line = await _before(deadline, process.stdout.readline())
                if not line:
                    break
                event = _parse(line)
                if event is None:
                    continue
                log.debug(
                    "claude code: event %s %s",
                    event.get("type"),
                    event.get("subtype", ""),
                )
                if spoken := reader.feed(event):
                    if first_text is None:
                        first_text = time.monotonic()
                    deadline = clock() + _TEXT_SILENCE_SECONDS
                    yield spoken
            if spoken := reader.finish():
                yield spoken
        finally:
            # Reached on success, on failure, and when the turn is cancelled
            # or its consumer goes away: the process never outlives its turn.
            await _end(process, expected=reader.result is not None)

        if reader.version is not None and not self._version_logged:
            self._version_logged = True
            log.info("claude code: the tool is version %s", reader.version)
        if reader.result is None:
            # The process has ended, so its diagnostics end too; a moment
            # is allowed for the last of them to be read.
            await asyncio.wait({diagnostics}, timeout=1)
            raise LanguageModelError(
                f"Claude Code ended without a result (exit {process.returncode}):"
                f" {_said(diagnostics)[:_ERROR_TEXT_LIMIT]}"
            )
        usage = reader.result.get("usage")
        usage = usage if isinstance(usage, dict) else {}
        log.info(
            "claude code: outcome=%s turns=%s tool_calls=%d first_text=%s total=%.2f s"
            " api=%s ms input=%s cache_read=%s cache_write=%s output=%s"
            " sources_dropped=%d",
            reader.result.get("subtype"),
            reader.result.get("num_turns"),
            reader.tool_calls,
            "-" if first_text is None else f"{first_text - started:.2f} s",
            time.monotonic() - started,
            reader.result.get("duration_api_ms"),
            usage.get("input_tokens"),
            usage.get("cache_read_input_tokens"),
            usage.get("cache_creation_input_tokens"),
            usage.get("output_tokens"),
            reader.dropped,
        )
        reader.raise_for_outcome()

    async def aclose(self) -> None:
        # Blocking, and brief: the directory is empty.
        self._directory.cleanup()


async def _send(
    stdin: asyncio.StreamWriter, history: Sequence[Exchange], user_text: str
) -> None:
    """Write the request to the process and close its input."""
    stdin.write(build_prompt(history, user_text).encode("utf-8"))
    # A process that has ended already has closed its end; what it printed,
    # or printed nothing, says why.
    with suppress(ConnectionError):
        await stdin.drain()
    stdin.close()


async def _before(deadline: float, wait: Awaitable[_T]) -> _T:
    """Wait for ``wait``, but not past ``deadline`` on the event loop's clock.

    Kept apart from the generator that calls it: a time limit held open
    across a ``yield`` would also count, and could interrupt, whatever the
    reply's consumer does between two pieces.
    """
    try:
        async with asyncio.timeout_at(deadline):
            return await wait
    except TimeoutError:
        raise LanguageModelError(
            f"Claude Code said nothing for {_TEXT_SILENCE_SECONDS} s"
        ) from None
    except ValueError:
        # asyncio's report of a line longer than the limit.
        raise LanguageModelError(
            "Claude Code printed a line too long to read"
        ) from None


async def _tail(stream: asyncio.StreamReader | None) -> bytes:
    """Read a stream to its end, keeping the last of it.

    Run as a task beside the reply, so that the process's diagnostics never
    fill their pipe and stall it.
    """
    kept = b""
    if stream is not None:
        with suppress(asyncio.CancelledError, OSError):
            while chunk := await stream.read(4096):
                kept = (kept + chunk)[-_STDERR_KEPT:]
    return kept


def _said(diagnostics: asyncio.Task[bytes]) -> str:
    """What the process's diagnostics ended with, on one line, if known."""
    if not diagnostics.done() or diagnostics.cancelled():
        return "no diagnostics"
    text = diagnostics.result().decode("utf-8", errors="replace")
    return " ".join(text.split()) or "no diagnostics"


# --- Stopping the process ----------------------------------------------------


async def _stop(process: asyncio.subprocess.Process) -> None:
    """Stop the process and whatever it started, and wait for it to end."""
    if process.returncode is None:
        if sys.platform == "win32":
            # Windows has no call that stops a tree of processes; its own
            # ``taskkill`` does, following each process's children. It
            # takes a moment, so it runs on another thread and the server
            # goes on serving meanwhile.
            def taskkill() -> None:
                subprocess.run(
                    ["taskkill", "/T", "/F", "/PID", str(process.pid)],
                    capture_output=True,
                    timeout=5,
                    check=False,
                )

            with suppress(OSError, subprocess.SubprocessError):
                await asyncio.to_thread(taskkill)
        else:
            # The process leads a session of its own (stream_reply).
            with suppress(ProcessLookupError, PermissionError):
                os.killpg(process.pid, signal.SIGKILL)
        # Whatever the above did, the process itself is not left running.
        with suppress(ProcessLookupError):
            process.kill()
    await process.wait()


async def _end(process: asyncio.subprocess.Process, *, expected: bool) -> None:
    """Wait for the process to end, stopping it if it should not go on.

    With ``expected`` the process has printed its result and is given a
    moment to end by itself. Otherwise it is stopped at once.
    """
    try:
        if expected and process.returncode is None:
            with suppress(TimeoutError):
                async with asyncio.timeout(_EXIT_SECONDS):
                    await process.wait()
    finally:
        # In a finally, so that a turn cancelled during the wait above
        # still stops its process. Shielded, so that the stopping runs to
        # its end even if this is cancelled in turn; the cancellation is
        # then passed on, not swallowed.
        await asyncio.shield(_stop(process))
