"""The web application: the device endpoint and a health check."""

from __future__ import annotations

import logging
from collections.abc import AsyncIterator, Callable, Mapping
from contextlib import AbstractAsyncContextManager, asynccontextmanager

from fastapi import FastAPI, WebSocket
from pydantic import SecretStr
from starlette.status import WS_1008_POLICY_VIOLATION

from server.pipeline import TurnDeps
from server.protocol import STREAM_PATH
from server.transport.auth import authenticate
from server.transport.connection import ConnectionRegistry, DeviceConnection

log = logging.getLogger(__name__)


def create_app(
    deps: TurnDeps,
    device_tokens: Mapping[str, SecretStr],
    *,
    background: Callable[[], AbstractAsyncContextManager[None]] | None = None,
) -> FastAPI:
    """Build the application.

    ``background`` is entered once the conversation store is ready and left
    at shutdown; the server uses it for what runs alongside the endpoint.
    """

    @asynccontextmanager
    async def lifespan(_: FastAPI) -> AsyncIterator[None]:
        await deps.store.initialise()
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

    @app.websocket(STREAM_PATH)
    async def stream(websocket: WebSocket) -> None:
        device_id = authenticate(
            websocket.headers.get("authorization"),
            websocket.headers.get("x-device-id"),
            device_tokens,
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
