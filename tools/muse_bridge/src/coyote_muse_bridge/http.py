from __future__ import annotations

import asyncio
import hmac
import json
import re
from typing import Any

from aiohttp import web

from .broker import BridgeBusy, BridgeUnavailable, UpstreamError

MAX_BODY_BYTES = 16 * 1024
MIN_TOKEN_CHARS = 32
_SESSION_ID_RE = re.compile(r"[A-Za-z0-9-]{1,64}")
BROKER_KEY = web.AppKey("broker")
TOKEN_KEY = web.AppKey("bridge_token", str)


def _json_error(status: int, message: str) -> web.Response:
    return web.json_response({"error": message}, status=status)


def _authorized(request: web.Request) -> bool:
    expected = request.app[TOKEN_KEY]
    header = request.headers.get("Authorization", "")
    prefix = "Bearer "
    supplied = header[len(prefix):] if header.startswith(prefix) else ""
    valid_shape = header.startswith(prefix)
    return valid_shape and hmac.compare_digest(supplied, expected)


async def _read_json(request: web.Request) -> Any:
    if request.content_length is not None and request.content_length > MAX_BODY_BYTES:
        raise web.HTTPRequestEntityTooLarge(
            max_size=MAX_BODY_BYTES, actual_size=request.content_length)
    body = bytearray()
    async for chunk in request.content.iter_chunked(4096):
        body.extend(chunk)
        if len(body) > MAX_BODY_BYTES:
            raise web.HTTPRequestEntityTooLarge(
                max_size=MAX_BODY_BYTES, actual_size=len(body))
    try:
        return json.loads(body)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise web.HTTPBadRequest(text="invalid JSON") from exc


async def chat(request: web.Request) -> web.StreamResponse:
    if not _authorized(request):
        return _json_error(401, "unauthorized")
    try:
        payload = await _read_json(request)
    except web.HTTPRequestEntityTooLarge:
        return _json_error(413, "request body exceeds 16384 bytes")
    except web.HTTPBadRequest:
        return _json_error(400, "invalid JSON")
    if not isinstance(payload, dict):
        return _json_error(400, "request must be a JSON object")
    text = payload.get("text")
    if not isinstance(text, str) or not text.strip():
        return _json_error(400, "text must be a non-empty string")
    session_id = payload.get("session_id")
    if session_id is not None and not (
        isinstance(session_id, str) and _SESSION_ID_RE.fullmatch(session_id)
    ):
        return _json_error(400, "session_id must match [A-Za-z0-9-]{1,64}")

    broker = request.app[BROKER_KEY]
    try:
        turn = await broker.start_turn(text, session_id)
    except BridgeBusy:
        return _json_error(409, "another chat turn is active")
    except BridgeUnavailable:
        return _json_error(503, "Muse is not connected")
    except UpstreamError:
        return _json_error(502, "Muse rejected or could not accept the request")

    response = web.StreamResponse(
        status=200, headers={
            "Content-Type": "application/x-ndjson",
            "Cache-Control": "no-store",
            "X-Content-Type-Options": "nosniff",
        })
    pending: asyncio.Task | None = None
    try:
        await response.prepare(request)
        iterator = turn.events().__aiter__()
        while True:
            if pending is None:
                pending = asyncio.create_task(iterator.__anext__())
            done, _ = await asyncio.wait({pending}, timeout=0.1)
            if not done:
                transport = request.transport
                if transport is None or transport.is_closing():
                    turn.disconnect()
                    pending.cancel()
                    break
                continue
            try:
                event = pending.result()
            except StopAsyncIteration:
                break
            finally:
                pending = None
            line = json.dumps(event, separators=(",", ":"), ensure_ascii=False).encode() + b"\n"
            await response.write(line)
        await response.write_eof()
    except (ConnectionError, asyncio.CancelledError):
        turn.disconnect()
        raise
    finally:
        if pending is not None:
            pending.cancel()
            await asyncio.gather(pending, return_exceptions=True)
        transport = request.transport
        if transport is None or transport.is_closing():
            turn.disconnect()
    return response


async def health(request: web.Request) -> web.Response:
    broker = request.app[BROKER_KEY]
    return web.json_response({
        "ok": True,
        "connected": bool(broker.connected),
        "busy": bool(broker.busy),
    })


def create_app(broker, bridge_token: str) -> web.Application:
    if not isinstance(bridge_token, str) or len(bridge_token) < MIN_TOKEN_CHARS:
        raise ValueError(f"bridge bearer token must be at least {MIN_TOKEN_CHARS} characters")
    app = web.Application(client_max_size=MAX_BODY_BYTES)
    app[BROKER_KEY] = broker
    app[TOKEN_KEY] = bridge_token
    app.router.add_post("/v1/chat", chat)
    app.router.add_get("/health", health)
    return app
