from __future__ import annotations

import asyncio

import pytest

from coyote_muse_bridge.broker import (
    BridgeBroker,
    BridgeBusy,
    BridgeUnavailable,
    UpstreamError,
)


class FakeSession:
    registered_at = 1.0

    def __init__(self) -> None:
        self.calls = []
        self.replies: asyncio.Queue = asyncio.Queue()

    async def send_chat(self, text, session_id=None):
        self.calls.append((text, session_id))
        reply = await self.replies.get()
        if isinstance(reply, BaseException):
            raise reply
        return reply


def ev(kind, message_id="reply", parent="note", text="", seq=None, **payload):
    data = {"type": "event", "event": kind, "payload": {"message_id": message_id}}
    if parent is not None:
        data["payload"]["parent_message_id"] = parent
    if text:
        data["payload"]["text"] = text
    data["payload"].update(payload)
    if seq is not None:
        data["seq"] = seq
    return data


async def collect(turn):
    return [item async for item in turn.events()]


def test_ack_event_race_correlation_dedup_and_quiet_settle():
    async def scenario():
        session = FakeSession()
        broker = BridgeBroker(lambda: session, settle_timeout=0.01, reply_timeout=1, turn_timeout=1)
        starting = asyncio.create_task(broker.start_turn("hello", "side-1"))
        await asyncio.sleep(0)

        # Events may beat the POST ACK. Unrelated and duplicate positive seq are ignored.
        broker.feed_event(ev("message.assistant", "wrong", "elsewhere", "wrong", seq=1))
        broker.feed_event(ev("delta.message_start", "reply", "note", seq=2))
        broker.feed_event(ev("delta.text_append", "reply", None, "hel", seq=3))
        broker.feed_event(ev("delta.text_append", "reply", None, "duplicate", seq=3))
        await session.replies.put({"ok": True, "status": 200,
                                   "response": {"result": {"message_id": "note",
                                                            "reply_to_message_id": "parent"}}})
        turn = await starting
        broker.feed_event(ev("delta.text_append", "reply", None, "lo", seq=4))
        broker.feed_event(ev("delta.message_done", "reply", None, seq=5))
        # Persisted full event for the same message must not duplicate text.
        broker.feed_event(ev("message.assistant", "reply", "note", "hello", seq=6))
        result = await asyncio.wait_for(collect(turn), 1)
        assert session.calls == [("hello", "side-1")]
        assert result == [
            {"type": "start", "message_id": "reply"},
            {"type": "text_delta", "text": "hel"},
            {"type": "text_delta", "text": "lo"},
            {"type": "done"},
        ]
        assert not broker.busy

    asyncio.run(scenario())


def test_full_messages_chains_and_busy_events_extend_then_settle():
    async def scenario():
        session = FakeSession()
        await session.replies.put({"ok": True, "status": 200,
                                   "response": {"message_id": "note", "reply_to_message_id": "parent"}})
        broker = BridgeBroker(lambda: session, settle_timeout=0.01, busy_hold_timeout=0.04,
                              reply_timeout=1, turn_timeout=1)
        turn = await broker.start_turn("hello")
        broker.feed_event({"type": "event", "event": "agent.status",
                           "payload": {"activity_code": "working"}})
        broker.feed_event(ev("message.assistant", "one", "note", "First", seq=10))
        await asyncio.sleep(0.02)
        assert broker.busy  # Busy prevents the ordinary quiet settle.
        broker.feed_event({"type": "event", "event": "agent.status", "seq": 11,
                           "payload": {"activity_code": "idle"}})
        broker.feed_event(ev("message.assistant", "two", "one", "Second", seq=12))
        result = await asyncio.wait_for(collect(turn), 1)
        assert result == [
            {"type": "busy", "busy": True},
            {"type": "start", "message_id": "one"},
            {"type": "text_delta", "text": "First"},
            {"type": "busy", "busy": False},
            {"type": "start", "message_id": "two"},
            {"type": "text_delta", "text": "Second"},
            {"type": "done"},
        ]

    asyncio.run(scenario())


