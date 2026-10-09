#!/usr/bin/env python3
"""Ollama -> Hermes shim for the PicoCalc.

Accepts Ollama-format ``POST /api/chat`` (what ``ai/ollama_provider.c`` speaks)
and forwards it to Hermes's OpenAI-compatible ``/v1/chat/completions``,
translating the streamed reply back into Ollama's newline-delimited JSON.

Stdlib only.  Secrets come from the environment and are never logged.
"""
from __future__ import annotations

import argparse
import asyncio
import hmac
import ipaddress
import json
import logging
import os
import signal
import ssl
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, AsyncIterator, Mapping
from urllib.parse import urlsplit

log = logging.getLogger("hermes_shim")

DEFAULT_BIND = "127.0.0.1"
DEFAULT_PORT = 11435
DEFAULT_API_URL = "http://127.0.0.1:8642/v1/chat/completions"
DEFAULT_MODEL = "hermes-agent"

MAX_BODY_BYTES = 64 * 1024  # request body from the device
MAX_LINE_BYTES = 64 * 1024  # one SSE line from Hermes
MAX_UPSTREAM_BODY_BYTES = 1024 * 1024  # a whole stream:false reply from Hermes
MAX_HEADER_LINE_BYTES = 8 * 1024
MAX_HEADERS = 64
MAX_CONNECTIONS = 16
MIN_TOKEN_BYTES = 32
CLIENT_READ_TIMEOUT = 10.0
CONNECT_TIMEOUT = 10.0

# The firmware's json_stream holds one record in a 2048 byte buffer and fails
# the whole reply on a longer one.  A character costs at most 6 bytes once JSON
# escaped, so 256 of them plus the envelope always fits.
MAX_CONTENT_CHARS = 256
# json_stream keeps at most 256 bytes of an "error" string.
MAX_ERROR_CHARS = 120


class UpstreamError(Exception):
    """Hermes could not be reached or answered with something unusable."""


class BadRequest(ValueError):
    """The Ollama request cannot be translated."""


@dataclass(frozen=True)
class Config:
    api_key: str = field(repr=False)
    api_url: str = DEFAULT_API_URL
    model: str = DEFAULT_MODEL
    bind: str = DEFAULT_BIND
    port: int = DEFAULT_PORT
    shim_token: str | None = field(default=None, repr=False)
    allow_clients: tuple = ()
    # Seconds between keep-alive records while Hermes is silent; 0 disables.
    heartbeat: float = 5.0
    # Seconds Hermes may stay silent before the request is abandoned.
    timeout: float = 300.0


# --------------------------------------------------------------------------
# Translation (pure functions)
# --------------------------------------------------------------------------

def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def translate_request(ollama: Any, model: str = DEFAULT_MODEL) -> dict:
    """Ollama /api/chat request -> OpenAI /v1/chat/completions request."""
    if not isinstance(ollama, dict):
        raise BadRequest("request must be a JSON object")
    messages = ollama.get("messages")
    if not isinstance(messages, list) or not messages:
        raise BadRequest("messages must be a non-empty array")
    translated = []
    for message in messages:
        if not isinstance(message, dict):
            raise BadRequest("each message must be an object")
        role, content = message.get("role"), message.get("content", "")
        if not isinstance(role, str) or not role or not isinstance(content, str):
            raise BadRequest("each message needs a string role and content")
        translated.append({"role": role, "content": content})

    stream = ollama.get("stream", True)  # Ollama streams unless told not to
    if not isinstance(stream, bool):
        raise BadRequest("stream must be a boolean")
    request: dict = {"model": model, "messages": translated, "stream": stream}

    options = ollama.get("options")
    if options is None:
        options = {}
    if not isinstance(options, dict):
        raise BadRequest("options must be an object")
    num_predict = options.get("num_predict")
    if num_predict is not None:
        if not _is_number(num_predict) or int(num_predict) != num_predict:
            raise BadRequest("options.num_predict must be an integer")
        if num_predict > 0:  # Ollama spells "no limit" as -1/-2
            request["max_tokens"] = int(num_predict)
    temperature = options.get("temperature")
    if temperature is not None:
        if not _is_number(temperature):
            raise BadRequest("options.temperature must be a number")
        request["temperature"] = float(temperature)
    return request


def _done_reason(finish_reason: Any) -> str:
    return "length" if finish_reason == "length" else "stop"


