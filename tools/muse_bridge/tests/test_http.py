from __future__ import annotations

import asyncio
from types import SimpleNamespace
from typing import Any, cast
import json

from aiohttp.test_utils import TestClient, TestServer

from coyote_muse_bridge.broker import BridgeBusy, BridgeUnavailable, UpstreamError
from coyote_muse_bridge.http import MAX_BODY_BYTES, _authorized, create_app
import pytest

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


async def client_for(broker, token=TOKEN, **options):
    client = TestClient(TestServer(create_app(broker, token, **options)))
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


def test_response_constructor_failure_disconnects_accepted_turn(monkeypatch):
    async def scenario():
        broker = Broker()
        turn = Turn([])
        broker.result = turn
        client = await client_for(broker)

        def fail_constructor(*args, **kwargs):
            raise RuntimeError("unexpected response construction failure")

        monkeypatch.setattr(
            "coyote_muse_bridge.http.web.StreamResponse", fail_constructor)
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


# ---- Ollama-compatible endpoint ---------------------------------------------

AUTH = {"Authorization": f"Bearer {TOKEN}"}


def ollama_body(**overrides):
    body = {
        "model": "muse",
        "messages": [{"role": "user", "content": "hello"}],
        "stream": True,
    }
    body.update(overrides)
    return body


def test_ollama_chat_requires_the_bridge_token_and_v1_auth_is_unchanged():
    async def scenario():
        broker = Broker()
        client = await client_for(broker)
        try:
            for headers in (
                {},
                {"Authorization": f"Basic {TOKEN}"},
                {"Authorization": f"Bearer {TOKEN}x"},
                {"Authorization": f"bearer {TOKEN}"},
                {"Authorization": "Bearer "},
            ):
                response = await client.post("/api/chat", json=ollama_body(), headers=headers)
                assert response.status == 401
                assert await response.json() == {"error": "unauthorized"}
                assert (await client.get("/api/tags", headers=headers)).status == 401
                assert (await client.post("/v1/chat", json={"text": "x"}, headers=headers)).status == 401
            # A token in the body or the query string is not a credential.
            response = await client.post(
                f"/api/chat?token={TOKEN}", json=ollama_body(token=TOKEN, api_key=TOKEN))
            assert response.status == 401
            assert broker.calls == []
        finally:
            await client.close()

    asyncio.run(scenario())


