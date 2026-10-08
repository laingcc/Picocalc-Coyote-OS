from __future__ import annotations

import asyncio
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, AsyncIterator, Callable


class BridgeError(Exception):
    pass


class BridgeBusy(BridgeError):
    pass


class BridgeUnavailable(BridgeError):
    pass


class UpstreamError(BridgeError):
    def __init__(self, message: str = "Muse request failed", *, status: int | None = None) -> None:
        super().__init__(message)
        self.status = status


_END = object()
MAX_TRACKED_MESSAGES = 32
MAX_ACCUMULATED_BYTES = 64 * 1024
MAX_DELTA_BYTES = 1024


@dataclass
class _TurnState:
    queue: asyncio.Queue
    started: float
    pending: deque = field(default_factory=lambda: deque(maxlen=32))
    acked: bool = False
    note_id: str = ""
    parent_id: str = ""
    accepted: set[str] = field(default_factory=set)
    rejected: deque[str] = field(default_factory=lambda: deque(maxlen=MAX_TRACKED_MESSAGES))
    seen_complete: set[str] = field(default_factory=set)
    accumulated: dict[str, str] = field(default_factory=dict)
    busy: bool = False
    last_event: float = 0.0
    last_content: float = 0.0
    detached: bool = False
    closed: bool = False
    changed: asyncio.Event = field(default_factory=asyncio.Event)
    watcher: asyncio.Task | None = None


class Turn:
    def __init__(self, broker: "BridgeBroker", state: _TurnState) -> None:
        self._broker = broker
        self._state = state

    async def events(self) -> AsyncIterator[dict]:
        try:
            while True:
                item = await self._state.queue.get()
                if item is _END:
                    return
                yield item
        finally:
            if not self._state.closed:
                self.disconnect()

    def disconnect(self) -> None:
        self._broker._disconnect(self._state)