def _content_records(content: str) -> list[dict]:
    content = content.replace("\x00", "")  # the firmware rejects embedded NULs
    return [
        {"message": {"role": "assistant", "content": content[i:i + MAX_CONTENT_CHARS]},
         "done": False}
        for i in range(0, len(content), MAX_CONTENT_CHARS)
    ]


def _first_choice(payload: Any) -> dict:
    choices = payload.get("choices") if isinstance(payload, dict) else None
    if isinstance(choices, list) and choices and isinstance(choices[0], dict):
        return choices[0]
    return {}


def _upstream_error_message(error: Any) -> str:
    message = error.get("message") if isinstance(error, dict) else error
    return message if isinstance(message, str) and message else "Hermes reported an error"


class StreamTranslator:
    """OpenAI SSE lines in, Ollama stream records out.

    Only text ``content`` deltas are forwarded; tool-call and tool-progress
    events are dropped.  Exactly one ``done`` record is ever produced, because
    the firmware treats anything after it as a protocol error.
    """

    def __init__(self) -> None:
        self.finished = False
        self._finish_reason: Any = None

    def feed(self, line: bytes) -> list[dict]:
        if self.finished:
            return []
        line = line.strip()
        if not line.startswith(b"data:"):
            return []  # blank separators, comments, "event:" names
        data = line[len(b"data:"):].strip()
        if data == b"[DONE]":
            return [self._done()]
        try:
            payload = json.loads(data)
        except ValueError:
            return []
        if not isinstance(payload, dict):
            return []
        if payload.get("error"):
            raise UpstreamError(_upstream_error_message(payload["error"]))
        choice = _first_choice(payload)
        if choice.get("finish_reason") is not None:
            self._finish_reason = choice["finish_reason"]
        delta = choice.get("delta")
        content = delta.get("content") if isinstance(delta, dict) else None
        return _content_records(content) if isinstance(content, str) else []

    def end(self) -> list[dict]:
        """Upstream closed: tolerate a missing [DONE] only after a finish_reason."""
        if self.finished:
            return []
        if self._finish_reason is None:
            raise UpstreamError("Hermes closed the stream early")
        return [self._done()]

    def _done(self) -> dict:
        self.finished = True
        return {"done": True, "done_reason": _done_reason(self._finish_reason)}


def translate_response(openai: Any, model: str = DEFAULT_MODEL) -> dict:
    """OpenAI chat.completion JSON -> Ollama stream:false JSON."""
    if not isinstance(openai, dict):
        raise UpstreamError("Hermes sent an unexpected reply")
    if openai.get("error"):
        raise UpstreamError(_upstream_error_message(openai["error"]))
    choice = _first_choice(openai)
    message = choice.get("message")
    if not isinstance(message, dict):
        raise UpstreamError("Hermes sent an unexpected reply")
    content = message.get("content")
    return {
        "model": model,
        "created_at": _now(),
        "message": {"role": "assistant", "content": content if isinstance(content, str) else ""},
        "done": True,
        "done_reason": _done_reason(choice.get("finish_reason")),
    }


def encode_record(record: dict) -> bytes:
    """One NDJSON line.  Raw UTF-8 like Ollama itself, which keeps records short."""
    text = json.dumps(record, ensure_ascii=False, separators=(",", ":"))
    return text.encode("utf-8", "replace") + b"\n"


def _now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


HEARTBEAT_RECORD = encode_record({"message": {"role": "assistant", "content": ""}, "done": False})


# --------------------------------------------------------------------------
# Access control
# --------------------------------------------------------------------------

def token_matches(header: str | None, expected: str) -> bool:
    prefix = "Bearer "
    if header is None or not header.startswith(prefix):
        return False
    supplied = header[len(prefix):].encode("utf-8", "replace")
    return hmac.compare_digest(supplied, expected.encode("utf-8"))


def client_allowed(peer: str, networks: tuple) -> bool:
    """Loopback is always allowed; other peers only if they match the allowlist."""
    if not networks:
        return True
    try:
        address = ipaddress.ip_address(peer.split("%", 1)[0])
    except ValueError:
        return False
    mapped = getattr(address, "ipv4_mapped", None)
    if mapped is not None:
        address = mapped
    if address.is_loopback:
        return True
    return any(address.version == network.version and address in network for network in networks)


# --------------------------------------------------------------------------
# Minimal HTTP plumbing
# --------------------------------------------------------------------------

_REASONS = {
    200: "OK", 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden", 404: "Not Found",
    405: "Method Not Allowed", 411: "Length Required", 413: "Payload Too Large",
    502: "Bad Gateway", 503: "Service Unavailable", 504: "Gateway Timeout",
}

