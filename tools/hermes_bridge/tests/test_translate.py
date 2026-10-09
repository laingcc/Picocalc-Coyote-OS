import ipaddress
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import hermes_shim as shim  # noqa: E402

FIRMWARE_RECORD_MAX = 2048  # JSON_STREAM_RECORD_MAX in ai/json_stream.h


def sse(payload) -> bytes:
    return b"data: " + json.dumps(payload).encode()


def delta(content, finish_reason=None) -> bytes:
    return sse({"choices": [{"index": 0, "delta": {"content": content},
                             "finish_reason": finish_reason}]})


class TranslateRequestTest(unittest.TestCase):
    def test_firmware_request_maps_to_openai(self):
        # Byte-for-byte what ollama_provider_build_request() emits.
        body = ('{"model":"llama3","messages":[{"role":"system","content":"be brief"},'
                '{"role":"user","content":"hi"}],"stream":true,"options":{"num_predict":256}}')
        self.assertEqual(shim.translate_request(json.loads(body)), {
            "model": "hermes-agent",
            "messages": [{"role": "system", "content": "be brief"},
                         {"role": "user", "content": "hi"}],
            "stream": True,
            "max_tokens": 256,
        })

    def test_model_is_replaced_by_configured_model(self):
        request = shim.translate_request(
            {"model": "llama3", "messages": [{"role": "user", "content": "x"}]}, "other-agent")
        self.assertEqual(request["model"], "other-agent")

    def test_message_order_and_system_first_preserved(self):
        messages = [{"role": "system", "content": "s"}, {"role": "user", "content": "u1"},
                    {"role": "assistant", "content": "a1"}, {"role": "user", "content": "u2"}]
        request = shim.translate_request({"messages": messages})
        self.assertEqual(request["messages"], messages)

    def test_only_role_and_content_are_forwarded(self):
        request = shim.translate_request(
            {"messages": [{"role": "user", "content": "x", "images": ["AAAA"]}]})
        self.assertEqual(request["messages"], [{"role": "user", "content": "x"}])

    def test_temperature_is_emitted_as_float(self):
        request = shim.translate_request(
            {"messages": [{"role": "user", "content": "x"}], "options": {"temperature": 1}})
        self.assertIsInstance(request["temperature"], float)
        self.assertEqual(request["temperature"], 1.0)
        self.assertIn('"temperature": 1.0', json.dumps(request))

    def test_num_predict_becomes_integer_max_tokens(self):
        request = shim.translate_request(
            {"messages": [{"role": "user", "content": "x"}], "options": {"num_predict": 64.0}})
        self.assertEqual(request["max_tokens"], 64)
        self.assertIsInstance(request["max_tokens"], int)

    def test_unlimited_num_predict_omits_max_tokens(self):
        for value in (-1, -2, 0):
            request = shim.translate_request(
                {"messages": [{"role": "user", "content": "x"}], "options": {"num_predict": value}})
            self.assertNotIn("max_tokens", request)

    def test_absent_options_are_not_invented(self):
        request = shim.translate_request({"messages": [{"role": "user", "content": "x"}]})
        self.assertNotIn("max_tokens", request)
        self.assertNotIn("temperature", request)

    def test_stream_defaults_to_true_and_passes_through(self):
        messages = [{"role": "user", "content": "x"}]
        self.assertIs(shim.translate_request({"messages": messages})["stream"], True)
        self.assertIs(shim.translate_request({"messages": messages, "stream": False})["stream"], False)

    def test_invalid_requests_are_rejected(self):
        bad = [
            [],
            {},
            {"messages": []},
            {"messages": "hi"},
            {"messages": [{"role": "user", "content": 5}]},
            {"messages": [{"content": "x"}]},
            {"messages": [{"role": "user", "content": "x"}], "stream": "yes"},
            {"messages": [{"role": "user", "content": "x"}], "options": []},
            {"messages": [{"role": "user", "content": "x"}], "options": {"num_predict": "9"}},
            {"messages": [{"role": "user", "content": "x"}], "options": {"num_predict": 1.5}},
            {"messages": [{"role": "user", "content": "x"}], "options": {"temperature": True}},
        ]
        for request in bad:
            with self.subTest(request=request), self.assertRaises(shim.BadRequest):
                shim.translate_request(request)


