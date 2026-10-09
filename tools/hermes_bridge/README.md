# Hermes shim

Lets the PicoCalc's existing Ollama chat client talk to a
[Hermes](https://github.com/NousResearch/hermes-agent) agent with no firmware
changes. The shim accepts Ollama's `POST /api/chat`, forwards it to Hermes's
OpenAI-compatible `/v1/chat/completions`, and translates the streamed reply back
into the newline-delimited JSON the firmware parses.

```
PicoCalc --Ollama /api/chat--> hermes_shim.py --OpenAI SSE--> hermes gateway
```

Hermes runs its tools (terminal, files, web, memory) on the host; the device
only renders the streamed text. Python 3.11+, standard library only.

## 1. Enable the Hermes API server

Add to `~/.hermes/.env`:

```
API_SERVER_ENABLED=true
API_SERVER_KEY=<a long random secret>
```

then run `hermes gateway`. It listens on `http://127.0.0.1:8642`.

## 2. Run the shim

Put the key in the environment, not on the command line:

```sh
export HERMES_API_KEY=<the API_SERVER_KEY from above>
python3 hermes_shim.py                      # 127.0.0.1:11435, this machine only
python3 hermes_shim.py --bind 192.168.1.20  # reachable by the PicoCalc on that LAN address
```

| Environment | Default | Meaning |
| --- | --- | --- |
| `HERMES_API_KEY` | required | Bearer key sent to Hermes. Never logged or echoed. |
| `HERMES_API_URL` | `http://127.0.0.1:8642/v1/chat/completions` | Hermes endpoint. |
| `HERMES_MODEL` | `hermes-agent` | Model name sent to Hermes (the device's model name is ignored). |
| `SHIM_BIND` / `--bind` | `127.0.0.1` | Listen address. |
| `SHIM_PORT` / `--port` | `11435` | Listen port. |
| `SHIM_TOKEN` | unset | If set (32+ characters), every request must carry `Authorization: Bearer <token>`. |
| `SHIM_ALLOW_CLIENTS` | unset | Comma-separated client IPs/CIDRs allowed to connect (loopback always is). |
| `SHIM_HEARTBEAT` | `5` | Seconds between keep-alive records while Hermes is silent; `0` disables. |
| `HERMES_TIMEOUT` | `300` | Seconds Hermes may stay silent before the request is abandoned. |

## 3. Point the PicoCalc at it

In the AI chat settings set the host to the machine running the shim and the
port to `11435`. The model name can be anything non-empty. Hermes turns that
involve tool use can run for a while, so consider raising `request_timeout_ms`
(default 120 s) in the device's AI config; the idle timeout is covered by the
shim's keep-alive records.

Check from another machine:

```sh
curl http://192.168.1.20:11435/api/tags
curl http://192.168.1.20:11435/api/chat \
  -d '{"model":"x","messages":[{"role":"user","content":"hi"}],"stream":true}'
```

## Security

Hermes is an agent with a shell. Anyone who can reach the shim can drive it.

- The shim binds to loopback unless you name another address. Bind to the one
  LAN address the PicoCalc uses rather than `0.0.0.0`.
- **The current firmware does not send an `Authorization` header**, so setting
  `SHIM_TOKEN` locks the PicoCalc out. Until the firmware sends its configured
  bearer token, restrict a LAN bind with `SHIM_ALLOW_CLIENTS=<PicoCalc IP>`
  (give the device a DHCP reservation) and/or a host firewall rule. An address
  allowlist is weaker than a token: it does not stop a host that can spoof the
  device's address on the same network. Use `SHIM_TOKEN` for any client that can
  send one.
- Traffic between the device and the shim, and between the shim and Hermes, is
  plain HTTP. Keep both on a trusted network.
- Request bodies are capped at 64 KiB, each SSE line from Hermes at 64 KiB, and
  concurrent connections at 16.

## What gets translated

| Ollama request | OpenAI request |
| --- | --- |
| `model` | replaced with `HERMES_MODEL` |
| `messages` | passed through in order (`role`/`content` only) |
| `options.num_predict` | `max_tokens` (omitted when zero or negative) |
| `options.temperature` | `temperature` (float) |
| `stream` | `stream` (defaults to `true`, as in Ollama) |

| Hermes stream | Sent to the device |
| --- | --- |
| `data: {"choices":[{"delta":{"content":"…"}}]}` | `{"message":{"role":"assistant","content":"…"},"done":false}` |
| `data: [DONE]` | `{"done":true,"done_reason":"stop"}` |
| tool-call / tool-progress events | dropped |
| an error, timeout or early close | `{"error":"…"}` |

Replies are sent as `application/x-ndjson`, one record per line, exactly as
Ollama does. Long deltas are split so every record fits the firmware's 2048-byte
record buffer.

## Tests

```sh
python3 -m unittest discover -s tests
```

The integration tests run the shim against an in-process fake OpenAI server; no
live Hermes is needed.
