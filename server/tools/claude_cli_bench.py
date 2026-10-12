"""Times the Claude Code tool as the server would use it, with no server.

Puts written questions to ``claude -p`` through the server's own client
(``server/llm/claude_code.py``), so the command line, the lock-down and its
check are exactly the server's, and prints for each question how long the
first text and the whole reply took, and the reply. Nothing is spoken and no
device, speech service or API key is involved; the tool answers under the
login of whoever is signed in to Claude Code on this machine.

Run from the repository root:

    python -m server.tools.claude_cli_bench
    python -m server.tools.claude_cli_bench --set search --runs 3
    python -m server.tools.claude_cli_bench --history 10 --debug

The questions are fixed and written here, so the output holds nothing
personal. A run that the lock-down check stops says so, naming the tools or
MCP servers the process should not have had.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import statistics
import time

from server.conversation import Exchange
from server.llm import LanguageModelError
from server.llm.claude_code import ClaudeCodeLanguageModel, FetchPolicy

#: Answerable from what the model knows; no search is expected.
PLAIN = (
    "What is two plus two?",
    "What is the capital of France?",
    "How many legs does a spider have?",
    "Who wrote Romeo and Juliet?",
    "Why is the sky blue?",
)

#: Need something current, so a search or a page read is expected. The last
#: but one asks for a page by its address, which shows whether reading works
#: or, with --sites, is refused; the last shows whether the tool tells the
#: model the date.
SEARCH = (
    "What is the weather in London today?",
    "What is in the news today?",
    "What is the price of gold right now?",
    "Who won the most recent Formula One race?",
    "Read the page at example.com and tell me what it says.",
    "What is today's date?",
)

#: One made-up exchange, repeated to stand for a day's conversation.
_FILLER = Exchange(
    "Tell me something about the Eiffel Tower.",
    "The Eiffel Tower was finished in eighteen eighty-nine for the World's "
    "Fair in Paris, and it was the tallest structure in the world for about "
    "forty years.",
)


async def ask(
    llm: ClaudeCodeLanguageModel, history: list[Exchange], question: str
) -> tuple[float | None, float]:
    """Put one question; return seconds to the first text and to the end."""
    started = time.monotonic()
    first: float | None = None
    pieces: list[str] = []
    try:
        async for piece in llm.stream_reply(history, question):
            if first is None:
                first = time.monotonic() - started
            pieces.append(piece)
    except LanguageModelError as exc:
        print(f"  FAILED: {exc}")
    total = time.monotonic() - started
    shown = "-" if first is None else f"{first:.2f} s"
    print(f"  first text {shown}, whole reply {total:.2f} s")
    print(f"  {''.join(pieces).strip()!r}")
    return first, total


async def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--set", choices=("plain", "search", "both"), default="both")
    parser.add_argument("--runs", type=int, default=1, help="times through each set")
    parser.add_argument(
        "--history", type=int, default=0, help="made-up earlier exchanges to send"
    )
    parser.add_argument("--model", default="claude-opus-5")
    parser.add_argument("--effort", default="low")
    parser.add_argument("--executable", default="claude")
    parser.add_argument(
        "--no-fetch",
        action="store_true",
        help="search only: no page may be read, from any site",
    )
    parser.add_argument(
        "--sites",
        nargs="+",
        metavar="HOST",
        help="read pages from these sites only, in place of any site",
    )
    parser.add_argument(
        "--debug", action="store_true", help="log the type of every event"
    )
    args = parser.parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.debug else logging.INFO,
        format="  %(name)s: %(message)s",
    )

    if args.no_fetch:
        policy = FetchPolicy()
    elif args.sites:
        policy = FetchPolicy.parse(args.sites)
    else:
        policy = FetchPolicy(any_site=True)
    llm = ClaudeCodeLanguageModel(
        model=args.model, effort=args.effort, max_tokens=8000,
        executable=args.executable, policy=policy,
    )  # fmt: skip
    history = [_FILLER] * args.history
    sets = {"plain": PLAIN, "search": SEARCH}
    try:
        for name, questions in sets.items():
            if args.set not in (name, "both"):
                continue
            firsts: list[float] = []
            for run in range(args.runs):
                for question in questions:
                    print(f"[{name}, run {run + 1}] {question}")
                    first, _ = await ask(llm, history, question)
                    if first is not None:
                        firsts.append(first)
            if firsts:
                print(
                    f"{name}: first text over {len(firsts)} replies: "
                    f"least {min(firsts):.2f} s, median "
                    f"{statistics.median(firsts):.2f} s, most {max(firsts):.2f} s\n"
                )
    finally:
        await llm.aclose()


if __name__ == "__main__":
    asyncio.run(main())