def test_real_nested_payload_shape_and_all_messages_must_complete():
    async def scenario():
        session = FakeSession()
        await session.replies.put({"ok": True, "status": 200,
                                   "response": {"message_id": "note"}})
        broker = BridgeBroker(lambda: session, settle_timeout=0.01,
                              reply_timeout=1, turn_timeout=1)
        turn = await broker.start_turn("hello")

        # Literal rows match the nested form delivered by the real subscription.
        broker.feed_event({
            "type": "event", "seq": 1,
            "payload": {
                "event_name": "delta.message_start", "id": "one",
                "parent_message_id": "note",
            },
        })
        broker.feed_event({
            "type": "event", "seq": 2,
            "event": "delta.text_append",
            "payload": {
                "event_name": "message.assistant", "message_id": "one",
                "reply_to_message_id": "note", "content": "First",
            },
        })
        broker.feed_event({
            "type": "event", "seq": 3,
            "payload": {
                "event_name": "delta.message_start", "message_id": "two",
                "reply_to_message_id": "one",
            },
        })
        broker.feed_event({
            "type": "event", "seq": 4,
            "payload": {
                "event_name": "delta.message_done", "id": "one",
                "parent_message_id": "note",
            },
        })
        await asyncio.sleep(0.03)
        assert broker.busy  # Message two is still open.
        broker.feed_event({
            "type": "event", "seq": 5,
            "payload": {
                "event": "message.assistant", "id": "two",
                "parent_message_id": "one", "display_text": "Second",
                "display_text_ready": True,
            },
        })
        broker.feed_event({
            "type": "event", "seq": 6,
            "event_name": "message.assistant", "id": "three",
            "reply_to_message_id": "two", "text": "Third",
        })
        assert await asyncio.wait_for(collect(turn), 1) == [
            {"type": "start", "message_id": "one"},
            {"type": "text_delta", "text": "First"},
            {"type": "start", "message_id": "two"},
            {"type": "text_delta", "text": "Second"},
            {"type": "start", "message_id": "three"},
            {"type": "text_delta", "text": "Third"},
            {"type": "done"},
        ]

    asyncio.run(scenario())


def test_session_loss_resets_positive_sequence_deduplication():
    async def scenario():
        session = FakeSession()
        broker = BridgeBroker(lambda: session, settle_timeout=0.005,
                              reply_timeout=1, turn_timeout=1)
        await session.replies.put({"ok": True, "response": {"message_id": "old-note"}})
        old = await broker.start_turn("old")
        broker.feed_event(ev("message.assistant", "old", "old-note", "old", seq=5))
        broker.session_lost()
        assert (await collect(old))[-1] == {"type": "error", "error": "Muse session lost"}

        await session.replies.put({"ok": True, "response": {"message_id": "new-note"}})
        new = await broker.start_turn("new")
        broker.feed_event(ev("message.assistant", "new", "new-note", "new", seq=1))
        assert await asyncio.wait_for(collect(new), 1) == [
            {"type": "start", "message_id": "new"},
            {"type": "text_delta", "text": "new"},
            {"type": "done"},
        ]

    asyncio.run(scenario())


def test_busy_unavailable_ack_failure_session_loss_and_timeouts():
    async def scenario():
        broker = BridgeBroker(lambda: None)
        with pytest.raises(BridgeUnavailable):
            await broker.start_turn("x")

        session = FakeSession()
        broker = BridgeBroker(lambda: session, ack_timeout=0.02, reply_timeout=0.02,
                              turn_timeout=0.1, settle_timeout=0.01)
        first = asyncio.create_task(broker.start_turn("first"))
        await asyncio.sleep(0)
        with pytest.raises(BridgeBusy):
            await broker.start_turn("second")
        await session.replies.put({"ok": False, "status": 429, "response": None})
        with pytest.raises(UpstreamError) as failed:
            await first
        assert failed.value.status == 429

        timeout_start = asyncio.create_task(broker.start_turn("timeout"))
        await asyncio.sleep(0)
        with pytest.raises(UpstreamError):
            await timeout_start

        await session.replies.put({"ok": True, "status": 200,
                                   "response": {"message_id": "note"}})
        turn = await broker.start_turn("no reply")
        assert [item async for item in turn.events()] == [
            {"type": "error", "error": "Muse reply timed out"}]

        await session.replies.put({"ok": True, "status": 200,
                                   "response": {"message_id": "note2"}})
        turn = await broker.start_turn("lost")
        broker.session_lost()
        assert [item async for item in turn.events()] == [
            {"type": "error", "error": "Muse session lost"}]

    asyncio.run(scenario())