def test_ollama_chat_rejects_invalid_input_before_reaching_muse():
    async def scenario():
        broker = Broker()
        client = await client_for(broker)
        user = {"role": "user", "content": "hi"}
        invalid = [
            b"not-json",
            b"[]",
            b'"muse"',
            json.dumps(ollama_body(model="llama3")).encode(),
            json.dumps(ollama_body(model="muse:other")).encode(),
            json.dumps(ollama_body(model="Muse")).encode(),
            json.dumps(ollama_body(model=["muse"])).encode(),
            json.dumps({"messages": [user], "stream": True}).encode(),
            json.dumps(ollama_body(stream=False)).encode(),
            json.dumps(ollama_body(stream=1)).encode(),
            json.dumps(ollama_body(stream="true")).encode(),
            json.dumps({"model": "muse", "messages": [user]}).encode(),
            json.dumps({"model": "muse", "stream": True}).encode(),
            json.dumps(ollama_body(messages=[])).encode(),
            json.dumps(ollama_body(messages="hi")).encode(),
            json.dumps(ollama_body(messages={"role": "user", "content": "hi"})).encode(),
            json.dumps(ollama_body(messages=["hi"])).encode(),
            json.dumps(ollama_body(messages=[{"role": "tool", "content": "hi"}, user])).encode(),
            json.dumps(ollama_body(messages=[{"role": "User", "content": "hi"}])).encode(),
            json.dumps(ollama_body(messages=[{"role": 1, "content": "hi"}])).encode(),
            json.dumps(ollama_body(messages=[{"content": "hi"}])).encode(),
            json.dumps(ollama_body(messages=[{"role": "user"}])).encode(),
            json.dumps(ollama_body(messages=[{"role": "user", "content": ["hi"]}])).encode(),
            json.dumps(ollama_body(messages=[{"role": "user", "content": None}])).encode(),
            # No user message at all, or an empty latest one.
            json.dumps(ollama_body(messages=[{"role": "system", "content": "be brief"}])).encode(),
            json.dumps(ollama_body(messages=[{"role": "assistant", "content": "hi"}])).encode(),
            json.dumps(ollama_body(messages=[{"role": "user", "content": ""}])).encode(),
            json.dumps(ollama_body(messages=[{"role": "user", "content": " \n\t"}])).encode(),
            json.dumps(ollama_body(messages=[user, {"role": "user", "content": ""}])).encode(),
            json.dumps(ollama_body(messages=[user] * 65)).encode(),
        ]
        try:
            for body in invalid:
                response = await client.post("/api/chat", data=body, headers=AUTH)
                assert response.status == 400, body
                assert set(await response.json()) == {"error"}
            response = await client.post(
                "/api/chat", data=b"x" * (MAX_BODY_BYTES + 1), headers=AUTH,
                skip_auto_headers={"Content-Length"})
            assert response.status == 413
            assert broker.calls == []

            # The message bound is inclusive.
            response = await client.post(
                "/api/chat", json=ollama_body(messages=[user] * 64), headers=AUTH)
            assert response.status == 200
            await response.read()
            assert broker.calls == [("hi", None)]
        finally:
            await client.close()

    asyncio.run(scenario())


def test_ollama_chat_sends_only_the_latest_user_content_and_ignores_options():
    async def scenario():
        broker = Broker()
        client = await client_for(broker)
        try:
            response = await client.post("/api/chat", headers=AUTH, json={
                "model": "muse:latest",
                "stream": True,
                "messages": [
                    {"role": "system", "content": "SYSTEM-PROMPT"},
                    {"role": "user", "content": "first question"},
                    {"role": "assistant", "content": "first answer"},
                    {"role": "user", "content": "second question"},
                    {"role": "assistant", "content": "partial"},
                ],
                # None of these may reach Muse or change the request.
                "options": {"num_predict": 8, "temperature": 0.8},
                "tools": [{"type": "function", "function": {"name": "shell"}}],
                "format": "json",
                "keep_alive": "5m",
                "session_id": "../escape",
                "action": "run",
                "text": "smuggled",
            })
            assert response.status == 200
            assert response.headers["Content-Type"].startswith("application/x-ndjson")
            assert await response.read() == (
                b'{"message":{"role":"assistant","content":"hi"},"done":false}\n'
                b'{"done":true}\n')
            assert broker.calls == [("second question", None)]
        finally:
            await client.close()

    asyncio.run(scenario())


def test_ollama_chat_translates_events_and_writes_exactly_one_done():
    async def scenario():
        broker = Broker()
        broker.result = Turn([
            {"type": "start", "message_id": "m1"},
            {"type": "busy", "busy": True},
            {"type": "text_delta", "text": 'He said "hi"\n'},
            {"type": "text_delta", "text": ""},
            {"type": "text_delta", "text": 7},
            {"type": "start", "message_id": "m2"},
            {"type": "text_delta", "text": "caf\u00e9 \U0001f9e9"},
            {"type": "busy", "busy": False},
            {"type": "mystery", "text": "ignored"},
            {"type": "done"},
            {"type": "text_delta", "text": "after the end"},
            {"type": "done"},
        ])
        client = await client_for(broker)
        try:
            response = await client.post("/api/chat", json=ollama_body(), headers=AUTH)
            assert response.status == 200
            body = await response.read()
            assert body == (
                b'{"message":{"role":"assistant","content":"He said \\"hi\\"\\n"},"done":false}\n'
                b'{"message":{"role":"assistant","content":"caf\xc3\xa9 \xf0\x9f\xa7\xa9"},"done":false}\n'
                b'{"done":true}\n')
            assert body.count(b'"done":true') == 1
            assert b"message_id" not in body and b"busy" not in body
        finally:
            await client.close()

    asyncio.run(scenario())


