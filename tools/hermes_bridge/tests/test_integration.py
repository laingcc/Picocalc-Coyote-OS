"""End-to-end tests: device-style HTTP client -> shim -> in-process fake Hermes."""
import asyncio
import http.client
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import hermes_shim as shim  # noqa: E402

API_KEY = "sk-test-" + "k" * 40
SHIM_TOKEN = "shim-" + "t" * 40

# Byte-for-byte the body ollama_provider_build_request() produces.
FIRMWARE_BODY = (b'{"model":"llama3","messages":[{"role":"system","content":"be brief"},'
                 b'{"role":"user","content":"hi"}],"stream":true,"options":{"num_predict":256}}')


def sse(payload) -> bytes:
    return b"data: " + json.dumps(payload).encode() + b"\n\n"


def delta(content, finish_reason=None) -> bytes:
    return sse({"choices": [{"index": 0, "delta": {"content": content},
                             "finish_reason": finish_reason}]})


DEFAULT_EVENTS = [
    sse({"choices": [{"index": 0, "delta": {"role": "assistant"}}]}),
    delta("Hel"),
    sse({"choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "c1"}]}}]}),
    delta("lo"),
    delta("", finish_reason="stop"),
    b"data: [DONE]\n\n",
]


class FakeHermes:
    """A scripted OpenAI-compatible server that records what it was sent."""

    def __init__(self):
        self.requests = []  # (request line, headers, parsed body)
        self.events = list(DEFAULT_EVENTS)
        self.status = 200
        self.delay = 0.0  # pause before each event
        self.json_reply = {"choices": [{"message": {"role": "assistant", "content": "Hello"},
                                        "finish_reason": "stop"}]}
        self.server = None
        self.port = 0
        self._tasks = set()

    async def start(self):
        self.server = await asyncio.start_server(self._handle, "127.0.0.1", 0)
        self.port = self.server.sockets[0].getsockname()[1]

    async def close(self):
        self.server.close()
        for task in self._tasks:
            task.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        await self.server.wait_closed()

    async def _handle(self, reader, writer):
        self._tasks.add(asyncio.current_task())
        try:
            request_line = (await reader.readline()).decode().strip()
            headers = {}
            while (line := await reader.readline()) not in (b"\r\n", b""):
                name, _, value = line.decode().partition(":")
                headers[name.strip().lower()] = value.strip()
            body = json.loads(await reader.readexactly(int(headers["content-length"])))
            self.requests.append((request_line, headers, body))
            if self.status != 200:
                payload = json.dumps({"error": {"message": "bad key " + API_KEY}}).encode()
                writer.write(b"HTTP/1.1 %d Nope\r\nContent-Length: %d\r\n\r\n%s"
                             % (self.status, len(payload), payload))
            elif body.get("stream"):
                writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                             b"Transfer-Encoding: chunked\r\n\r\n")
                for event in self.events:
                    if self.delay:
                        await asyncio.sleep(self.delay)
                    writer.write(b"%x\r\n%s\r\n" % (len(event), event))
                    await writer.drain()
                writer.write(b"0\r\n\r\n")
            else:
                payload = json.dumps(self.json_reply).encode()
                writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             b"Content-Length: %d\r\n\r\n%s" % (len(payload), payload))
            await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()


class ShimIntegrationTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.hermes = FakeHermes()
        await self.hermes.start()
        self.addAsyncCleanup(self.hermes.close)
        self.shim = None

    async def start_shim(self, **overrides):
        settings = {
            "api_key": API_KEY,
            "api_url": f"http://127.0.0.1:{self.hermes.port}/v1/chat/completions",
            "port": 0,
            "heartbeat": 0,
        }
        settings.update(overrides)
        self.shim = shim.Shim(shim.Config(**settings))
        await self.shim.start()
        self.addAsyncCleanup(self.shim.close)

    async def request(self, method, path, body=None, headers=None):
        """Issue a request with stdlib http.client (an independent HTTP decoder)."""
        def blocking():
            connection = http.client.HTTPConnection("127.0.0.1", self.shim.port, timeout=10)
            try:
                connection.request(method, path, body=body, headers=headers or {})
                response = connection.getresponse()
                return response.status, dict(response.getheaders()), response.read()
            finally:
                connection.close()
        return await asyncio.to_thread(blocking)

    async def chat(self, body=FIRMWARE_BODY, headers=None):
        merged = {"Content-Type": "application/json", "Connection": "close"}
        merged.update(headers or {})
        return await self.request("POST", "/api/chat", body, merged)

    @staticmethod
    def records(raw):
        return [json.loads(line) for line in raw.split(b"\n") if line]

    async def test_streaming_chat_end_to_end(self):
        await self.start_shim()
        status, headers, raw = await self.chat()

        self.assertEqual(status, 200)
        self.assertEqual(headers["Content-Type"], "application/x-ndjson")
        self.assertEqual(headers["Transfer-Encoding"], "chunked")
        self.assertEqual(raw, (
            b'{"message":{"role":"assistant","content":"Hel"},"done":false}\n'
            b'{"message":{"role":"assistant","content":"lo"},"done":false}\n'
            b'{"done":true,"done_reason":"stop"}\n'))

        request_line, upstream_headers, upstream_body = self.hermes.requests[0]
        self.assertEqual(request_line, "POST /v1/chat/completions HTTP/1.1")
        self.assertEqual(upstream_headers["authorization"], "Bearer " + API_KEY)
        self.assertEqual(upstream_body, {
            "model": "hermes-agent",
            "messages": [{"role": "system", "content": "be brief"},
                         {"role": "user", "content": "hi"}],
            "stream": True,
            "max_tokens": 256,
        })

    async def test_temperature_reaches_hermes_as_float(self):
        await self.start_shim()
        body = json.dumps({"messages": [{"role": "user", "content": "hi"}],
                           "options": {"temperature": 1, "num_predict": 32}})
        status, _, _ = await self.chat(body)
        self.assertEqual(status, 200)
        upstream = self.hermes.requests[0][2]
        self.assertIsInstance(upstream["temperature"], float)
        self.assertEqual(upstream["max_tokens"], 32)

    async def test_non_streaming_chat(self):
        await self.start_shim()
        body = json.dumps({"model": "x", "stream": False,
                           "messages": [{"role": "user", "content": "hi"}]})
        status, headers, raw = await self.chat(body)
        self.assertEqual(status, 200)
        self.assertEqual(headers["Content-Type"], "application/json")
        reply = json.loads(raw)
        self.assertEqual(reply["message"], {"role": "assistant", "content": "Hello"})
        self.assertIs(reply["done"], True)
        self.assertEqual(reply["done_reason"], "stop")
        self.assertIs(self.hermes.requests[0][2]["stream"], False)

    async def test_tags_probe(self):
        await self.start_shim()
        status, _, raw = await self.request("GET", "/api/tags")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(raw)["models"][0]["name"], "hermes-agent")
        self.assertEqual(self.hermes.requests, [])

    async def test_unknown_routes_and_methods(self):
        await self.start_shim()
        self.assertEqual((await self.request("GET", "/api/generate"))[0], 404)
        self.assertEqual((await self.request("GET", "/api/chat"))[0], 405)
        self.assertEqual((await self.request("POST", "/api/tags", b"{}"))[0], 405)

    async def test_shim_token_is_enforced(self):
        await self.start_shim(shim_token=SHIM_TOKEN)
        status, headers, _ = await self.chat()
        self.assertEqual(status, 401)
        self.assertEqual(headers["WWW-Authenticate"], "Bearer")
        self.assertEqual((await self.chat(headers={"Authorization": "Bearer wrong"}))[0], 401)
        self.assertEqual((await self.request("GET", "/api/tags"))[0], 401)
        self.assertEqual(self.hermes.requests, [])

        status, _, raw = await self.chat(headers={"Authorization": "Bearer " + SHIM_TOKEN})
        self.assertEqual(status, 200)
        self.assertIs(self.records(raw)[-1]["done"], True)
        # The device's token is for the shim only; Hermes gets its own key.
        self.assertEqual(self.hermes.requests[0][1]["authorization"], "Bearer " + API_KEY)

    async def test_oversized_request_body_is_rejected_unread(self):
        await self.start_shim()
        body = json.dumps({"messages": [{"role": "user", "content": "x" * shim.MAX_BODY_BYTES}]})
        status, _, raw = await self.chat(body)
        self.assertEqual(status, 413)
        self.assertIn("error", json.loads(raw))
        self.assertEqual(self.hermes.requests, [])

    async def test_bad_requests(self):
        await self.start_shim()
        self.assertEqual((await self.chat(b"{not json"))[0], 400)
        self.assertEqual((await self.chat(b'{"messages":[]}'))[0], 400)
        self.assertEqual((await self.chat(b"\xff\xfe"))[0], 400)
        self.assertEqual(self.hermes.requests, [])

    async def test_oversized_header_line_is_rejected(self):
        await self.start_shim()
        status, _, _ = await self.chat(headers={"X-Padding": "p" * (shim.MAX_HEADER_LINE_BYTES + 1)})
        self.assertEqual(status, 400)

    async def test_oversized_sse_line_becomes_error_record(self):
        await self.start_shim()
        self.hermes.events = [delta("ok"), delta("x" * (shim.MAX_LINE_BYTES + 1)), b"data: [DONE]\n\n"]
        status, _, raw = await self.chat()
        self.assertEqual(status, 200)
        records = self.records(raw)
        self.assertEqual(records[0]["message"]["content"], "ok")
        self.assertEqual(records[-1], {"error": "Hermes sent an oversized line"})
        self.assertFalse(any(record.get("done") for record in records))

    async def test_large_delta_is_split_into_firmware_sized_records(self):
        await self.start_shim()
        text = "\U0001F600" * 2000
        self.hermes.events = [delta(text), b"data: [DONE]\n\n"]
        _, _, raw = await self.chat()
        lines = raw.splitlines(keepends=True)
        self.assertTrue(all(len(line) <= 2048 for line in lines))
        records = self.records(raw)
        self.assertEqual("".join(r["message"]["content"] for r in records[:-1]), text)
        self.assertEqual(records[-1], {"done": True, "done_reason": "stop"})

    async def test_upstream_rejection_never_echoes_the_key(self):
        await self.start_shim()
        self.hermes.status = 401
        with self.assertLogs("hermes_shim", level="INFO") as logs:
            status, _, raw = await self.chat()
        self.assertEqual(status, 200)  # the stream had already begun
        self.assertEqual(self.records(raw), [{"error": "Hermes rejected the API key (HTTP 401)"}])
        self.assertNotIn(API_KEY.encode(), raw)
        self.assertNotIn(API_KEY, "\n".join(logs.output))

    async def test_in_stream_error_is_forwarded_with_key_redacted(self):
        await self.start_shim()
        self.hermes.events = [delta("a"), sse({"error": {"message": "boom " + API_KEY}})]
        _, _, raw = await self.chat()
        self.assertEqual(self.records(raw)[-1], {"error": "boom [redacted]"})
        self.assertNotIn(API_KEY.encode(), raw)

    async def test_non_streaming_upstream_failure_is_502(self):
        await self.start_shim()
        self.hermes.status = 500
        body = json.dumps({"stream": False, "messages": [{"role": "user", "content": "hi"}]})
        status, _, raw = await self.chat(body)
        self.assertEqual(status, 502)
        self.assertEqual(json.loads(raw), {"error": "Hermes returned HTTP 500"})

    async def test_truncated_stream_becomes_error_record(self):
        await self.start_shim()
        self.hermes.events = [delta("partial")]
        _, _, raw = await self.chat()
        records = self.records(raw)
        self.assertEqual(records[0]["message"]["content"], "partial")
        self.assertEqual(records[-1], {"error": "Hermes closed the stream early"})

    async def test_unreachable_hermes_becomes_error_record(self):
        await self.hermes.close()
        await self.start_shim()
        status, _, raw = await self.chat()
        self.assertEqual(status, 200)
        self.assertEqual(self.records(raw), [{"error": "cannot reach Hermes"}])

    async def test_heartbeats_keep_the_device_connection_alive(self):
        await self.start_shim(heartbeat=0.02)
        self.hermes.delay = 0.15
        self.hermes.events = [delta("slow"), b"data: [DONE]\n\n"]
        _, _, raw = await self.chat()
        records = self.records(raw)
        heartbeat = {"message": {"role": "assistant", "content": ""}, "done": False}
        self.assertGreaterEqual(records.count(heartbeat), 2)
        self.assertEqual(records[0], heartbeat)  # sent before Hermes said anything
        self.assertEqual("".join(r["message"]["content"] for r in records[:-1]), "slow")
        self.assertEqual(records[-1], {"done": True, "done_reason": "stop"})

    async def test_silent_hermes_times_out(self):
        await self.start_shim(timeout=0.1)
        self.hermes.delay = 5
        _, _, raw = await self.chat()
        self.assertEqual(self.records(raw), [{"error": "Hermes timed out"}])

    async def test_secrets_are_not_logged(self):
        await self.start_shim(shim_token=SHIM_TOKEN)
        with self.assertLogs(level="DEBUG") as logs:
            await self.chat(headers={"Authorization": "Bearer " + SHIM_TOKEN})
            await self.chat(headers={"Authorization": "Bearer nope-" + "n" * 40})
        output = "\n".join(logs.output)
        self.assertIn("POST /api/chat", output)
        for secret in (API_KEY, SHIM_TOKEN, "nope-"):
            self.assertNotIn(secret, output)


if __name__ == "__main__":
    unittest.main()
