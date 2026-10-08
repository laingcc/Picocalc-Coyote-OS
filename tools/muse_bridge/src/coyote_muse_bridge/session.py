from __future__ import annotations

import asyncio
import json
import logging
import time
import uuid
from collections.abc import Callable

from musegadget import __version__
from musegadget.executor import COMMAND_SPECS
from musegadget.link_client import DeviceDescription, LinkSession, Outcome
from musegadget.noise import Header
from musegadget.service import DEFAULT_NOISE_HOST, Service

log = logging.getLogger(__name__)

SUBSCRIBE_PATH = "/chat/subscribe"
SUBSCRIBE_APP_ID = "hatch-web"
DEFAULT_MAX_LINE_BYTES = 64 * 1024


class NDJSONParser:
    """Incremental, bounded NDJSON parser.

    Oversized or malformed lines are reported without retaining their contents;
    parsing resumes at the following newline.
    """

    def __init__(
        self,
        sink: Callable[[dict], None],
        error_sink: Callable[[Exception], None],
        max_line_bytes: int,
    ) -> None:
        self._sink = sink
        self._error_sink = error_sink
        self._max = max_line_bytes
        self._buffer = bytearray()
        self._overflow = False

    def feed(self, data: bytes) -> None:
        for part in data.splitlines(keepends=True):
            ended = part.endswith((b"\n", b"\r"))
            content = part.rstrip(b"\r\n") if ended else part
            if not self._overflow:
                if len(self._buffer) + len(content) > self._max:
                    self._buffer.clear()
                    self._overflow = True
                    self._error_sink(ValueError("subscription line too large"))
                else:
                    self._buffer.extend(content)
            if ended:
                if not self._overflow and self._buffer:
                    self._emit()
                self._buffer.clear()
                self._overflow = False

    def end(self) -> None:
        if not self._overflow and self._buffer:
            self._emit()
        self._buffer.clear()
        self._overflow = False

    def _emit(self) -> None:
        try:
            value = json.loads(self._buffer)
            if not isinstance(value, dict):
                raise ValueError("subscription row is not an object")
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
            self._error_sink(ValueError(f"malformed subscription row: {type(exc).__name__}"))
            return
        self._sink(value)


class _Subscription:
    """Request-like adapter consumed by upstream LinkSession._read_loop."""

    def __init__(
        self,
        done: asyncio.Future,
        ready: asyncio.Event,
        parser: NDJSONParser,
        failed: Callable[[Exception], None],
    ) -> None:
        self.done = done
        self._ready = ready
        self._parser = parser
        self._failed = failed
        self._status = 0
        done.add_done_callback(lambda future: None if future.cancelled() else future.exception())

    def on_frame(self, frame) -> None:
        if self.done.done():
            return
        if frame.kind == "reset":
            self._fail(ConnectionError("subscription stream reset"))
            return
        if frame.kind == "response":
            self._status = frame.value.status
            data, ended = frame.value.body, frame.value.end_body
            if self._status != 200:
                self._fail(ConnectionError(f"subscription refused with HTTP {self._status}"))
                return
            self._ready.set()
        else:
            data, ended = frame.value.data, frame.value.end_body
        if data:
            self._parser.feed(data)
        if ended:
            self._parser.end()
            self._fail(ConnectionError("subscription stream ended"))

    def _fail(self, exc: Exception) -> None:
        if not self.done.done():
            self.done.set_result(None)
        self._failed(exc)


