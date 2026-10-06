"""A stand-in for the device, for testing the server from a desktop.

Plays one or more recorded requests to the server the way the device will:
``utterance_start``, then 20 ms frames in real time, with silence after the
recording ends, until the server says to stop. It collects each reply, can
save it as a WAV file, and prints how long each stage took.

Run from the repository root, with the server already running:

    python -m server.tools.desktop_client question.wav --out-dir <directory>

Each input must be a 16 kHz, mono, 16-bit WAV file; several files make
several turns of one conversation. Recordings and replies are personal data:
keep them outside the repository.

The device token is read from DEVICE_TOKENS in the server's configuration,
or from the HEY_CLAUDE_DEVICE_TOKEN environment variable. It is never taken
from the command line, where it would be kept in the shell's history.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
import wave
from dataclasses import dataclass, field
from pathlib import Path

from websockets.asyncio.client import ClientConnection, connect
from websockets.exceptions import WebSocketException

from server.protocol import (
    CAPTURE_FRAME_MS,
    CAPTURE_SAMPLE_RATE,
    MDNS_SERVICE_TYPE,
    SAMPLE_WIDTH_BYTES,
    STREAM_PATH,
    frame_bytes,
)

_FRAME_BYTES = frame_bytes(CAPTURE_SAMPLE_RATE, CAPTURE_FRAME_MS)
_FRAME_SECONDS = CAPTURE_FRAME_MS / 1000

#: Seconds to wait for any one message from the server before giving up.
_RECEIVE_TIMEOUT = 60.0


def read_request(path: Path) -> bytes:
    """Return the samples of a request recording, checking its format."""
    with wave.open(str(path), "rb") as wav:
        actual = (wav.getframerate(), wav.getnchannels(), wav.getsampwidth())
        if actual != (CAPTURE_SAMPLE_RATE, 1, SAMPLE_WIDTH_BYTES):
            raise SystemExit(
                f"{path.name}: needs 16000 Hz, mono, 16-bit; this file is "
                f"{actual[0]} Hz, {actual[1]} channel(s), {actual[2] * 8}-bit"
            )
        return wav.readframes(wav.getnframes())


def write_reply(path: Path, pcm: bytes, sample_rate: int) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(SAMPLE_WIDTH_BYTES)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)


@dataclass
class Reply:
    """What came back for one request, and when."""

    speech_seconds: float
    stop_capture_at: float | None = None
    first_audio_at: float | None = None
    done_at: float | None = None
    sample_rate: int = 0
    audio: bytearray = field(default_factory=bytearray)
    error: str | None = None


async def _send_request(
    ws: ClientConnection, pcm: bytes, stop: asyncio.Event
) -> None:
    """Send the recording, then silence, in real time, until told to stop."""
    silence = bytes(_FRAME_BYTES)
    started = time.monotonic()
    sent = 0
    while not stop.is_set():
        frame = pcm[sent * _FRAME_BYTES : (sent + 1) * _FRAME_BYTES] or silence
        # The last frame of the recording may be short; pad it.
        await ws.send(frame.ljust(_FRAME_BYTES, b"\x00"))
        sent += 1
        # Paced against the start, so that time spent sending does not
        # accumulate into drift.
        await asyncio.sleep(max(0.0, started + sent * _FRAME_SECONDS - time.monotonic()))


async def request(ws: ClientConnection, pcm: bytes) -> Reply:
    """Make one request and collect its reply."""
    result = Reply(
        speech_seconds=len(pcm) / SAMPLE_WIDTH_BYTES / CAPTURE_SAMPLE_RATE
    )
    stop = asyncio.Event()
    await ws.send(json.dumps({"type": "utterance_start"}))
    started = time.monotonic()
    sender = asyncio.create_task(_send_request(ws, pcm, stop))
    try:
        while True:
            async with asyncio.timeout(_RECEIVE_TIMEOUT):
                message = await ws.recv()
            at = time.monotonic() - started
            if isinstance(message, bytes):
                if result.first_audio_at is None:
                    result.first_audio_at = at
                result.audio += message
                continue
            event = json.loads(message)
            kind = event["type"]
            if kind == "stop_capture":
                result.stop_capture_at = at
                stop.set()
            elif kind == "reply_start":
                result.sample_rate = event["sample_rate"]
            elif kind == "reply_end":
                result.done_at = at
                return result
            elif kind == "error":
                result.error = f"{event['code']}: {event['message']}"
                result.done_at = at
                return result
    finally:
        stop.set()
        await sender


def report(name: str, result: Reply) -> None:
    """Print the timings of one turn.

    Times are from ``utterance_start``. The recording is sent in real time,
    so its own length is when the speaker stopped.
    """
    print(f"\n{name}")
    print(f"  request length          {result.speech_seconds:6.2f} s")
    if result.error:
        print(f"  error                   {result.error}")
        return
    if result.stop_capture_at is not None:
        wait = result.stop_capture_at - result.speech_seconds
        print(f"  end of speech detected  {wait:6.2f} s after the speaker stopped")
    if result.first_audio_at is None:
        print("  no reply audio (nothing was heard)")
        return
    print(
        f"  first reply audio       "
        f"{result.first_audio_at - result.speech_seconds:6.2f} s after the speaker stopped"
    )
    seconds = len(result.audio) / SAMPLE_WIDTH_BYTES / result.sample_rate
    print(f"  reply length            {seconds:6.2f} s at {result.sample_rate} Hz")
    print(
        f"  reply fully received    "
        f"{result.done_at - result.speech_seconds:6.2f} s after the speaker stopped"
    )


async def discover(timeout: float = 5.0) -> str:
    """Find the server by mDNS and return its WebSocket address."""
    from zeroconf import ServiceStateChange
    from zeroconf.asyncio import AsyncServiceBrowser, AsyncServiceInfo, AsyncZeroconf

    found: asyncio.Queue[str] = asyncio.Queue()

    def on_change(zeroconf, service_type, name, state_change) -> None:
        if state_change is ServiceStateChange.Added:
            found.put_nowait(name)

    zeroconf = AsyncZeroconf()
    browser = AsyncServiceBrowser(
        zeroconf.zeroconf, MDNS_SERVICE_TYPE, handlers=[on_change]
    )
    try:
        async with asyncio.timeout(timeout):
            name = await found.get()
        info = AsyncServiceInfo(MDNS_SERVICE_TYPE, name)
        if not await info.async_request(zeroconf.zeroconf, 3000):
            raise SystemExit("mDNS: found the service but not its address")
        address = info.parsed_addresses()[0]
        path = (info.properties.get(b"path") or STREAM_PATH.encode()).decode()
        # The address is the machine's own; it is shown, never stored.
        print(f"mDNS: found {name.split('.')[0]!r} on port {info.port}")
        return f"ws://{address}:{info.port}{path}"
    except TimeoutError:
        raise SystemExit("mDNS: no Hey Claude server found on the network") from None
    finally:
        await browser.async_cancel()
        await zeroconf.async_close()


def device_token(device_id: str) -> str:
    token = os.environ.get("HEY_CLAUDE_DEVICE_TOKEN")
    if token:
        return token
    from server.config import get_settings

    secret = get_settings().device_tokens.get(device_id)
    if secret is None:
        raise SystemExit(
            f"no token for device {device_id!r}: add it to DEVICE_TOKENS in "
            "server/.env, or set HEY_CLAUDE_DEVICE_TOKEN"
        )
    return secret.get_secret_value()


async def main_async(args: argparse.Namespace) -> int:
    requests = [(path, read_request(path)) for path in args.wav]
    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)

    if args.discover:
        url = await discover()
    elif args.url:
        url = args.url
    else:
        from server.config import get_settings

        url = f"ws://127.0.0.1:{get_settings().bind_port}{STREAM_PATH}"

    headers = {
        "Authorization": f"Bearer {device_token(args.device_id)}",
        "X-Device-Id": args.device_id,
    }
    failed = False
    try:
        async with connect(url, additional_headers=headers) as ws:
            ready = json.loads(await ws.recv())
            if ready.get("type") != "ready":
                raise SystemExit(f"expected ready, got {ready.get('type')!r}")
            print(f"connected, protocol {ready.get('protocol')}")
            for path, pcm in requests:
                result = await request(ws, pcm)
                report(path.name, result)
                failed = failed or result.error is not None
                if args.out_dir and result.audio:
                    out = args.out_dir / f"{path.stem}.reply.wav"
                    write_reply(out, bytes(result.audio), result.sample_rate)
                    print(f"  saved                   {out}")
    except (OSError, WebSocketException) as exc:
        # Only the kind of failure: the text can repeat the request headers.
        raise SystemExit(f"connection failed ({type(exc).__name__})") from None
    return 1 if failed else 0


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Send recorded requests to the Hey Claude server."
    )
    parser.add_argument("wav", nargs="+", type=Path, help="16 kHz mono 16-bit WAV")
    parser.add_argument("--out-dir", type=Path, help="save each reply here as WAV")
    parser.add_argument("--url", help="server address, ws://host:port/v1/stream")
    parser.add_argument(
        "--discover", action="store_true", help="find the server by mDNS"
    )
    parser.add_argument("--device-id", default="desktop-client")
    sys.exit(asyncio.run(main_async(parser.parse_args())))


if __name__ == "__main__":
    main()