_STREAM_HEAD = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: application/x-ndjson\r\n"
    b"Transfer-Encoding: chunked\r\n"
    b"Connection: close\r\n"
    b"\r\n"
)


async def _read_headers(reader: asyncio.StreamReader) -> dict[str, str]:
    """Header block up to the blank line; names lower-cased.  Bounded."""
    headers: dict[str, str] = {}
    for _ in range(MAX_HEADERS + 1):
        line = await reader.readline()  # ValueError past the reader's limit
        if line in (b"\r\n", b"\n"):
            return headers
        if not line.endswith(b"\n"):
            raise ValueError("truncated header block")
        name, separator, value = line.decode("latin-1").partition(":")
        if not separator:
            raise ValueError("malformed header")
        headers[name.strip().lower()] = value.strip()
    raise ValueError("too many headers")


async def _iter_body(reader: asyncio.StreamReader, headers: Mapping[str, str]) -> AsyncIterator[bytes]:
    """Decode a response body: chunked, Content-Length, or read-until-close."""
    if "chunked" in headers.get("transfer-encoding", "").lower():
        while True:
            size = int((await reader.readline()).split(b";", 1)[0].strip(), 16)
            if size == 0:
                return
            while size > 0:
                data = await reader.read(min(size, 16384))
                if not data:
                    raise UpstreamError("Hermes closed the stream early")
                size -= len(data)
                yield data
            await reader.readexactly(2)
    elif "content-length" in headers:
        remaining = int(headers["content-length"])
        while remaining > 0:
            data = await reader.read(min(remaining, 16384))
            if not data:
                raise UpstreamError("Hermes closed the stream early")
            remaining -= len(data)
            yield data
    else:
        while data := await reader.read(16384):
            yield data


async def _iter_lines(chunks: AsyncIterator[bytes], limit: int) -> AsyncIterator[bytes]:
    """Split a byte stream into lines, never buffering more than ``limit`` + a chunk."""
    buffer = bytearray()
    async for chunk in chunks:
        buffer += chunk
        while (index := buffer.find(b"\n")) >= 0:
            if index > limit:
                raise UpstreamError("Hermes sent an oversized line")
            yield bytes(buffer[:index])
            del buffer[:index + 1]
        if len(buffer) > limit:
            raise UpstreamError("Hermes sent an oversized line")
    if buffer:
        yield bytes(buffer)


def _chunk(data: bytes) -> bytes:
    return b"%x\r\n%s\r\n" % (len(data), data)