class BridgeLinkSession(LinkSession):
    """Official LinkSession plus the missing chat subscription hook."""

    def __init__(
        self,
        *,
        event_sink: Callable[[dict], None],
        subscription_error_sink: Callable[[Exception], None],
        max_line_bytes: int = DEFAULT_MAX_LINE_BYTES,
        **kwargs,
    ) -> None:
        super().__init__(**kwargs)
        self.subscription_ready = asyncio.Event()
        self._event_sink = event_sink
        self._subscription_error_sink = subscription_error_sink
        self._max_line_bytes = max_line_bytes
        self._subscription_started = False
        self._subscription_id = 0
        self._subscription_failed_once = False
        self._subscription_fatal = asyncio.Event()

    @staticmethod
    def ndjson_parser(sink, error_sink, max_line_bytes=DEFAULT_MAX_LINE_BYTES) -> NDJSONParser:
        return NDJSONParser(sink, error_sink, max_line_bytes)

    def _handle(self, message: dict):
        was_registered = self.registered_at is not None
        outcome = super()._handle(message)
        if not was_registered and self.registered_at is not None and not self._subscription_started:
            self._subscription_started = True
            task = asyncio.create_task(self._open_subscription())
            self._tasks.add(task)
            task.add_done_callback(self._tasks.discard)
        return outcome

    async def _read_loop(self) -> Outcome:
        """Let the official reader run, but end the session if subscribe dies."""
        reader = asyncio.create_task(super()._read_loop())
        failed = asyncio.create_task(self._subscription_fatal.wait())
        try:
            done, _ = await asyncio.wait({reader, failed}, return_when=asyncio.FIRST_COMPLETED)
            if reader in done:
                return reader.result()
            return Outcome.CLOSED
        finally:
            for task in (reader, failed):
                if not task.done():
                    task.cancel()
            await asyncio.gather(reader, failed, return_exceptions=True)

    async def _open_subscription(self) -> None:
        try:
            headers = [
                Header("Content-Type", "application/json"),
                Header("Accept", "application/x-ndjson"),
                Header("x-request-id", str(uuid.uuid4())),
                Header("x-app-id", SUBSCRIBE_APP_ID),
            ]
            encrypted = self._transport.start_stream_request(
                "POST", SUBSCRIBE_PATH, headers=headers)
            self._subscription_id = encrypted.stream_id
            done = asyncio.get_running_loop().create_future()
            parser = NDJSONParser(
                self._on_subscription_row, self._subscription_row_error, self._max_line_bytes)
            self._requests[encrypted.stream_id] = _Subscription(
                done, self.subscription_ready, parser, self._subscription_failed)
            await self._send_frames(encrypted.frames)
            frames = self._transport.encrypt_body_chunk(
                encrypted.stream_id, b"{}", end_body=True)
            await self._send_frames(frames)
        except Exception as exc:
            self._subscription_failed(exc)

    def _subscription_failed(self, exc: Exception) -> None:
        if self._subscription_failed_once:
            return
        self._subscription_failed_once = True
        self.subscription_ready.clear()
        self._subscription_error_sink(exc)
        # A reply subscription is essential. Ending the official read loop lets
        # Service apply its normal reconnect/token-refresh behavior.
        self._subscription_fatal.set()

    @staticmethod
    def _subscription_row_error(exc: Exception) -> None:
        # A bad row is isolated by NDJSONParser. Do not report a session loss:
        # the stream remains usable and later rows can still complete a turn.
        log.warning("dropping subscription row: %s", exc)

    def _on_subscription_row(self, row: dict) -> None:
        if row.get("type") == "event":
            self._event_sink(row)


class BridgeService(Service):
    """Stock Service with only its LinkSession factory seam replaced."""

    def __init__(self, *, event_sink, subscription_error_sink, **kwargs) -> None:
        super().__init__(**kwargs)
        self._event_sink = event_sink
        self._subscription_error_sink = subscription_error_sink

    async def _session(self, vm: dict, pairing: dict) -> tuple[Outcome, float]:
        device = DeviceDescription(
            node_id=self.identity.node_id,
            display_name=self.display_name,
            version=__version__,
            commands=COMMAND_SPECS,
        )
        session = BridgeLinkSession(
            noise_host=pairing.get("noise_host") or DEFAULT_NOISE_HOST,
            vm_id=vm["vm_id"] or vm["vm_name"],
            vm_auth_token=vm["vm_auth_token"],
            device=device,
            run_command=self.executor.run,
            event_sink=self._event_sink,
            subscription_error_sink=self._subscription_error_sink,
        )
        log.info("connecting to Muse VM")
        started = time.monotonic()
        self._current = session
        try:
            outcome = await session.run(self._stop)
        except Exception as exc:
            log.warning("session failed: %s", type(exc).__name__)
            outcome = Outcome.CLOSED
        finally:
            self._current = None
            self._subscription_error_sink(ConnectionError("Muse session ended"))
        lasted = time.monotonic() - (session.registered_at or time.monotonic())
        log.info("session ended: %s after %.0fs", outcome.value, time.monotonic() - started)
        return outcome, lasted