class StreamTranslatorTest(unittest.TestCase):
    def test_content_delta_becomes_ollama_record(self):
        self.assertEqual(shim.StreamTranslator().feed(delta("Hel")), [
            {"message": {"role": "assistant", "content": "Hel"}, "done": False}])

    def test_done_marker_becomes_single_done_record(self):
        translator = shim.StreamTranslator()
        self.assertEqual(translator.feed(b"data: [DONE]"), [{"done": True, "done_reason": "stop"}])
        self.assertTrue(translator.finished)
        # The firmware fails the reply if anything follows the terminal record.
        self.assertEqual(translator.feed(b"data: [DONE]"), [])
        self.assertEqual(translator.feed(delta("late")), [])
        self.assertEqual(translator.end(), [])

    def test_sse_framing_variants(self):
        translator = shim.StreamTranslator()
        self.assertEqual(translator.feed(b""), [])
        self.assertEqual(translator.feed(b": keep-alive\r"), [])
        self.assertEqual(translator.feed(b"event: message"), [])
        self.assertEqual(
            translator.feed(b'data:{"choices":[{"delta":{"content":"a"}}]}\r')[0]["message"]["content"],
            "a")
        self.assertEqual(translator.feed(b"data: {not json"), [])

    def test_tool_and_role_deltas_are_ignored(self):
        translator = shim.StreamTranslator()
        ignored = [
            sse({"choices": [{"delta": {"role": "assistant"}}]}),
            sse({"choices": [{"delta": {"role": "assistant", "content": None}}]}),
            sse({"choices": [{"delta": {"content": ""}}]}),
            sse({"choices": [{"delta": {"tool_calls": [
                {"index": 0, "function": {"name": "terminal", "arguments": "{}"}}]}}]}),
            sse({"choices": []}),
            sse({"type": "hermes.tool.progress", "tool": "terminal", "status": "running"}),
            b"event: hermes.tool.progress",
        ]
        for line in ignored:
            with self.subTest(line=line):
                self.assertEqual(translator.feed(line), [])
        self.assertFalse(translator.finished)

    def test_length_finish_reason_is_reported(self):
        translator = shim.StreamTranslator()
        translator.feed(delta("x", finish_reason="length"))
        self.assertEqual(translator.feed(b"data: [DONE]"), [{"done": True, "done_reason": "length"}])

    def test_close_without_done_needs_a_finish_reason(self):
        translator = shim.StreamTranslator()
        translator.feed(delta("x", finish_reason="stop"))
        self.assertEqual(translator.end(), [{"done": True, "done_reason": "stop"}])

        truncated = shim.StreamTranslator()
        truncated.feed(delta("x"))
        with self.assertRaises(shim.UpstreamError):
            truncated.end()

    def test_error_event_raises(self):
        with self.assertRaises(shim.UpstreamError) as caught:
            shim.StreamTranslator().feed(sse({"error": {"message": "agent crashed"}}))
        self.assertEqual(str(caught.exception), "agent crashed")

    def test_large_delta_is_split_to_fit_firmware_record_buffer(self):
        # Worst cases for encoded size: control characters and 4-byte UTF-8.
        for text in ("\x01" * 3000, "\U0001F600" * 3000, 'a"\\\n' * 1000):
            with self.subTest(text=text[:4]):
                records = shim.StreamTranslator().feed(delta(text))
                self.assertGreater(len(records), 1)
                self.assertEqual("".join(r["message"]["content"] for r in records), text)
                for record in records:
                    self.assertLessEqual(len(shim.encode_record(record)), FIRMWARE_RECORD_MAX)

    def test_nul_is_stripped(self):
        records = shim.StreamTranslator().feed(delta("a\x00b"))
        self.assertEqual(records[0]["message"]["content"], "ab")

    def test_records_encode_as_single_ndjson_lines(self):
        line = shim.encode_record({"message": {"role": "assistant", "content": "a\nb é"}, "done": False})
        self.assertEqual(line.count(b"\n"), 1)
        self.assertTrue(line.endswith(b"\n"))
        self.assertEqual(json.loads(line)["message"]["content"], "a\nb é")
        self.assertEqual(json.loads(shim.HEARTBEAT_RECORD),
                         {"message": {"role": "assistant", "content": ""}, "done": False})


class TranslateResponseTest(unittest.TestCase):
    def test_openai_json_becomes_ollama_json(self):
        reply = shim.translate_response({"choices": [{
            "message": {"role": "assistant", "content": "hello"}, "finish_reason": "stop"}]})
        self.assertEqual(reply["message"], {"role": "assistant", "content": "hello"})
        self.assertIs(reply["done"], True)
        self.assertEqual(reply["done_reason"], "stop")
        self.assertEqual(reply["model"], "hermes-agent")
        self.assertIn("created_at", reply)

    def test_null_content_becomes_empty_string(self):
        reply = shim.translate_response({"choices": [{
            "message": {"role": "assistant", "content": None}, "finish_reason": "length"}]})
        self.assertEqual(reply["message"]["content"], "")
        self.assertEqual(reply["done_reason"], "length")

    def test_unusable_replies_raise(self):
        for reply in ([], {}, {"choices": []}, {"error": {"message": "nope"}}):
            with self.subTest(reply=reply), self.assertRaises(shim.UpstreamError):
                shim.translate_response(reply)


