from __future__ import annotations

import asyncio
import hmac
import json
import re
import time
from typing import Any, Callable

from aiohttp import web

from .broker import BridgeBusy, BridgeUnavailable, UpstreamError

MAX_BODY_BYTES = 16 * 1024
MIN_TOKEN_BYTES = 32
_SESSION_ID_RE = re.compile(r"[A-Za-z0-9-]{1,64}")
BROKER_KEY = web.AppKey("broker")
TOKEN_KEY = web.AppKey("bridge_token", str)
HEARTBEAT_KEY = web.AppKey("ollama_heartbeat_interval", float)

# Ollama-compatible endpoint (PicoCalc firmware speaks only this shape).
OLLAMA_MODEL_NAMES = ("muse", "muse:latest")
OLLAMA_ROLES = ("system", "user", "assistant")
OLLAMA_MAX_MESSAGES = 64
OLLAMA_CHUNK_CHARS = 256
# The firmware gives up after 15 s without a byte; an empty delta is a no-op
# to it and to any other Ollama client.
OLLAMA_HEARTBEAT_SECONDS = 4.0
OLLAMA_HEARTBEAT_MAX_SECONDS = 5.0
OLLAMA_HEARTBEAT = b'{"message":{"role":"assistant","content":""},"done":false}\n'


def _json_error(status: int, message: str) -> web.Response:
    return web.json_response({"error": message}, status=status)


def _authorized(request: web.Request) -> bool:
    expected = request.app[TOKEN_KEY].encode("utf-8")
    header = request.headers.get("Authorization", "")
    prefix = "Bearer "
    supplied = header[len(prefix):] if header.startswith(prefix) else ""
    valid_shape = header.startswith(prefix)
    try:
        supplied_bytes = supplied.encode("utf-8")
    except UnicodeEncodeError:
        supplied_bytes = b""
    return valid_shape and hmac.compare_digest(supplied_bytes, expected)


def validate_bridge_token(token: str) -> None:
    if not isinstance(token, str):
        raise ValueError("bridge bearer token must be text")
    try:
        encoded = token.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError("bridge bearer token is not valid UTF-8") from exc
    if len(encoded) < MIN_TOKEN_BYTES:
        raise ValueError(
            f"bridge bearer token must be at least {MIN_TOKEN_BYTES} UTF-8 bytes")


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

    return await _stream_turn(request, turn, lambda event: [_ndjson(event)])


async def _stream_turn(
    request: web.Request,
    turn,
    translate: Callable[[dict], list[bytes]],
    *,
    finish: Callable[[], list[bytes]] | None = None,
    heartbeat: bytes | None = None,
    heartbeat_interval: float = 0.0,
) -> web.StreamResponse:
    """Write a turn's events as NDJSON lines until it ends or the client goes.

    ``translate`` maps one broker event to zero or more complete lines and
    ``finish`` supplies any closing lines once the events are exhausted. When
    ``heartbeat`` is set, that line is written whenever nothing else has been
    for ``heartbeat_interval`` seconds.
    """
    pending: asyncio.Task | None = None
    try:
        response = web.StreamResponse(
            status=200, headers={
                "Content-Type": "application/x-ndjson",
                "Cache-Control": "no-store",
                "X-Content-Type-Options": "nosniff",
            })
        await response.prepare(request)
        iterator = turn.events().__aiter__()
        last_write = time.monotonic()
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
                if heartbeat is not None and time.monotonic() - last_write >= heartbeat_interval:
                    await response.write(heartbeat)
                    last_write = time.monotonic()
                continue
            try:
                event = pending.result()
            except StopAsyncIteration:
                for line in finish() if finish is not None else ():
                    await response.write(line)
                break
            finally:
                pending = None
            for line in translate(event):
                await response.write(line)
                last_write = time.monotonic()
        await response.write_eof()
    except BaseException:
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


def _ndjson(record: dict) -> bytes:
    return json.dumps(record, separators=(",", ":"), ensure_ascii=False).encode() + b"\n"