class BridgeBroker:
    """Correlate the global Muse event subscription with one serialized turn."""

    def __init__(
        self,
        session_provider: Callable[[], Any],
        *,
        ack_timeout: float = 60.0,
        reply_timeout: float = 300.0,
        turn_timeout: float = 900.0,
        settle_timeout: float = 3.0,
        busy_hold_timeout: float = 300.0,
        queue_size: int = 64,
    ) -> None:
        self._session_provider = session_provider
        self._ack_timeout = ack_timeout
        self._reply_timeout = reply_timeout
        self._turn_timeout = turn_timeout
        self._settle_timeout = settle_timeout
        self._busy_hold_timeout = busy_hold_timeout
        self._queue_size = queue_size
        self._active: _TurnState | None = None
        self._last_seq = 0

    @property
    def connected(self) -> bool:
        session = self._session_provider()
        if session is None or session.registered_at is None:
            return False
        ready = getattr(session, "subscription_ready", None)
        return ready is None or ready.is_set()

    @property
    def busy(self) -> bool:
        return self._active is not None

    async def start_turn(self, text: str, session_id: str | None = None) -> Turn:
        if self._active is not None:
            raise BridgeBusy("another turn is active")
        session = self._session_provider()
        if session is None or session.registered_at is None:
            raise BridgeUnavailable("not connected to Muse")
        ready = getattr(session, "subscription_ready", None)
        if ready is not None and not ready.is_set():
            raise BridgeUnavailable("Muse reply subscription is not ready")

        state = _TurnState(asyncio.Queue(self._queue_size), time.monotonic())
        self._active = state
        try:
            reply = await asyncio.wait_for(
                session.send_chat(text, session_id), timeout=self._ack_timeout)
        except TimeoutError as exc:
            self._release(state)
            raise UpstreamError("Muse acknowledgement timed out") from exc
        except Exception as exc:
            self._release(state)
            raise UpstreamError("Muse request failed") from exc

        if not isinstance(reply, dict) or not reply.get("ok"):
            status = reply.get("status") if isinstance(reply, dict) else None
            self._release(state)
            raise UpstreamError("Muse rejected the request", status=status)
        result = reply.get("response")
        if isinstance(result, dict) and isinstance(result.get("result"), dict):
            result = result["result"]
        if not isinstance(result, dict) or not isinstance(result.get("message_id"), str):
            self._release(state)
            raise UpstreamError("Muse acknowledgement had no message id")

        state.note_id = result["message_id"]
        parent = result.get("reply_to_message_id")
        state.parent_id = parent if isinstance(parent, str) else ""
        state.acked = True
        pending = list(state.pending)
        state.pending.clear()
        for event in pending:
            self._process(state, event)
        state.watcher = asyncio.create_task(self._watch(state))
        return Turn(self, state)

    def feed_event(self, event: dict) -> None:
        seq = event.get("seq") if isinstance(event, dict) else None
        if isinstance(seq, int) and not isinstance(seq, bool) and seq > 0:
            if seq <= self._last_seq:
                return
            self._last_seq = seq
        state = self._active
        if state is None or state.closed or not isinstance(event, dict):
            return
        if not state.acked:
            state.pending.append(event)
            return
        self._process(state, event)

    def session_lost(self, _exc: Exception | None = None) -> None:
        # Sequence numbers belong to one subscription session. A reconnected VM
        # may restart them at one.
        self._last_seq = 0
        state = self._active
        if state is not None:
            self._fail(state, "Muse session lost")

    def _process(self, state: _TurnState, event: dict) -> None:
        if event.get("type") != "event":
            return
        payload = event.get("payload")
        if not isinstance(payload, dict):
            payload = {}
        # Match the official ESP32 parser: payload fields override envelope
        # fields when both are present.
        kind = self._first_string(payload, "event", "event_name")
        if not kind:
            kind = self._first_string(event, "event", "event_name")
        now = time.monotonic()

        if kind in ("agent.status", "task.status"):
            code = payload.get("activity_code")
            status = payload.get("status")
            if isinstance(code, str):
                busy = bool(code and code not in ("online", "idle"))
            elif isinstance(status, str):
                busy = bool(status and status not in ("completed", "failed", "idle"))
            else:
                return
            if busy != state.busy:
                state.busy = busy
                self._emit(state, {"type": "busy", "busy": busy})
            state.last_event = now
            state.changed.set()
            return

        if kind not in (
            "delta.message_start", "delta.text_append", "delta.message_done",
            "message.assistant",
        ):
            return
        message_id = self._field(event, payload, "message_id", "id")
        if not message_id:
            return
        parent = self._field(
            event, payload, "reply_to_message_id", "parent_message_id")
        if not self._related(state, message_id, parent):
            return

        ready = self._field_value(event, payload, "display_text_ready")
        if kind == "message.assistant" and ready is False:
            return

        if message_id not in state.accepted:
            if len(state.accepted) >= MAX_TRACKED_MESSAGES:
                return
            state.accepted.add(message_id)
            self._emit(state, {"type": "start", "message_id": message_id})
        state.last_event = now

        if kind == "delta.text_append":
            text = self._field(event, payload, "display_text", "content", "text")
            if text:
                state.accumulated[message_id] = self._bounded_text(
                    state.accumulated.get(message_id, "") + text)
                state.last_content = now
                self._emit_text(state, text)
        elif kind == "delta.message_done":
            state.seen_complete.add(message_id)
        elif kind == "message.assistant":
            if message_id in state.seen_complete:
                state.changed.set()
                return
            text = self._field(event, payload, "display_text", "content", "text")
            if text:
                existing = state.accumulated.get(message_id, "")
                if text != existing:
                    delta = text[len(existing):] if existing and text.startswith(existing) else text
                    if delta:
                        self._emit_text(state, delta)
                state.accumulated[message_id] = self._bounded_text(text)
                state.last_content = now
                state.seen_complete.add(message_id)
        state.changed.set()

    @staticmethod
    def _first_string(source: dict, *names: str) -> str:
        for name in names:
            value = source.get(name)
            if isinstance(value, str) and value:
                return value
        return ""

    @classmethod
    def _field(cls, top: dict, payload: dict, *names: str) -> str:
        # The ESP32 parser reads the envelope first and then the payload, so a
        # usable payload field takes precedence. Within a row, prefer the
        # canonical spelling over its compatibility aliases.
        return cls._first_string(payload, *names) or cls._first_string(top, *names)

    @staticmethod
    def _field_value(top: dict, payload: dict, name: str):
        return payload[name] if name in payload else top.get(name)

    def _related(self, state: _TurnState, message_id: str, parent: str) -> bool:
        if message_id in state.rejected:
            return False
        valid_parents = {state.note_id, state.parent_id, *state.accepted}
        valid_parents.discard("")
        if parent and parent not in valid_parents:
            state.rejected.append(message_id)
            return False
        return True

    @staticmethod
    def _bounded_text(text: str) -> str:
        raw = text.encode("utf-8")[:MAX_ACCUMULATED_BYTES]
        return raw.decode("utf-8", errors="ignore")

    def _emit_text(self, state: _TurnState, text: str) -> None:
        raw = text.encode("utf-8")
        while raw:
            end = min(len(raw), MAX_DELTA_BYTES)
            while end < len(raw) and end and raw[end] & 0xC0 == 0x80:
                end -= 1
            chunk = raw[:end].decode("utf-8")
            self._emit(state, {"type": "text_delta", "text": chunk})
            raw = raw[end:]

    async def _watch(self, state: _TurnState) -> None:
        try:
            while self._active is state and not state.closed:
                now = time.monotonic()
                age = now - state.started
                if age >= self._turn_timeout:
                    self._fail(state, "Muse turn timed out")
                    return
                complete = bool(state.accepted) and state.accepted <= state.seen_complete
                if not complete:
                    if age >= self._reply_timeout and not state.busy:
                        self._fail(state, "Muse reply timed out")
                        return
                    delay = min(0.1, self._turn_timeout - age)
                    if not state.busy:
                        delay = min(delay, max(0.001, self._reply_timeout - age))
                else:
                    quiet_for = now - state.last_event
                    busy_hold = state.busy and now - state.last_content < self._busy_hold_timeout
                    if quiet_for >= self._settle_timeout and not busy_hold:
                        self._finish(state)
                        return
                    delay = max(0.001, self._settle_timeout - quiet_for)
                    if busy_hold:
                        delay = min(delay, max(0.001, self._busy_hold_timeout - (now - state.last_content)))
                state.changed.clear()
                try:
                    await asyncio.wait_for(state.changed.wait(), timeout=delay)
                except TimeoutError:
                    pass
        except asyncio.CancelledError:
            raise

    def _emit(self, state: _TurnState, event: dict) -> None:
        if state.detached or state.closed:
            return
        try:
            state.queue.put_nowait(event)
        except asyncio.QueueFull:
            # Never block the Muse session's global event reader.
            self._release(state)

    def _finish(self, state: _TurnState) -> None:
        if state.closed:
            return
        if not state.detached:
            self._emit(state, {"type": "done"})
        self._release(state)

    def _fail(self, state: _TurnState, message: str) -> None:
        if state.closed:
            return
        if not state.detached:
            self._emit(state, {"type": "error", "error": message})
        self._release(state)

    def _release(self, state: _TurnState) -> None:
        if state.closed:
            return
        state.closed = True
        if self._active is state:
            self._active = None
        watcher = state.watcher
        if watcher is not None and watcher is not asyncio.current_task():
            watcher.cancel()
        if not state.detached:
            try:
                state.queue.put_nowait(_END)
            except asyncio.QueueFull:
                # Make termination observable even after a producer overflow.
                state.queue.get_nowait()
                state.queue.put_nowait(_END)

    def _disconnect(self, state: _TurnState) -> None:
        if state.closed:
            return
        state.detached = True
        while not state.queue.empty():
            state.queue.get_nowait()
        # Deliberately do not cancel the Muse request: the upstream API exposes
        # no cancellation contract. The active slot stays quarantined until the
        # reply settles or times out.
        state.changed.set()
