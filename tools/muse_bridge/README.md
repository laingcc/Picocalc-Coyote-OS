# Coyote Muse Bridge

`coyote-muse-bridge` keeps the official Muse Gadget Linux connection and adds a
small authenticated streaming HTTP endpoint for Coyote OS, plus an
Ollama-compatible `POST /api/chat` so a PicoCalc can use it as its normal chat
host (see [PicoCalc setup](#picocalc-setup)). It subclasses the
SDK's `LinkSession`, opens `POST /chat/subscribe` after registration, and uses
the inherited `send_chat()` implementation. TLS, WebSocket, Noise, token
refresh, command dispatch, pairing state, and reconnect behavior remain the
official SDK's responsibility.

## Important deployment notes

- **This replaces the stock `musegadget.service`.** Do not run both: one paired
  device identity should have one live Link session. The supplied unit conflicts
  with the stock unit and reuses its `/var/lib/musegadget` identity, pairing,
  refresh token, and SDK-token state.
- The HTTP listener defaults to `127.0.0.1:8765`. Binding to a LAN address is an
  explicit operator choice (`--bind 0.0.0.0` or a specific interface address).
- The bridge endpoint is plaintext HTTP. On a LAN, the bearer token and chat
  content are visible to an on-path observer. Use a trusted isolated network or
  put TLS in a reverse proxy; do not expose the listener to the Internet.
- The bridge token is separate from the Muse SDK token and paired device tokens.
  Logs contain message lengths/state only, never message content or credentials.

## Install and pair

Python 3.11+, `uv`, Bluetooth/network prerequisites required by Muse Gadget,
and a Muse Gadget SDK token are required. The SDK dependency is pinned to
`86cf33fb4092ba700b4dc33928966d1bcb31556d` and the repository's `linux`
subdirectory in `pyproject.toml`.

```sh
cd tools/muse_bridge
uv sync --extra test
```

If this machine is not already paired, use the official CLI from the same
environment first:

```sh
sudo MUSEGADGET_SDK_TOKEN='mgst_…' uv run musegadget pair
```

An existing stock installation needs no new pairing. Stop it before manually
running the bridge:

```sh
sudo systemctl disable --now musegadget.service
```

Create a high-entropy bridge bearer without putting it in a command-line
argument:

```sh
sudo install -d -m 0750 /etc/coyote-muse-bridge
openssl rand -hex 32 | sudo tee /etc/coyote-muse-bridge/token >/dev/null
sudo chmod 0640 /etc/coyote-muse-bridge/token
```

For a manual root run, choose the unprivileged account whose permissions Muse
commands should use:

```sh
sudo uv run coyote-muse-bridge --run-as "$USER"
```

A non-root run uses the current account and can set
`MUSEGADGET_STATE_DIR` to the directory holding the existing official state.
For LAN access, add `--bind 0.0.0.0` only after considering the plaintext risk.
A bridge token may alternatively be supplied through
`COYOTE_MUSE_BRIDGE_TOKEN`; protect the environment file containing it.

## systemd

Install the package/virtual environment under `/opt/coyote-muse-bridge/.venv`,
copy `systemd/coyote-muse-bridge.service` to `/etc/systemd/system/`, then create
`/etc/coyote-muse-bridge/environment`:

```ini
MUSEGADGET_RUN_AS=coyote
# Optional and explicit LAN exposure:
# COYOTE_MUSE_BIND=0.0.0.0
```

The supplied unit reads the token file rather than an environment variable. To
change the bind address, add a systemd override that replaces `ExecStart`, for
example with `--bind 0.0.0.0`; the commented environment variable above is only
operator documentation and is not consumed automatically.

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now coyote-muse-bridge.service
sudo systemctl status coyote-muse-bridge.service
```

## HTTP API

Health is intentionally unauthenticated and discloses only connection/busy
booleans:

```sh
curl http://127.0.0.1:8765/health
```

Chat accepts at most 16 KiB of JSON. `session_id`, when present, must match
`[A-Za-z0-9-]{1,64}`.

```sh
TOKEN=$(sudo cat /etc/coyote-muse-bridge/token)
curl --no-buffer \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  --data '{"text":"Hello","session_id":"coyote-1"}' \
  http://127.0.0.1:8765/v1/chat
unset TOKEN
```

This expands the token into `curl`'s process arguments, where another local
user may briefly be able to see it. Avoid typing tokens directly into commands
saved in shell history, and use a protected curl config or equivalent secret
injection when local process-list exposure is a concern.

The response is `application/x-ndjson`; each `start`, `text_delta`, `busy`,
`done`, or `error` object is written immediately. Statuses are: `401` bad auth,
`400` invalid input, `413` oversized body, `409` another active turn, `503` no
registered/subscribed Muse session, and `502` failure before Muse accepts the
turn.

### Ollama-compatible endpoint

`POST /api/chat` takes the same bearer token and an Ollama chat request of at
most 16 KiB:

- `model` must be `muse` or `muse:latest`, and `stream` must be `true`.
- `messages` is a list of 1 to 64 objects with a `role` of `system`, `user` or
  `assistant` and a string `content`. **Only the content of the latest `user`
  message is sent to Muse**; it must not be empty. Muse keeps its own
  conversation, so the system prompt and earlier turns the client repeats are
  validated and then dropped.
- Everything else (`options`, `tools`, `format`, `keep_alive`, …) is ignored.
  Nothing in the request can select a tool, an action or a Muse session.

The reply is Ollama NDJSON. Text arrives as
`{"message":{"role":"assistant","content":"…"},"done":false}` in pieces of at
most 256 characters, a successful turn ends with exactly one `{"done":true}`,
and a turn Muse does not finish ends with one `{"error":"…"}` record instead.
While Muse is thinking, a record with empty `content` is written every 4
seconds (never more than 5 apart) so the client's idle timeout does not fire.
Rejections before the stream starts use the same statuses as `/v1/chat`, with
an `{"error":"…"}` body.

`GET /api/tags` returns the single model `muse` and requires the same token.
No other Ollama route exists.

```sh
curl --no-buffer \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  --data '{"model":"muse","stream":true,"messages":[{"role":"user","content":"Hello"}]}' \
  http://127.0.0.1:8765/api/chat
```

## PicoCalc setup

Coyote OS talks to the bridge exactly as it talks to Ollama, with one extra
setting for the token.

1. Let the bridge listen on the LAN. It binds to loopback by default; start it
   with `--bind` set to this machine's LAN address (or `0.0.0.0`). With the
   supplied unit, use a drop-in (`sudo systemctl edit coyote-muse-bridge`):

   ```ini
   [Service]
   ExecStart=
   ExecStart=/opt/coyote-muse-bridge/.venv/bin/coyote-muse-bridge --token-file /etc/coyote-muse-bridge/token --bind 192.168.1.20
   ```

2. On the PicoCalc press F5 in chat mode, choose **Connection settings**, then
   **Provider** and pick **muse** (it sits right after `ollama`): that fills in
   **Port** `8765` and **Model** `muse`. Set the one remaining value:

   | Setting | Value |
   | --- | --- |
   | Host | the bridge machine's LAN IP address, e.g. `192.168.1.20` |
   | API key | the contents of `/etc/coyote-muse-bridge/token` |

   then **Apply & connect**.

   **API key** is the single credential setting: for a cloud provider it holds
   that provider's key, and for `muse` it holds the bridge token. It is masked
   while you type it, and stored in plain text in `/coyote/ai.ini` as
   `bearer_token=`. Choosing a different provider drops it.

   The token from `openssl rand -hex 32` is 64 characters. Rather than typing
   it on the PicoCalc keyboard, you can put it on the SD card from a computer
   by adding one line to `/coyote/ai.ini` (it must look like a bearer token:
   letters, digits and `-._~+/`, with any `=` padding last):

   ```ini
   bearer_token=<contents of /etc/coyote-muse-bridge/token>
   ```

   Read the token with `sudo cat /etc/coyote-muse-bridge/token` on the bridge
   machine; do not paste it into chats, issues or logs.

Things to know:

- **Trusted LAN only.** The PicoCalc speaks plain HTTP, so the bearer token
  and every message cross the network unencrypted, readable by anyone on that
  network. Run this only on a LAN you control, and never expose port 8765 to
  the Internet, directly or through a port forward.
- The PicoCalc keeps the token in plain text in `/coyote/ai.ini`. Anyone who
  can hold the SD card can read it. If the card or the device is lost, write a
  new token to `/etc/coyote-muse-bridge/token` and restart the bridge.
- The firmware stops waiting after its **Request ms** setting (120 s by
  default) however alive the stream is. Raise it under **Connection settings**
  (up to `600000`) if Muse's longer replies are cut off; a cut-off turn still
  runs to its end on Muse and holds the bridge busy (`409`) until it settles.
- The firmware's **System prompt**, **Temperature** and **Max predict**
  settings have no effect on Muse.

## Test

```sh
uv run --isolated --extra test pytest -q
```

## Limitations

- Exactly one turn is active. A disconnected HTTP client's turn remains
  quarantined until it settles or times out so late global subscription events
  cannot attach to the next client.
- Client disconnect does **not** cancel work on Muse; the official API used here
  exposes no server-cancellation contract.
- The acknowledgement from Muse is awaited before any response byte is
  written, so a slow acknowledgement is not covered by `/api/chat` heartbeats.
- Replies are text-only. Completion uses a quiet-settle window because a Muse
  turn may emit multiple assistant messages and busy/idle events.
- Pairing and a real end-to-end Muse reply require a Muse account/device and
  network access; the automated suite uses the official Noise test harness.
