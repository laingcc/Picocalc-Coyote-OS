from __future__ import annotations

import asyncio
import json

from musegadget.link_client import DeviceDescription, MessageDecoder, Outcome
from musegadget.noise import (
    ApplicationResponse,
    BodyChunk,
    NoiseFrameDecoder,
    NoiseXXResponder,
    ServiceFrame,
    encode_noise_frames,
)
from musegadget.noise.transport import decode_request_envelope, encode_response_envelope

from coyote_muse_bridge.session import BridgeLinkSession
from coyote_muse_bridge.session import BridgeService


DEVICE = DeviceDescription("homelink-test", "test", "0", {})


class Pipe:
    def __init__(self, inbox: asyncio.Queue, outbox: asyncio.Queue) -> None:
        self.inbox, self.outbox = inbox, outbox

    async def send(self, data) -> None:
        await self.outbox.put(data)

    async def recv(self):
        data = await self.inbox.get()
        if data is None:
            raise ConnectionError("closed")
        return data

    async def close(self) -> None:
        await self.outbox.put(None)


class FakeVm:
    """The official SDK test harness shape: real Noise, in-memory WebSocket."""

    def __init__(self, ws: Pipe) -> None:
        self.ws = ws
        self.decoder = NoiseFrameDecoder()
        self.messages = MessageDecoder()
        self.control_id = 0

    async def handshake(self) -> None:
        responder = NoiseXXResponder()
        responder.initialize()
        await self.ws.send(responder.read_message1_and_write_message2(await self.ws.recv()))
        responder.read_message3(await self.ws.recv())
        self.send_cipher, self.recv_cipher = responder.split()

    async def next_frame(self):
        while True:
            plain = self.recv_cipher.decrypt_with_ad(b"", await self.ws.recv())
            assembled = self.decoder.decode(plain)
            if assembled is not None:
                return decode_request_envelope(assembled)

    async def send_frame(self, frame) -> None:
        for chunk in encode_noise_frames(encode_response_envelope(frame)):
            await self.ws.send(self.send_cipher.encrypt_with_ad(b"", chunk))

    async def accept_control(self) -> dict:
        request = await self.next_frame()
        self.control_id = request.stream_id
        assert (request.value.verb, request.value.path) == ("POST", "/link-control")
        await self.send_frame(ServiceFrame.response(
            self.control_id, ApplicationResponse(status=200, end_body=False)))
        while True:
            frame = await self.next_frame()
            messages = self.messages.feed(frame.value.data)
            if messages:
                return messages[0]

    async def register(self, register: dict) -> None:
        body = json.dumps({"type": "res", "id": register["id"], "ok": True},
                          separators=(",", ":")).encode()
        wire = len(body).to_bytes(4, "little") + body
        await self.send_frame(ServiceFrame.body_chunk(
            self.control_id, BodyChunk(data=wire, end_body=False)))


def make_pair(event_sink, error_sink=lambda exc: None, **kwargs):
    to_device, to_vm = asyncio.Queue(), asyncio.Queue()
    device_ws, vm_ws = Pipe(to_device, to_vm), Pipe(to_vm, to_device)

    async def connect(url, headers):
        return device_ws

    session = BridgeLinkSession(
        noise_host="gw.example", vm_id="vm", vm_auth_token="secret", device=DEVICE,
        run_command=lambda *args: {"ok": True}, connect=connect,
        event_sink=event_sink, subscription_error_sink=error_sink, **kwargs,
    )
    return session, FakeVm(vm_ws)


def test_registration_opens_subscription_and_fragmented_ndjson_is_parsed():
    async def scenario():
        events, errors = [], []
        session, vm = make_pair(events.append, errors.append)
        stop = asyncio.Event()
        running = asyncio.create_task(session.run(stop))
        await vm.handshake()
        register = await vm.accept_control()
        await vm.register(register)
        request = await vm.next_frame()
        assert (request.value.verb, request.value.path, request.value.end_body) == (
            "POST", "/chat/subscribe", False)
        headers = {h.key.lower(): h.value for h in request.value.headers}
        assert headers["accept"] == "application/x-ndjson"
        assert headers["x-app-id"] == "hatch-web"
        body = await vm.next_frame()
        assert body.value.data == b"{}" and body.value.end_body
        await vm.send_frame(ServiceFrame.response(
            request.stream_id, ApplicationResponse(status=200, body=b'{"type":"sub', end_body=False)))
        await vm.send_frame(ServiceFrame.body_chunk(
            request.stream_id, BodyChunk(
                data=b'scribed"}\nnot-json\n{"type":"event","seq":1,', end_body=False)))
        await vm.send_frame(ServiceFrame.body_chunk(
            request.stream_id, BodyChunk(data=b'"event":"delta.text_append","payload":{"message_id":"r","text":"hi"}}\n', end_body=False)))
        await asyncio.wait_for(session.subscription_ready.wait(), 1)
        for _ in range(20):
            if events:
                break
            await asyncio.sleep(0)
        assert events == [{"type": "event", "seq": 1, "event": "delta.text_append",
                           "payload": {"message_id": "r", "text": "hi"}}]
        assert errors == []
        stop.set()
        assert await running is Outcome.STOPPED

    asyncio.run(scenario())


