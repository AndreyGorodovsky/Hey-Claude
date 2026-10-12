"""A stand-in for the Claude Code tool, run as a process by the tests.

Prints the events of one scenario, one JSON object to a line, as the tool
does with ``--output-format stream-json``. Usage:

    python fake_claude_cli.py <scenario> [<report file>]

With a report file, it first writes what it was started with (its standard
input, working directory and environment), for the tests to read.

The events are built by the functions below, which the tests of the event
reader use too, without a process. Their shapes were written from the real
tool's output as seen on 2026-10-12, version 2.1.296; they are a copy, and
only ``server/tools/claude_cli_bench.py`` shows what the real tool prints.
"""

from __future__ import annotations

import json
import os
import sys
import time


def init_event(
    tools: list[str] | None = None,
    servers: list[dict] | None = None,
    mode: str = "dontAsk",
) -> dict:
    return {
        "type": "system",
        "subtype": "init",
        "tools": ["WebSearch"] if tools is None else tools,
        "mcp_servers": servers or [],
        "permissionMode": mode,
        "claude_code_version": "0.0.0-fake",
    }


def stream_event(inner: dict, parent: str | None = None) -> dict:
    return {"type": "stream_event", "event": inner, "parent_tool_use_id": parent}


def block_start(kind: str) -> dict:
    return stream_event({"type": "content_block_start", "content_block": {"type": kind}})


def block_stop() -> dict:
    return stream_event({"type": "content_block_stop"})


def text_delta(text: str, parent: str | None = None) -> dict:
    return stream_event(
        {"type": "content_block_delta", "delta": {"type": "text_delta", "text": text}},
        parent,
    )


def thinking_delta(text: str) -> dict:
    return stream_event(
        {
            "type": "content_block_delta",
            "delta": {"type": "thinking_delta", "thinking": text},
        }
    )


def text_block(*pieces: str) -> list[dict]:
    return [block_start("text"), *(text_delta(piece) for piece in pieces), block_stop()]


def tool_call() -> list[dict]:
    return [
        block_start("tool_use"),
        block_stop(),
        {"type": "user", "message": {"content": [{"type": "tool_result"}]}},
    ]


def result_event(**fields: object) -> dict:
    return {
        "type": "result",
        "subtype": "success",
        "is_error": False,
        "num_turns": 1,
        "usage": {"input_tokens": 10, "output_tokens": 5},
        **fields,
    }


def emit(*events: dict) -> None:
    for event in events:
        print(json.dumps(event), flush=True)


def main() -> None:
    scenario = sys.argv[1]
    request = sys.stdin.read()
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w", encoding="utf-8") as report:
            json.dump(
                {
                    "stdin": request,
                    "cwd": os.getcwd(),
                    "cwd_entries": os.listdir(),
                    "env": dict(os.environ),
                },
                report,
            )

    if scenario == "plain":
        emit(init_event(), *text_block("Paris. ", "It is on ", "the Seine."))
        emit(result_event())
    elif scenario == "search":
        emit(init_event(), *text_block("Let me check."), *tool_call())
        emit(*text_block("It is sunny."), result_event(num_turns=2))
    elif scenario == "extra_tool":
        emit(init_event(tools=["WebSearch", "Bash"]))
        emit(*text_block("This must not be spoken."), result_event())
    elif scenario == "error_result":
        emit(init_event())
        emit(
            result_event(
                subtype="error_during_execution", is_error=True, result="Not logged in"
            )
        )
    elif scenario == "dies":
        emit(init_event(), *text_block("Paris."))
        sys.exit(3)
    elif scenario == "bad_option":
        # As a tool that no longer knows one of its options: nothing on
        # standard output, a reason on standard error.
        print("error: unknown option '--safe-mode'", file=sys.stderr, flush=True)
        sys.exit(1)
    elif scenario == "hangs":
        emit(init_event(), *text_block("Paris. "))
        time.sleep(600)
    elif scenario == "search_hangs":
        emit(init_event(), *text_block("Let me check."), block_start("tool_use"))
        time.sleep(600)
    elif scenario == "busy_but_wordless":
        # A reply that keeps printing, as while searching again and again,
        # and never says another word.
        emit(init_event(), *text_block("Let me check."))
        while True:
            emit(*tool_call(), block_start("thinking"), thinking_delta("Hm."))
            time.sleep(0.1)
    elif scenario == "noise":
        print("not json at all", flush=True)
        print("[1, 2, 3]", flush=True)
        emit(init_event(), *text_block("Paris."), result_event())
    else:
        sys.exit(f"unknown scenario {scenario!r}")


if __name__ == "__main__":
    main()