def _latest_user_text(payload: Any) -> str:
    """Validate an Ollama chat request and return the text to send to Muse.

    Raises ValueError with a message that is safe to return to the client.
    Everything not named here (options, tools, format, keep_alive, ...) is
    ignored: none of it reaches Muse.
    """
    if not isinstance(payload, dict):
        raise ValueError("request must be a JSON object")
    if payload.get("model") not in OLLAMA_MODEL_NAMES:
        raise ValueError("model must be muse")
    if payload.get("stream") is not True:
        raise ValueError("stream must be true")
    messages = payload.get("messages")
    if not isinstance(messages, list) or not messages:
        raise ValueError("messages must be a non-empty list")
    if len(messages) > OLLAMA_MAX_MESSAGES:
        raise ValueError(f"messages must have at most {OLLAMA_MAX_MESSAGES} entries")
    latest = None
    for message in messages:
        if not isinstance(message, dict):
            raise ValueError("each message must be an object")
        role = message.get("role")
        content = message.get("content")
        if not isinstance(role, str) or role not in OLLAMA_ROLES:
            raise ValueError("message role must be system, user or assistant")
        if not isinstance(content, str):
            raise ValueError("message content must be a string")
        if role == "user":
            latest = content
    if latest is None or not latest.strip():
        raise ValueError("the latest user message must be non-empty")
    try:
        latest.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError("message content must be valid Unicode") from exc
    return latest


def _ollama_chunks(text: str) -> list[bytes]:
    # Slicing a str cuts between code points, so no UTF-8 sequence is split.
    return [
        _ndjson({"message": {"role": "assistant", "content": text[start:start + OLLAMA_CHUNK_CHARS]},
                 "done": False})
        for start in range(0, len(text), OLLAMA_CHUNK_CHARS)
    ]


async def ollama_chat(request: web.Request) -> web.StreamResponse:
    """Ollama-shaped ``POST /api/chat`` for clients that only speak Ollama."""
    if not _authorized(request):
        return _json_error(401, "unauthorized")
    try:
        payload = await _read_json(request)
    except web.HTTPRequestEntityTooLarge:
        return _json_error(413, "request body exceeds 16384 bytes")
    except web.HTTPBadRequest:
        return _json_error(400, "invalid JSON")
    try:
        text = _latest_user_text(payload)
    except ValueError as exc:
        return _json_error(400, str(exc))

    broker = request.app[BROKER_KEY]
    try:
        turn = await broker.start_turn(text, None)
    except BridgeBusy:
        return _json_error(409, "another chat turn is active")
    except BridgeUnavailable:
        return _json_error(503, "Muse is not connected")
    except UpstreamError:
        return _json_error(502, "Muse rejected or could not accept the request")

    # Exactly one terminal record is written: {"done":true} or one error.
    ended = False

    def translate(event: dict) -> list[bytes]:
        nonlocal ended
        if ended:
            return []
        kind = event.get("type")
        if kind == "text_delta":
            text = event.get("text")
            return _ollama_chunks(text) if isinstance(text, str) else []
        if kind == "done":
            ended = True
            return [_ndjson({"done": True})]
        if kind == "error":
            ended = True
            # Fixed text: upstream detail stays in the bridge.
            return [_ndjson({"error": "Muse did not complete the reply"})]
        return []  # start/busy have no Ollama equivalent; heartbeats cover them

    def finish() -> list[bytes]:
        nonlocal ended
        if ended:
            return []
        ended = True
        return [_ndjson({"error": "Muse did not complete the reply"})]

    return await _stream_turn(
        request, turn, translate, finish=finish,
        heartbeat=OLLAMA_HEARTBEAT,
        heartbeat_interval=request.app[HEARTBEAT_KEY])


async def ollama_tags(request: web.Request) -> web.Response:
    if not _authorized(request):
        return _json_error(401, "unauthorized")
    return web.json_response({"models": [{"name": "muse", "model": "muse"}]})


async def health(request: web.Request) -> web.Response:
    broker = request.app[BROKER_KEY]
    return web.json_response({
        "ok": True,
        "connected": bool(broker.connected),
        "busy": bool(broker.busy),
    })


def create_app(
    broker, bridge_token: str, *, heartbeat_interval: float = OLLAMA_HEARTBEAT_SECONDS,
) -> web.Application:
    validate_bridge_token(bridge_token)
    if not 0 < heartbeat_interval <= OLLAMA_HEARTBEAT_MAX_SECONDS:
        raise ValueError(
            f"heartbeat interval must be within (0, {OLLAMA_HEARTBEAT_MAX_SECONDS}] seconds")
    app = web.Application(client_max_size=MAX_BODY_BYTES)
    app[BROKER_KEY] = broker
    app[TOKEN_KEY] = bridge_token
    app[HEARTBEAT_KEY] = float(heartbeat_interval)
    app.router.add_post("/v1/chat", chat)
    app.router.add_post("/api/chat", ollama_chat)
    app.router.add_get("/api/tags", ollama_tags)
    app.router.add_get("/health", health)
    return app