def test_disconnect_quarantines_late_events_before_next_turn():
    async def scenario():
        session = FakeSession()
        broker = BridgeBroker(lambda: session, reply_timeout=0.04, turn_timeout=0.08,
                              settle_timeout=0.005)
        await session.replies.put({"ok": True, "status": 200,
                                   "response": {"message_id": "old-note"}})
        old = await broker.start_turn("old")
        old.disconnect()
        broker.feed_event(ev("message.assistant", "late", "old-note", "late"))
        with pytest.raises(BridgeBusy):
            await broker.start_turn("too soon")
        await asyncio.sleep(0.05)
        assert not broker.busy

        await session.replies.put({"ok": True, "status": 200,
                                   "response": {"message_id": "new-note"}})
        new = await broker.start_turn("new")
        broker.feed_event(ev("message.assistant", "late", "old-note", "late again"))
        broker.feed_event(ev("message.assistant", "fresh", "new-note", "fresh"))
        assert [item async for item in new.events()] == [
            {"type": "start", "message_id": "fresh"},
            {"type": "text_delta", "text": "fresh"},
            {"type": "done"},
        ]

    asyncio.run(scenario())


def test_queue_overflow_quarantines_turn_until_upstream_settles():
    async def scenario():
        session = FakeSession()
        await session.replies.put({"ok": True, "response": {"message_id": "note"}})
        broker = BridgeBroker(
            lambda: session, queue_size=1, reply_timeout=1,
            turn_timeout=1, settle_timeout=0.005)
        turn = await broker.start_turn("overflow")
        watcher = turn._state.watcher
        broker.feed_event(ev("delta.text_append", "reply", "note", "text"))
        assert await asyncio.wait_for(collect(turn), 1) == []
        await asyncio.sleep(0)
        assert broker.busy
        with pytest.raises(BridgeBusy):
            await broker.start_turn("must wait")
        broker.feed_event(ev("message.assistant", "reply", "note", "text"))
        await asyncio.sleep(0.02)
        assert not broker.busy
        assert watcher is not None and watcher.done()

    asyncio.run(scenario())


def test_ack_cancellation_and_session_loss_cannot_strand_or_revive_turn():
    async def scenario():
        session = FakeSession()
        broker = BridgeBroker(lambda: session, ack_timeout=1, reply_timeout=0.02,
                              turn_timeout=0.05)
        starting = asyncio.create_task(broker.start_turn("cancelled"))
        await asyncio.sleep(0)
        starting.cancel()
        with pytest.raises(asyncio.CancelledError):
            await starting
        assert broker.busy
        broker.session_lost()
        assert not broker.busy

        racing = asyncio.create_task(broker.start_turn("race"))
        await asyncio.sleep(0)
        broker.session_lost()
        await session.replies.put({"ok": True, "response": {"message_id": "late-note"}})
        with pytest.raises((BridgeUnavailable, UpstreamError)):
            await racing
        assert not broker.busy

    asyncio.run(scenario())


def test_uncertain_ack_failures_keep_global_turn_quarantined():
    async def scenario():
        session = FakeSession()
        broker = BridgeBroker(lambda: session, reply_timeout=0.05,
                              turn_timeout=0.1)

        await session.replies.put(ConnectionError("reset after send"))
        with pytest.raises(UpstreamError):
            await broker.start_turn("possibly accepted")
        assert broker.busy
        with pytest.raises(BridgeBusy):
            await broker.start_turn("must not overlap")
        broker.session_lost()
        assert not broker.busy

        uncertain_replies = [
            None,
            {},
            {"status": 502},
            {"ok": None},
            {"ok": True, "response": {}},
            {"ok": True, "response": {"message_id": ""}},
            {"ok": True, "response": {"message_id": "\ud800"}},
        ]
        for reply in uncertain_replies:
            await session.replies.put(reply)
            with pytest.raises(UpstreamError):
                await broker.start_turn("malformed acknowledgement")
            assert broker.busy
            with pytest.raises(BridgeBusy):
                await broker.start_turn("still must not overlap")
            broker.session_lost()
            assert not broker.busy

    asyncio.run(scenario())


def test_malformed_unicode_row_is_dropped_without_losing_session():
    async def scenario():
        session = FakeSession()
        await session.replies.put({"ok": True, "response": {"message_id": "note"}})
        broker = BridgeBroker(lambda: session, settle_timeout=0.005,
                              reply_timeout=1, turn_timeout=1)
        turn = await broker.start_turn("unicode")
        broker.feed_event(ev("delta.text_append", "bad", "note", "\ud800oops"))
        broker.feed_event(ev("message.assistant", "good", "note", "safe"))
        assert await asyncio.wait_for(collect(turn), 1) == [
            {"type": "start", "message_id": "good"},
            {"type": "text_delta", "text": "safe"},
            {"type": "done"},
        ]

    asyncio.run(scenario())