class AccessTest(unittest.TestCase):
    TOKEN = "t" * 32

    def test_token_matches(self):
        self.assertTrue(shim.token_matches("Bearer " + self.TOKEN, self.TOKEN))
        for header in (None, "", self.TOKEN, "Bearer", "Bearer " + self.TOKEN + "x",
                       "bearer " + self.TOKEN, "Basic " + self.TOKEN, "Bearer é"):
            with self.subTest(header=header):
                self.assertFalse(shim.token_matches(header, self.TOKEN))

    def test_client_allowlist(self):
        networks = (ipaddress.ip_network("192.168.1.50"), ipaddress.ip_network("10.0.0.0/24"))
        self.assertTrue(shim.client_allowed("192.168.1.50", networks))
        self.assertTrue(shim.client_allowed("10.0.0.7", networks))
        self.assertTrue(shim.client_allowed("::ffff:10.0.0.7", networks))
        self.assertTrue(shim.client_allowed("127.0.0.1", networks))
        self.assertTrue(shim.client_allowed("::1", networks))
        self.assertFalse(shim.client_allowed("192.168.1.51", networks))
        self.assertFalse(shim.client_allowed("fe80::1%eth0", networks))
        self.assertFalse(shim.client_allowed("?", networks))
        self.assertTrue(shim.client_allowed("192.168.1.51", ()))


class ConfigTest(unittest.TestCase):
    KEY = "sk-" + "k" * 40

    def test_defaults(self):
        config = shim.load_config({"HERMES_API_KEY": self.KEY})
        self.assertEqual(config.bind, "127.0.0.1")
        self.assertEqual(config.port, 11435)
        self.assertEqual(config.api_url, "http://127.0.0.1:8642/v1/chat/completions")
        self.assertEqual(config.model, "hermes-agent")
        self.assertIsNone(config.shim_token)

    def test_environment_and_argument_overrides(self):
        config = shim.load_config({
            "HERMES_API_KEY": self.KEY, "HERMES_MODEL": "m", "SHIM_BIND": "0.0.0.0",
            "SHIM_PORT": "9000", "HERMES_API_URL": "http://box:1/v1/chat/completions",
            "SHIM_ALLOW_CLIENTS": "192.168.1.50, 10.0.0.0/24",
        })
        self.assertEqual((config.bind, config.port, config.model), ("0.0.0.0", 9000, "m"))
        self.assertEqual(len(config.allow_clients), 2)
        config = shim.load_config({"HERMES_API_KEY": self.KEY, "SHIM_BIND": "0.0.0.0"},
                                  bind="192.168.1.2", port=1234)
        self.assertEqual((config.bind, config.port), ("192.168.1.2", 1234))

    def test_secrets_never_appear_in_repr_or_errors(self):
        token = "s" * 40
        config = shim.load_config({"HERMES_API_KEY": self.KEY, "SHIM_TOKEN": token})
        self.assertNotIn(self.KEY, repr(config))
        self.assertNotIn(token, repr(config))
        bad = [
            {"HERMES_API_KEY": self.KEY + "\r\nX-Evil: 1"},
            {"HERMES_API_KEY": self.KEY, "SHIM_TOKEN": "short-secret"},
            {"HERMES_API_KEY": self.KEY, "HERMES_API_URL": "http://user:hunter2@box/v1"},
        ]
        for env in bad:
            with self.subTest(env=sorted(env)), self.assertRaises(ValueError) as caught:
                shim.load_config(env)
            message = str(caught.exception)
            for secret in (self.KEY, "short-secret", "hunter2"):
                self.assertNotIn(secret, message)

    def test_invalid_configuration_is_rejected(self):
        bad = [
            {},
            {"HERMES_API_KEY": ""},
            {"HERMES_API_KEY": self.KEY, "HERMES_API_URL": "ftp://box/x"},
            {"HERMES_API_KEY": self.KEY, "HERMES_API_URL": "http://box:notaport/x"},
            {"HERMES_API_KEY": self.KEY, "SHIM_PORT": "http"},
            {"HERMES_API_KEY": self.KEY, "SHIM_PORT": "70000"},
            {"HERMES_API_KEY": self.KEY, "SHIM_ALLOW_CLIENTS": "picocalc.local"},
            {"HERMES_API_KEY": self.KEY, "HERMES_TIMEOUT": "0"},
            {"HERMES_API_KEY": self.KEY, "SHIM_HEARTBEAT": "-1"},
        ]
        for env in bad:
            with self.subTest(env=env), self.assertRaises(ValueError):
                shim.load_config(env)


if __name__ == "__main__":
    unittest.main()