def test_ollama_chat_chunks_long_text_at_256_characters_without_splitting_utf8():
    async def scenario():
        # 600 characters in 1320 bytes, mixing 1- to 4-byte sequences so cuts
        # land beside and inside multi-byte characters.
        text = ("a\u00e9\u20ac\U0001f9e9b" * 120)
        assert len(text) == 600
        broker = Broker()
        broker.result = Turn([{"type": "text_delta", "text": text}, {"type": "done"}])
        client = await client_for(broker)
        try:
            response = await client.post("/api/chat", json=ollama_body(), headers=AUTH)
            lines = (await response.read()).split(b"\n")
            assert lines[-1] == b"" and lines[-2] == b'{"done":true}'
            records = [json.loads(line.decode("utf-8")) for line in lines[:-2]]
            assert all(set(r) == {"message", "done"} and r["done"] is False for r in records)
            assert all(r["message"]["role"] == "assistant" for r in records)
            pieces = [r["message"]["content"] for r in records]
            assert [len(piece) for piece in pieces] == [256, 256, 88]
            assert "".join(pieces) == text
            # Every line stays far below the firmware's 2048-byte record cap.
            assert max(len(line) for line in lines) <= 1100
        finally:
            await client.close()

    asyncio.run(scenario())


def test_ollama_chat_writes_heartbeats_while_waiting_and_then_the_reply():
    async def scenario():
        gate = asyncio.Event()

        class SlowTurn(Turn):
            async def events(self):
                yield {"type": "start", "message_id": "m"}
                await gate.wait()
                yield {"type": "text_delta", "text": "late"}
                yield {"type": "done"}

        heartbeat = b'{"message":{"role":"assistant","content":""},"done":false}\n'
        broker = Broker()
        broker.result = SlowTurn([])
        client = await client_for(broker, heartbeat_interval=0.15)
        try:
            response = await client.post("/api/chat", json=ollama_body(), headers=AUTH)
            assert response.status == 200
            loop = asyncio.get_running_loop()
            began = loop.time()
            for _ in range(3):
                assert await asyncio.wait_for(response.content.readline(), 1.0) == heartbeat
            elapsed = loop.time() - began
            assert 0.25 <= elapsed < 1.5  # paced by the interval, not a busy loop
            gate.set()
            rest = await response.content.read()
            assert rest.endswith(
                b'{"message":{"role":"assistant","content":"late"},"done":false}\n{"done":true}\n')
            assert rest.replace(heartbeat, b"") == (
                b'{"message":{"role":"assistant","content":"late"},"done":false}\n{"done":true}\n')
        finally:
            await client.close()

    asyncio.run(scenario())


def test_heartbeat_interval_can_never_exceed_five_seconds():
    assert create_app(Broker(), TOKEN) is not None
    assert create_app(Broker(), TOKEN, heartbeat_interval=5.0) is not None
    for bad in (0, -1.0, 5.01, 60.0):
        with pytest.raises(ValueError):
            create_app(Broker(), TOKEN, heartbeat_interval=bad)
    from coyote_muse_bridge.http import OLLAMA_HEARTBEAT_SECONDS
    assert 0 < OLLAMA_HEARTBEAT_SECONDS <= 5.0


def test_v1_chat_stream_carries_no_ollama_heartbeat():
    async def scenario():
        gate = asyncio.Event()

        class SlowTurn(Turn):
            async def events(self):
                yield {"type": "start", "message_id": "m"}
                await gate.wait()
                yield {"type": "done"}

        broker = Broker()
        broker.result = SlowTurn([])
        client = await client_for(broker, heartbeat_interval=0.05)
        try:
            response = await client.post("/v1/chat", json={"text": "x"}, headers=AUTH)
            await asyncio.sleep(0.4)
            gate.set()
            assert await response.read() == b'{"type":"start","message_id":"m"}\n{"type":"done"}\n'
        finally:
            await client.close()

    asyncio.run(scenario())


