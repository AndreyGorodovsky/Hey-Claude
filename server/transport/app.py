"""The web application: the device endpoint, pairing and a health check."""

from __future__ import annotations

import asyncio
import json
import logging
import re
import time
from collections.abc import AsyncIterator, Callable, Mapping
from contextlib import AbstractAsyncContextManager, asynccontextmanager

from fastapi import FastAPI, Request, WebSocket
from fastapi.responses import JSONResponse
from pydantic import SecretStr
from starlette.status import WS_1008_POLICY_VIOLATION

from server.pairing import PairedDevices
from server.pipeline import TurnDeps
from server.protocol import (
    MAX_DEVICE_ID_LENGTH,
    PAIR_CODE_DIGITS,
    PAIR_PATH,
    STREAM_PATH,
)
from server.transport.auth import authenticate
from server.transport.connection import ConnectionRegistry, DeviceConnection

log = logging.getLogger(__name__)

#: A pairing code as typed: the digits and nothing else.
_CODE = re.compile(rf"[0-9]{{{PAIR_CODE_DIGITS}}}")

#: Longest pairing request read, in bytes. A real one is under 100; the
#: limit keeps a request from anywhere on the network from filling memory,
#: since this is the one route that needs no token.
_PAIR_BODY_MAX = 512


def create_app(
    deps: TurnDeps,
    device_tokens: Mapping[str, SecretStr],
    *,
    paired: PairedDevices | None = None,
    background: Callable[[], AbstractAsyncContextManager[None]] | None = None,
) -> FastAPI:
    """Build the application.

    ``paired`` holds the paired devices and the pairing code; without it,
    only DEVICE_TOKENS is checked and there is no pairing request.
    ``background`` is entered once the conversation store is ready and left
    at shutdown; the server uses it for what runs alongside the endpoint.
    """

    @asynccontextmanager
    async def lifespan(_: FastAPI) -> AsyncIterator[None]:
        await deps.store.initialise()
        if paired is not None:
            await asyncio.to_thread(paired.initialise)
        if background is None:
            yield
        else:
            async with background():
                yield

    app = FastAPI(title="Hey Claude", lifespan=lifespan, docs_url=None, redoc_url=None)
    registry = ConnectionRegistry()

    @app.get("/healthz")
    async def healthz() -> dict[str, str]:
        return {"status": "ok"}

    if paired is not None:

        @app.post(PAIR_PATH)
        async def pair(request: Request) -> JSONResponse:
            # Read by hand rather than as a model, so that a malformed
            # request gets the same plain refusal as a wrong code and
            # nothing in the answer echoes what was sent.
            raw = b""
            async for chunk in request.stream():
                raw += chunk
                if len(raw) > _PAIR_BODY_MAX:
                    return JSONResponse({"error": "too large"}, status_code=413)
            try:
                body = json.loads(raw)
                device_id, code = body["device_id"], body["code"]
            except (ValueError, KeyError, TypeError):
                return JSONResponse({"error": "bad request"}, status_code=400)
            if (
                not isinstance(device_id, str)
                or not isinstance(code, str)
                or not 0 < len(device_id) <= MAX_DEVICE_ID_LENGTH
                or not _CODE.fullmatch(code)
            ):
                return JSONResponse({"error": "bad request"}, status_code=400)
            token = await asyncio.to_thread(paired.pair, device_id, code, time.time())
            if token is None:
                log.warning("refused a pairing: no code, or an expired or wrong one")
                return JSONResponse({"error": "refused"}, status_code=403)
            if device_id in device_tokens:
                log.warning(
                    "device %s: paired; its DEVICE_TOKENS entry is no longer used",
                    device_id,
                )
            else:
                log.info("device %s: paired", device_id)
            return JSONResponse({"token": token})

    @app.websocket(STREAM_PATH)
    async def stream(websocket: WebSocket) -> None:
        device_id = await asyncio.to_thread(
            authenticate,
            websocket.headers.get("authorization"),
            websocket.headers.get("x-device-id"),
            device_tokens,
            paired,
        )
        if device_id is None:
            log.warning("rejected a connection: device not authenticated")
            # Closing before accepting refuses the upgrade with HTTP 403.
            await websocket.close(code=WS_1008_POLICY_VIOLATION)
            return

        await websocket.accept()
        connection = DeviceConnection(websocket, device_id, deps)
        await registry.claim(device_id, connection)
        log.info("device %s: connected", device_id)
        try:
            await connection.run()
        finally:
            registry.release(device_id, connection)
            log.info("device %s: disconnected", device_id)

    return app