class _Reply(Exception):
    """Abort request handling with a plain JSON error response."""

    def __init__(self, status: int, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.message = message


# --------------------------------------------------------------------------
# The shim
# --------------------------------------------------------------------------

class Shim:
    def __init__(self, config: Config) -> None:
        self.config = config
        url = urlsplit(config.api_url)
        self._tls = url.scheme == "https"
        self._host = url.hostname or ""
        self._port = url.port or (443 if self._tls else 80)
        self._host_header = url.netloc
        self._target = (url.path or "/") + (f"?{url.query}" if url.query else "")
        self._connections = 0
        self._server: asyncio.AbstractServer | None = None
        self.port = config.port

    async def start(self) -> None:
        self._server = await asyncio.start_server(
            self._handle, self.config.bind, self.config.port, limit=MAX_HEADER_LINE_BYTES)
        self.port = self._server.sockets[0].getsockname()[1]

    async def close(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()

    # -- device side -------------------------------------------------------

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peername = writer.get_extra_info("peername")
        peer = peername[0] if peername else "?"
        self._connections += 1
        try:
            status = await self._serve(reader, writer, peer)
            log.info("%s %s", peer, status)
        except (ConnectionError, asyncio.IncompleteReadError):
            log.info("%s disconnected", peer)
        except Exception as exc:  # never let one connection take the server down
            log.error("%s request failed: %s", peer, type(exc).__name__)
        finally:
            self._connections -= 1
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass

    async def _serve(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter,
                     peer: str) -> str:
        try:
            if self._connections > MAX_CONNECTIONS:
                raise _Reply(503, "shim is busy")
            try:
                async with asyncio.timeout(CLIENT_READ_TIMEOUT):
                    method, path, body = await self._read_request(reader, peer)
            except TimeoutError:
                raise _Reply(400, "request timed out") from None
            if path == "/api/chat":
                if method != "POST":
                    raise _Reply(405, "method not allowed")
                return await self._chat(writer, body)
            if method != "GET":
                raise _Reply(405, "method not allowed")
            if path == "/api/tags":
                model = self.config.model
                await self._send_json(writer, 200, {"models": [{"name": model, "model": model}]})
            elif path == "/":
                await self._send(writer, 200, b"Ollama is running", "text/plain")
            else:
                raise _Reply(404, "not found")
            return f"{method} {path} 200"
        except _Reply as reply:
            await self._send_json(writer, reply.status, {"error": reply.message})
            return f"rejected {reply.status} ({reply.message})"

    async def _read_request(self, reader: asyncio.StreamReader, peer: str) -> tuple[str, str, bytes]:
        try:
            request_line = (await reader.readline()).decode("latin-1").split()
            headers = await _read_headers(reader)
        except ValueError:
            raise _Reply(400, "malformed request") from None
        if len(request_line) != 3 or not request_line[2].startswith("HTTP/1."):
            raise _Reply(400, "malformed request")
        method, path = request_line[0], request_line[1].partition("?")[0]

        # Access checks come before any body is read.
        if not client_allowed(peer, self.config.allow_clients):
            raise _Reply(403, "forbidden")
        token = self.config.shim_token
        if token is not None and not token_matches(headers.get("authorization"), token):
            raise _Reply(401, "unauthorized")

        if method != "POST":
            return method, path, b""
        if "transfer-encoding" in headers:
            raise _Reply(411, "Content-Length is required")
        try:
            length = int(headers["content-length"])
        except (KeyError, ValueError):
            raise _Reply(411, "Content-Length is required") from None
        if length < 0:
            raise _Reply(400, "malformed request")
        if length > MAX_BODY_BYTES:
            raise _Reply(413, f"request body exceeds {MAX_BODY_BYTES} bytes")
        return method, path, await reader.readexactly(length)

    async def _chat(self, writer: asyncio.StreamWriter, body: bytes) -> str:
        try:
            request = translate_request(json.loads(body), self.config.model)
        except BadRequest as exc:
            raise _Reply(400, str(exc)) from None
        except ValueError:  # JSONDecodeError, UnicodeDecodeError
            raise _Reply(400, "invalid JSON") from None
        payload = json.dumps(request, separators=(",", ":")).encode("utf-8")
        if request["stream"]:
            return await self._chat_stream(writer, payload)
        try:
            async with asyncio.timeout(self.config.timeout):
                reply = translate_response(await self._fetch_json(payload), self.config.model)
        except TimeoutError:
            raise _Reply(504, "Hermes timed out") from None
        except UpstreamError as exc:
            raise _Reply(502, self._error_text(exc)) from None
        await self._send_json(writer, 200, reply)
        return "POST /api/chat 200"

    async def _chat_stream(self, writer: asyncio.StreamWriter, payload: bytes) -> str:
        # Answer straight away and keep the socket warm: the firmware gives up
        # after a short idle period, and Hermes can be silent for a long time
        # while it runs tools.  Failures after this point travel as an Ollama
        # {"error": ...} record, which the firmware shows to the user.
        writer.write(_STREAM_HEAD)
        await writer.drain()

        loop = asyncio.get_running_loop()
        translator = StreamTranslator()
        lines = self._upstream_lines(payload)
        pending: asyncio.Task | None = None
        outcome = "ok"

        async def next_line() -> bytes | None:
            return await anext(lines, None)

        try:
            deadline = loop.time() + self.config.timeout
            while not translator.finished:
                if pending is None:
                    pending = asyncio.ensure_future(next_line())
                wait = deadline - loop.time()
                if self.config.heartbeat > 0:
                    wait = min(wait, self.config.heartbeat)
                done, _ = await asyncio.wait({pending}, timeout=max(wait, 0))
                if not done:
                    if loop.time() >= deadline:
                        raise UpstreamError("Hermes timed out")
                    writer.write(_chunk(HEARTBEAT_RECORD))
                    await writer.drain()
                    continue
                line, pending = pending.result(), None
                deadline = loop.time() + self.config.timeout
                records = translator.end() if line is None else translator.feed(line)
                for record in records:
                    writer.write(_chunk(encode_record(record)))
                await writer.drain()
        except UpstreamError as exc:
            outcome = "upstream error"
            writer.write(_chunk(encode_record({"error": self._error_text(exc)})))
        finally:
            if pending is not None:
                pending.cancel()
                await asyncio.gather(pending, return_exceptions=True)
            await lines.aclose()  # drops the Hermes connection, stopping the run
        writer.write(b"0\r\n\r\n")
        await writer.drain()
        return f"POST /api/chat stream {outcome}"

    async def _send(self, writer: asyncio.StreamWriter, status: int, body: bytes,
                    content_type: str) -> None:
        head = (
            f"HTTP/1.1 {status} {_REASONS.get(status, 'Error')}\r\n"
            f"Content-Type: {content_type}\r\n"
            f"Content-Length: {len(body)}\r\n"
            + ("WWW-Authenticate: Bearer\r\n" if status == 401 else "")
            + "Connection: close\r\n\r\n"
        )
        writer.write(head.encode("latin-1") + body)
        await writer.drain()

    async def _send_json(self, writer: asyncio.StreamWriter, status: int, payload: dict) -> None:
        await self._send(writer, status, encode_record(payload), "application/json")

    def _error_text(self, error: Exception) -> str:
        """Short, ASCII, and guaranteed not to carry the API key to the device."""
        text = " ".join(str(error).replace(self.config.api_key, "[redacted]").split())
        return text.encode("ascii", "replace").decode("ascii")[:MAX_ERROR_CHARS]

    # -- Hermes side -------------------------------------------------------

    async def _open_upstream(self, payload: bytes, accept: str):
        context = ssl.create_default_context() if self._tls else None
        async with asyncio.timeout(CONNECT_TIMEOUT):
            reader, writer = await asyncio.open_connection(
                self._host, self._port, ssl=context, limit=MAX_HEADER_LINE_BYTES)
        try:
            head = (
                f"POST {self._target} HTTP/1.1\r\n"
                f"Host: {self._host_header}\r\n"
                f"Authorization: Bearer {self.config.api_key}\r\n"
                "Content-Type: application/json\r\n"
                f"Accept: {accept}\r\n"
                f"Content-Length: {len(payload)}\r\n"
                "Connection: close\r\n\r\n"
            )
            writer.write(head.encode("latin-1") + payload)
            await writer.drain()
            status_line = (await reader.readline()).split(None, 2)
            if len(status_line) < 2 or not status_line[0].startswith(b"HTTP/1."):
                raise UpstreamError("Hermes sent an unexpected reply")
            status = int(status_line[1])
            headers = await _read_headers(reader)
            if status == 200:
                return reader, writer, headers
            log.warning("hermes answered HTTP %d", status)
            if status in (401, 403):
                raise UpstreamError(f"Hermes rejected the API key (HTTP {status})")
            raise UpstreamError(f"Hermes returned HTTP {status}")
        except BaseException:
            writer.close()
            raise

    async def _upstream_lines(self, payload: bytes) -> AsyncIterator[bytes]:
        writer = None
        try:
            reader, writer, headers = await self._open_upstream(payload, "text/event-stream")
            async for line in _iter_lines(_iter_body(reader, headers), MAX_LINE_BYTES):
                yield line
        except (OSError, ValueError, TimeoutError, asyncio.IncompleteReadError) as exc:
            raise self._unreachable(exc) from None
        finally:
            if writer is not None:
                writer.close()

    async def _fetch_json(self, payload: bytes) -> Any:
        writer = None
        try:
            reader, writer, headers = await self._open_upstream(payload, "application/json")
            body = bytearray()
            async for chunk in _iter_body(reader, headers):
                body += chunk
                if len(body) > MAX_UPSTREAM_BODY_BYTES:
                    raise UpstreamError("Hermes sent an oversized reply")
            return json.loads(body)
        except (OSError, ValueError, asyncio.IncompleteReadError) as exc:
            raise self._unreachable(exc) from None
        finally:
            if writer is not None:
                writer.close()

    def _unreachable(self, exc: Exception) -> UpstreamError:
        # asyncio's own TimeoutError from the connect guard is an OSError subclass.
        log.warning("hermes request failed: %s", type(exc).__name__)
        if isinstance(exc, ValueError):
            return UpstreamError("Hermes sent an unexpected reply")
        return UpstreamError("cannot reach Hermes")


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------

def _header_safe(value: str) -> bool:
    return value.isascii() and value.isprintable() and value == value.strip()


def _seconds(env: Mapping[str, str], name: str, default: float) -> float:
    try:
        value = float(env.get(name) or default)
    except ValueError:
        raise ValueError(f"{name} must be a number of seconds") from None
    if value < 0:
        raise ValueError(f"{name} must not be negative")
    return value


def load_config(env: Mapping[str, str], bind: str | None = None, port: int | None = None) -> Config:
    """Build the configuration from the environment.  Error text never includes a secret."""
    api_key = env.get("HERMES_API_KEY", "")
    if not api_key:
        raise ValueError("HERMES_API_KEY is not set")
    if not _header_safe(api_key):
        raise ValueError("HERMES_API_KEY must be printable ASCII without surrounding spaces")

    api_url = env.get("HERMES_API_URL") or DEFAULT_API_URL
    url = urlsplit(api_url)
    try:
        url.port
    except ValueError:
        raise ValueError("HERMES_API_URL has an invalid port") from None
    if url.scheme not in ("http", "https") or not url.hostname or not _header_safe(api_url):
        raise ValueError("HERMES_API_URL must be an http(s) URL")
    if url.username is not None or url.password is not None:
        raise ValueError("HERMES_API_URL must not contain credentials; use HERMES_API_KEY")

    shim_token = env.get("SHIM_TOKEN") or None
    if shim_token is not None:
        if not _header_safe(shim_token):
            raise ValueError("SHIM_TOKEN must be printable ASCII without surrounding spaces")
        if len(shim_token) < MIN_TOKEN_BYTES:
            raise ValueError(f"SHIM_TOKEN must be at least {MIN_TOKEN_BYTES} characters")

    try:
        allow = tuple(
            ipaddress.ip_network(item.strip(), strict=False)
            for item in env.get("SHIM_ALLOW_CLIENTS", "").split(",") if item.strip()
        )
    except ValueError:
        raise ValueError("SHIM_ALLOW_CLIENTS must be comma-separated IP addresses or CIDRs") from None

    if port is None:
        try:
            port = int(env.get("SHIM_PORT") or DEFAULT_PORT)
        except ValueError:
            raise ValueError("SHIM_PORT must be an integer") from None
    if not 0 <= port <= 65535:
        raise ValueError("port must be between 0 and 65535")

    timeout = _seconds(env, "HERMES_TIMEOUT", 300.0)
    if timeout == 0:
        raise ValueError("HERMES_TIMEOUT must be greater than zero")
    return Config(
        api_key=api_key,
        api_url=api_url,
        model=env.get("HERMES_MODEL") or DEFAULT_MODEL,
        bind=bind or env.get("SHIM_BIND") or DEFAULT_BIND,
        port=port,
        shim_token=shim_token,
        allow_clients=allow,
        heartbeat=_seconds(env, "SHIM_HEARTBEAT", 5.0),
        timeout=timeout,
    )


def _is_loopback(bind: str) -> bool:
    if bind == "localhost":
        return True
    try:
        return ipaddress.ip_address(bind).is_loopback
    except ValueError:
        return False


async def serve(config: Config) -> None:
    shim = Shim(config)
    await shim.start()
    log.info("listening on %s:%d -> %s (model %s)",
             config.bind, shim.port, config.api_url, config.model)
    if not _is_loopback(config.bind):
        if config.shim_token is None and not config.allow_clients:
            log.warning("listening beyond loopback with neither SHIM_TOKEN nor "
                        "SHIM_ALLOW_CLIENTS: anyone who can reach %s:%d can drive Hermes",
                        config.bind, shim.port)
        elif config.shim_token is None:
            log.warning("no SHIM_TOKEN: access is limited by client address only")
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signum in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(signum, stop.set)
    try:
        await stop.wait()
    finally:
        await shim.close()


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    # Deliberately no flags for secrets: command lines are visible in `ps`.
    parser = argparse.ArgumentParser(
        prog="hermes_shim",
        description="Ollama /api/chat shim in front of a Hermes API server. "
                    "Secrets are read from the environment (HERMES_API_KEY, SHIM_TOKEN).")
    parser.add_argument(
        "--bind", default=None,
        help="listen address (default: 127.0.0.1 or $SHIM_BIND; name a LAN address explicitly)")
    parser.add_argument("--port", type=int, default=None,
                        help=f"listen port (default: {DEFAULT_PORT} or $SHIM_PORT)")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        config = load_config(os.environ, bind=args.bind, port=args.port)
    except ValueError as exc:
        print(f"hermes_shim: {exc}", file=sys.stderr)
        return 2
    try:
        asyncio.run(serve(config))
    except OSError as exc:
        print(f"hermes_shim: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