def test_subscription_rejects_bad_status_malformed_and_oversized_lines():
    async def scenario():
        events, errors = [], []
        session, vm = make_pair(events.append, errors.append, max_line_bytes=32)
        running = asyncio.create_task(session.run(asyncio.Event()))
        await vm.handshake()
        register = await vm.accept_control()
        await vm.register(register)
        request = await vm.next_frame()
        await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(
            request.stream_id,
            ApplicationResponse(status=403, body=b"denied-secret", end_body=True)))
        assert await asyncio.wait_for(running, 1) is Outcome.CLOSED
        assert not events
        assert len(errors) == 1 and "403" in str(errors[0]) and "secret" not in str(errors[0])

        # Parser behavior is tested without a second handshake.
        received, parse_errors = [], []
        parser = BridgeLinkSession.ndjson_parser(received.append, parse_errors.append, 32)
        parser.feed(b"not-json\n" + b"x" * 40 + b"\n{\"ok\":1}\n")
        assert received == [{"ok": 1}]
        assert len(parse_errors) == 2

    asyncio.run(scenario())


def test_ndjson_parser_handles_every_fragment_boundary_and_recovers_boundedly():
    wire = b'{"type":"event","payload":{"event_name":"message.assistant"}}\r\n{"ok":2}\n'
    expected = [
        {"type": "event", "payload": {"event_name": "message.assistant"}},
        {"ok": 2},
    ]
    for split in range(len(wire) + 1):
        received, errors = [], []
        parser = BridgeLinkSession.ndjson_parser(received.append, errors.append, 128)
        parser.feed(wire[:split])
        parser.feed(wire[split:])
        parser.end()
        assert received == expected, split
        assert errors == [], split

    received, errors = [], []
    parser = BridgeLinkSession.ndjson_parser(received.append, errors.append, 16)
    for byte in b"not-json\n" + b"x" * 40 + b"\n{\"ok\":1}\n":
        parser.feed(bytes([byte]))
        assert len(parser._buffer) <= 16
    assert received == [{"ok": 1}]
    assert len(errors) == 2


def test_sdk_protected_contract_used_by_bridge_is_present():
    """Make SDK drift in the deliberately narrow protected seam loud."""
    from musegadget.link_client import LinkSession

    required = {"_handle", "_send_frames"}
    assert required <= set(dir(LinkSession))
    session, _ = make_pair(lambda event: None)
    assert isinstance(session._requests, dict)
    assert isinstance(session._tasks, set)
    # These are established by LinkSession.run before registration can invoke
    # BridgeLinkSession._open_subscription.
    assert not hasattr(session, "_transport")
    assert not hasattr(session, "_ws")


def test_bridge_service_only_replaces_session_construction(monkeypatch):
    async def scenario():
        from musegadget.executor import Account, Executor
        from musegadget.identity import Identity
        from musegadget.service import Service

        captured = {}

        class FakeBridgeSession:
            registered_at = 10.0

            def __init__(self, **kwargs):
                captured.update(kwargs)

            async def run(self, stop):
                return Outcome.STOPPED

        monkeypatch.setattr("coyote_muse_bridge.session.BridgeLinkSession", FakeBridgeSession)
        service = BridgeService(
            identity=Identity("02:00:00:00:00:01"),
            executor=Executor(Account.current()),
            event_sink=lambda event: None,
            subscription_error_sink=lambda exc: None,
        )
        assert isinstance(service, Service)
        outcome, _ = await service._session(
            {"vm_id": "vm-id", "vm_name": "name", "vm_auth_token": "vm-token"},
            {"noise_host": "noise.example"},
        )
        assert outcome is Outcome.STOPPED
        assert captured["noise_host"] == "noise.example"
        assert captured["vm_id"] == "vm-id"
        assert captured["vm_auth_token"] == "vm-token"
        assert callable(captured["event_sink"])
        assert service._current is None

    asyncio.run(scenario())
