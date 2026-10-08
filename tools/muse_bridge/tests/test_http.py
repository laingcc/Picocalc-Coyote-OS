from __future__ import annotations

import asyncio
from types import SimpleNamespace
from typing import Any, cast
import json

from aiohttp.test_utils import TestClient, TestServer

from coyote_muse_bridge.broker import BridgeBusy, BridgeUnavailable, UpstreamError
from coyote_muse_bridge.http import MAX_BODY_BYTES, _authorized, create_app

TOKEN = "bridge-secret-0123456789abcdef012345"


def test_utf8_token_strength_and_constant_time_comparison_use_bytes():
    token = "🧩" * 8
    app = create_app(Broker(), token)
    request = cast(Any, SimpleNamespace(
        app=app, headers={"Authorization": f"Bearer {token}"}))
    assert _authorized(request)
    request.headers["Authorization"] = f"Bearer {token[:-1]}x"
    assert not _authorized(request)


class Turn:
    def __init__(self, events):
        self.items = events
        self.disconnected = False

    async def events(self):
        for item in self.items:
            yield item

    def disconnect(self):
        self.disconnected = True


class Broker:
    connected = True
    busy = False

    def __init__(self):
        self.calls = []
        self.result = Turn([{"type": "text_delta", "text": "hi"}, {"type": "done"}])

    async def start_turn(self, text, session_id=None):
        self.calls.append((text, session_id))
        if isinstance(self.result, BaseException):
            raise self.result
        return self.result


async def client_for(broker, token=TOKEN):
    client = TestClient(TestServer(create_app(broker, token)))
    await client.start_server()
    return client


def test_auth_validation_limits_status_mapping_and_health_redaction(monkeypatch):
    async def scenario():
        broker = Broker()
        client = await client_for(broker)
        try:
            assert (await client.post("/v1/chat", json={"text": "x"})).status == 401
            assert (await client.post("/v1/chat", json={"text": "x"},
                                      headers={"Authorization": f"Basic {TOKEN}"})).status == 401
            calls = []
            monkeypatch.setattr("coyote_muse_bridge.http.hmac.compare_digest",
                                lambda supplied, expected: calls.append((supplied, expected)) or supplied == expected)
            auth = {"Authorization": f"Bearer {TOKEN}"}
            invalid = [
                b"not-json",
                json.dumps({"text": ""}).encode(),
                json.dumps({"text": 2}).encode(),
                json.dumps({"text": "x", "session_id": "../bad"}).encode(),
                json.dumps({"text": "x", "session_id": "a" * 65}).encode(),
            ]
            for body in invalid:
                response = await client.post("/v1/chat", data=body, headers=auth)
                assert response.status == 400
            assert calls and all(expected == TOKEN.encode("utf-8") for _, expected in calls)

            response = await client.post(
                "/v1/chat", data=b"x" * (MAX_BODY_BYTES + 1), headers=auth,
                skip_auto_headers={"Content-Length"})
            assert response.status == 413
            for exc, status in ((BridgeBusy(), 409), (BridgeUnavailable(), 503),
                                (UpstreamError("bad", status=500), 502)):
                broker.result = exc
                assert (await client.post("/v1/chat", json={"text": "x"}, headers=auth)).status == status

            health = await client.get("/health")
            assert health.status == 200
            body = await health.text()
            assert TOKEN not in body and "Authorization" not in body
            assert json.loads(body) == {"ok": True, "connected": True, "busy": False}
        finally:
            await client.close()

    asyncio.run(scenario())


def test_stream_is_compact_ndjson_and_each_event_is_flushed():
    async def scenario():
        gate = asyncio.Event()

        class StreamingTurn(Turn):
            async def events(self):
                yield {"type": "start", "message_id": "m"}
                await gate.wait()
                yield {"type": "text_delta", "text": "hello"}
                yield {"type": "done"}

        broker = Broker()
        broker.result = StreamingTurn([])
        client = await client_for(broker)
        try:
            response = await client.post(
                "/v1/chat", json={"text": "hello", "session_id": "side-1"},
                headers={"Authorization": f"Bearer {TOKEN}"})
            assert response.status == 200
            assert response.headers["Content-Type"].startswith("application/x-ndjson")
            assert await asyncio.wait_for(response.content.readline(), 0.2) == b'{"type":"start","message_id":"m"}\n'
            gate.set()
            assert await response.content.read() == (
                b'{"type":"text_delta","text":"hello"}\n{"type":"done"}\n')
            assert broker.calls == [("hello", "side-1")]
        finally:
            await client.close()

    asyncio.run(scenario())


def test_client_disconnect_marks_turn_without_claiming_upstream_cancellation():
    async def scenario():
        entered = asyncio.Event()

        class WaitingTurn(Turn):
            async def events(self):
                yield {"type": "start"}
                entered.set()
                await asyncio.Event().wait()

        broker = Broker()
        turn = WaitingTurn([])
        broker.result = turn
        client = await client_for(broker)
        try:
            response = await client.post("/v1/chat", json={"text": "x"},
                                         headers={"Authorization": f"Bearer {TOKEN}"})
            await response.content.readline()
            await entered.wait()
            response.close()
            for _ in range(50):
                if turn.disconnected:
                    break
                await asyncio.sleep(0.01)
            assert turn.disconnected
        finally:
            await client.close()

    asyncio.run(scenario())


def test_prepare_failure_disconnects_accepted_turn(monkeypatch):
    async def scenario():
        broker = Broker()
        turn = Turn([])
        broker.result = turn
        client = await client_for(broker)

        async def fail_prepare(self, request):
            raise RuntimeError("unexpected preparation failure")

        monkeypatch.setattr(
            "coyote_muse_bridge.http.web.StreamResponse.prepare", fail_prepare)
        try:
            try:
                await client.post(
                    "/v1/chat", json={"text": "x"},
                    headers={"Authorization": f"Bearer {TOKEN}"})
            except Exception:
                pass
            assert turn.disconnected
        finally:
            await client.close()

    asyncio.run(scenario())


def test_unexpected_stream_write_failure_disconnects_accepted_turn(monkeypatch):
    async def scenario():
        broker = Broker()
        turn = Turn([{"type": "done"}])
        broker.result = turn
        client = await client_for(broker)

        async def fail_write(self, data):
            raise RuntimeError("unexpected stream write failure")

        monkeypatch.setattr(
            "coyote_muse_bridge.http.web.StreamResponse.write", fail_write)
        try:
            try:
                await client.post(
                    "/v1/chat", json={"text": "x"},
                    headers={"Authorization": f"Bearer {TOKEN}"})
            except Exception:
                pass
            for _ in range(50):
                if turn.disconnected:
                    break
                await asyncio.sleep(0.01)
            assert turn.disconnected
        finally:
            await client.close()

    asyncio.run(scenario())