def test_ollama_chat_maps_broker_failures_to_statuses_without_detail():
    async def scenario():
        broker = Broker()
        client = await client_for(broker)
        try:
            for exc, status, message in (
                (BridgeBusy("turn 17 of client 10.0.0.9"), 409, "another chat turn is active"),
                (BridgeUnavailable("noise handshake secret"), 503, "Muse is not connected"),
                (UpstreamError("upstream said: mgst_secret", status=500), 502,
                 "Muse rejected or could not accept the request"),
            ):
                broker.result = exc
                response = await client.post("/api/chat", json=ollama_body(), headers=AUTH)
                assert response.status == status
                assert await response.json() == {"error": message}
        finally:
            await client.close()

    asyncio.run(scenario())


def test_ollama_chat_reports_a_mid_stream_error_once_and_never_done():
    async def scenario():
        client = None
        try:
            for events in (
                [{"type": "text_delta", "text": "par"},
                 {"type": "error", "error": "Muse session lost: token mgst_secret"},
                 {"type": "done"},
                 {"type": "error", "error": "again"}],
                # The turn ended without saying how.
                [{"type": "text_delta", "text": "par"}],
            ):
                broker = Broker()
                broker.result = Turn(events)
                client = await client_for(broker)
                response = await client.post("/api/chat", json=ollama_body(), headers=AUTH)
                assert response.status == 200
                body = await response.read()
                assert body == (
                    b'{"message":{"role":"assistant","content":"par"},"done":false}\n'
                    b'{"error":"Muse did not complete the reply"}\n')
                assert b"mgst_secret" not in body and b'"done":true' not in body
                await client.close()
                client = None
        finally:
            if client is not None:
                await client.close()

    asyncio.run(scenario())


def test_ollama_chat_client_disconnect_quarantines_the_turn():
    async def scenario():
        entered = asyncio.Event()

        class WaitingTurn(Turn):
            async def events(self):
                yield {"type": "text_delta", "text": "x"}
                entered.set()
                await asyncio.Event().wait()

        broker = Broker()
        turn = WaitingTurn([])
        broker.result = turn
        client = await client_for(broker, heartbeat_interval=0.05)
        try:
            response = await client.post("/api/chat", json=ollama_body(), headers=AUTH)
            await response.content.readline()
            await entered.wait()
            response.close()
            for _ in range(100):
                if turn.disconnected:
                    break
                await asyncio.sleep(0.01)
            assert turn.disconnected
        finally:
            await client.close()

    asyncio.run(scenario())


def test_api_tags_lists_only_muse_and_requires_authentication():
    async def scenario():
        client = await client_for(Broker())
        try:
            assert (await client.get("/api/tags")).status == 401
            response = await client.get("/api/tags", headers=AUTH)
            assert response.status == 200
            assert await response.json() == {"models": [{"name": "muse", "model": "muse"}]}
            assert TOKEN not in await response.text()
        finally:
            await client.close()

    asyncio.run(scenario())


def test_only_the_documented_routes_exist():
    async def scenario():
        client = await client_for(Broker())
        try:
            for method, path in (
                ("GET", "/api/chat"), ("POST", "/api/tags"), ("POST", "/api/generate"),
                ("POST", "/api/pull"), ("DELETE", "/api/delete"), ("POST", "/api/embeddings"),
                ("POST", "/api/show"), ("GET", "/api/version"), ("POST", "/v1/chat/completions"),
                ("GET", "/"), ("POST", "/exec"), ("GET", "/api/chat/../../health/x"),
            ):
                response = await client.request(method, path, headers=AUTH)
                assert response.status in (404, 405), (method, path)
        finally:
            await client.close()

    asyncio.run(scenario())
