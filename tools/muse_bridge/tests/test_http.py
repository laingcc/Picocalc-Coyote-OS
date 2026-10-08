from __future__ import annotations

import asyncio
import json

from aiohttp.test_utils import TestClient, TestServer

from coyote_muse_bridge.broker import BridgeBusy, BridgeUnavailable, UpstreamError
from coyote_muse_bridge.http import MAX_BODY_BYTES, create_app


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


async def client_for(broker, token="bridge-secret"):
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
                                      headers={"Authorization": "Basic bridge-secret"})).status == 401
            calls = []
            monkeypatch.setattr("coyote_muse_bridge.http.hmac.compare_digest",
                                lambda supplied, expected: calls.append((supplied, expected)) or supplied == expected)
            auth = {"Authorization": "Bearer bridge-secret"}
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
            assert calls and all(expected == "bridge-secret" for _, expected in calls)

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
            assert "bridge-secret" not in body and "Authorization" not in body
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
                headers={"Authorization": "Bearer bridge-secret"})
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
                                         headers={"Authorization": "Bearer bridge-secret"})
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
