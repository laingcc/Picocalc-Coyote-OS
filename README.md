# Coyote OS for PicoCalc

Calculator firmware for the [PicoCalc](https://www.clockworkpi.com/picocalc) kit, targeting the
Raspberry Pi Pico 2 W (RP2350). It is a graphing calculator and text editor with a
keyboard-driven AI chat mode that streams replies from an [Ollama](https://ollama.com) host on
the local network, or directly from [DeepSeek](https://api-docs.deepseek.com) over verified TLS.

![calculator mode](/assets/scr_000.bmp)

![graphing mode](/assets/scr_001.bmp)

## Features

- **Calculator** – four tabs (F1–F4), expressions evaluated with
  [tinyexpr](https://github.com/codeplea/tinyexpr), ten-entry history per tab.
- **Graphing** – tab 4 plots `f(x)`; F6 opens the graph menu to keep up to four extra functions
  on screen or clear them.
- **Text editor** – F1 opens a file, F5 saves, F6 starts a new one. Files live in `/coyote` on the
  SD card.
- **AI chat** – type a message, Enter sends it, the reply streams in as it is generated. The
  transcript keeps the last four turns.
- **Wi-Fi manager** – joins a WPA2-AES (or open) network in the background and retries with
  backoff after a drop.
- **Network scan** – pick the SSID from a list of up to 16 scanned networks instead of typing it.
- **Clock** – the time is fetched from `pool.ntp.org` (SNTP) when Wi-Fi comes online and again
  every hour, and shown as `HH:MM` at the right of the chat status bar. It is UTC until you set
  **UTC offset min** under F5, **Connection settings** (minutes ahead of UTC, `-720` to `840`; for example
  `-300` for UTC-5 or `330` for UTC+5:30). It is saved to
  `/coyote/clock.txt` so it survives a reboot; until the next sync a restored time is shown as
  `HH:MM?`, because it is behind by however long the unit was off.
- **SD persistence** – chat and Wi-Fi settings are saved to `/coyote/ai.ini` and reloaded at
  boot. A card that fails to mount is left alone, never formatted.
- **SD card over USB** – Home, **USB disk** hands the card to a computer, which mounts it as a
  drive named *PicoCalc SD card*: files can be dropped on it and `/coyote/ai.ini` edited without
  taking the card out. Eject it on the computer first, then press Esc. See
  [SD card over USB](#sd-card-over-usb).

### Keys

| Key | Calculator | Text editor | Chat |
| --- | --- | --- | --- |
| Home (Shift+Tab) | Mode menu: Text / Calculator / Chat / Connection settings / USB disk | same | same |
| F1–F4 | Switch tab | F1: open file | – |
| F5 | Settings: beeps, reboot to bootloader | Save as | Chat menu |
| F6 | Graph menu (tab 4) | New file | – |
| Enter | Evaluate / plot | New line | Send |
| Esc | – | – | Cancel the reply, or clear the composer |
| Up / Down | – | – | Scroll the transcript |

In every menu, Up/Down move, Enter selects, and Esc or Backspace goes back. In a text prompt,
Enter confirms and Esc cancels.

## Host tests

The AI stack has a host-native test suite in `tests/`. It is plain C11 built with the host
compiler (`-Wall -Wextra -Werror`, no compiler extensions) and needs neither the Pico SDK nor
the submodules:

```sh
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Run `./build-tests/coyote_ai_tests` directly to see the count; it currently prints
`12336 checks, 0 failures`.

## Building the firmware

You need CMake, the `arm-none-eabi` GCC toolchain, and a checkout of the
[Pico SDK](https://github.com/raspberrypi/pico-sdk) with its submodules. The SDK is not vendored
in this repository; point the build at it with `PICO_SDK_PATH` (on the command line as below, or
as an environment variable). CI builds against SDK 2.2.0.

```sh
git clone --recursive https://github.com/laingcc/Picocalc-Coyote-OS.git
cd Picocalc-Coyote-OS

cmake -S . -B build -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=/path/to/pico-sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

The result is `build/coyote.uf2`. `tinyexpr` and `pico-vfs` are git submodules; in an existing
clone run `git submodule update --init --recursive` first.

`-DPICO_BOARD=pico2_w` matters: a board without the CYW43 radio still builds, but CMake warns
and the firmware is compiled without Wi-Fi, so chat cannot connect.

## Flashing

1. Hold BOOTSEL on the Pico 2 W and plug it into the computer over its own USB port, then
   release the button. It mounts as a drive named `RP2350`.
2. Copy the firmware onto it: `cp build/coyote.uf2 /media/$USER/RP2350/`. The board reboots
   into Coyote OS.

Once Coyote OS is running, F5 → Reboot in calculator mode returns to the bootloader without
the button.

Debug output goes to UART0 at 115200 baud (USB stdio is disabled), which the PicoCalc exposes on
its USB Type-C port.

## SD card over USB

Home, **USB disk** hands the card to a computer, which mounts it as an ordinary FAT drive named
*PicoCalc SD card*: the same card the firmware keeps `/coyote` on, without taking it out of the
case.

1. Plug the **Pico's own USB port** into the computer. The PicoCalc's USB Type-C is the charging
   port and the STM32's serial console, so it cannot carry the disk.
2. On the PicoCalc, press Home and choose **USB disk**. The screen says what the computer is
   seeing; it stays up for as long as the mode runs.
3. On the computer, do what you came for - drop a file on the card, edit `/coyote/ai.ini`, take a
   copy of what is on it.
4. **Eject the drive on the computer**, then press Esc on the PicoCalc.

While the mode is up the card belongs to the computer, so nothing else can use it: the chat, the
editor and the settings all wait, and a reply that is still streaming has to finish or be
cancelled before the card can be handed over. Coming back, the card is brought up again from
scratch - it may have been swapped while it was gone - and the settings are read from it again if
the computer wrote anything, so an edited `ai.ini` takes effect without a reboot.

**Power.** The Pico's USB port is wired into the PicoCalc's power path: plugging it into a
computer feeds the cells past the mainboard's charging circuit, which is a known quirk of the
PicoCalc and happens whether or not the USB disk mode is running. If that matters for how you
use it, power the Pico through a cable with the 5 V wire cut, or take the cells out.

## First-run chat setup

1. Press Home and choose **Chat**.
2. Press F5 and choose **Connection settings**.
3. Select **SSID**, then **Scan networks** to pick one from the list (you are asked for its
   password next) or **Type manually**. An empty password joins an open network.
4. Set **Host** to the machine running Ollama and check **Port** (default `11434`). Set
   **Model** to a model that host has pulled.
5. Choose **Apply & connect**. The settings are applied and written to `/coyote/ai.ini`; the
   status bar shows `joining`, then `online`.
6. Type a message and press Enter.

The password is shown while you type it and appears as `********` in the menus afterwards. The
remaining entries (connect, request and idle timeouts, reply length limit) have working
defaults. Without an SD card the settings still apply, but only until power-off.

Ollama listens on `127.0.0.1` by default; start it with `OLLAMA_HOST=0.0.0.0` so the PicoCalc
can reach it.

## API key and token

There is one credential setting. **API key** holds whatever the chosen provider expects: a cloud
provider's key, or the Muse bridge's token. It is optional and empty by default, is masked while
you type it, appears as `********` in the menus afterwards, and is stored in plain text in
`/coyote/ai.ini` as `bearer_token=…` (you can put it there from a computer instead of typing it).
Any value must look like a bearer token: letters, digits and `-._~+/`, with `=` padding last.

A key is remembered per provider. Choosing another provider puts the key being used away under the
provider it was typed for and brings back that provider's own key, so switching between, say,
DeepSeek and the Muse bridge does not mean typing either one again. Remembered keys are written
under their provider's name, e.g. `key_muse=…`; clearing the key clears the remembered one too.

The key is sent as an `Authorization: Bearer` header. For a cloud provider it never leaves TLS. For
`ollama` and `muse` - a host on your own LAN, which speaks plain HTTP - it travels in clear, so use
those only on a network you trust.

## DeepSeek

The firmware can talk to DeepSeek itself: no bridge or other machine is involved.

1. Join Wi-Fi as above.
2. Press F5 in Chat, select **Provider** and choose **deepseek**. Host, port and model change
   to `api.deepseek.com`, `443` and `deepseek-chat`.
3. Press F5, **Connection settings**, **API key**, and type your DeepSeek key. It is echoed as
   asterisks and shown as `********` afterwards. Choose **Apply & connect**.
4. Wait for the clock in the status bar to show a time without a `?`, then send a message.

**Model** offers `deepseek-chat` and `deepseek-reasoner`; no other value is accepted. The
reasoner's thinking is not shown, only its answer, and it counts against **Max predict**, so
raise that if replies come back empty.

What to know about it:

- **The API key is stored in plain text** as `bearer_token` in `/coyote/ai.ini`. Anyone who can
  take the SD card can read it. Use a key you can revoke, with a spending limit.
- The connection is TLS 1.2 to `api.deepseek.com:443` only. The server's certificate chain and
  host name are always verified, against the Amazon Trust Services roots built into the
  firmware (`tls/tls_trust.c`). There is no setting to relax this and no plain-HTTP fallback:
  if TLS cannot be set up or verified the request fails with `secure connection failed`.
- Certificates can only be checked against the real date, so a message is refused with
  `clock not synced: wait` until the SNTP client has answered since power-on. A time restored
  from the SD card (`HH:MM?`) is not enough. This needs UDP port 123 to `pool.ntp.org`.
- While the provider is `deepseek` the host and port cannot be changed, so the key is never sent
  anywhere else. Switching back to `ollama` clears the host and model.
- If DeepSeek moves to a certificate authority other than Amazon's, requests fail until the
  roots are updated and the firmware rebuilt.
- `401` shows as `API key rejected`, `402` as `no DeepSeek balance`, `429` as `rate limited`.
- The TLS handshake does its public-key arithmetic inside the network poll, so the keyboard
  can be unresponsive for a moment while a request connects.

In `ai.ini` the minimum is:

```
provider=deepseek
bearer_token=<your key>
```

## Architecture

The firmware is a single super-loop: `main.c` reads a key, dispatches it to the active mode
(`MODE_CALCULATOR`, `MODE_TEXT`, `MODE_CHAT`), and polls the network. There are no threads and
no RTOS; lwIP runs in `NO_SYS` mode with raw callbacks, driven from that same loop. The AI stack
is C11 with fixed-size static storage and makes no heap allocations; mbedTLS allocates from
lwIP's fixed pool, not the C heap. The SD card is mounted
read/write as FAT at `/`, and everything Coyote OS writes goes under `/coyote`.

Only `ai/app_services.c`, `time/time_service.c` and `tls/tls_clock.c` include Pico SDK, lwIP or mbedTLS headers. Every other module in `ai/`
and `time/` reaches the network through function-pointer adapters, which is what lets the host tests drive
the transport with fake DNS, TCP and clocks.

- `UI/chat_mode.c` – the chat screen: transcript, composer, status bar, menus.
- `ai/chat_model` – fixed-capacity transcript and composer.
- `ai/chat_layout` – word wrap for the transcript.
- `ai/chat_request` – turns the transcript into a provider request and measures its size.
- `ai/chat_settings` – the editable settings: labels, validation, display formatting.
- `ai/ai_config` – configuration struct, defaults, `ai.ini` parser and serialiser.
- `ai/config_store` – loads and saves `ai.ini` through a temporary file and a backup.
- `ai/provider.h`, `ai/ollama_provider` – provider event interface and the Ollama request
  builder / reply decoder.
- `ai/deepseek_provider` – the DeepSeek (OpenAI-compatible) request builder, its server-sent
  event reply decoder, and the checks made before a request may start.
- `ai/json_stream`, `ai/http_parser` – incremental, allocation-free JSON and HTTP parsers.
- `ai/http_stream` – the HTTP request state machine: DNS, connect, send, receive, timeouts,
  cancel. A TLS request is connected through the TLS adapter or not at all.
- `ai/wifi_manager` – Wi-Fi station state machine with backoff.
- `ai/wifi_scan` – collects and de-duplicates scan results.
- `ai/app_services` – binds the above to CYW43 and lwIP and is polled from the main loop.
- `time/time_sync` – the clock: carries the synced time forward, decides when SNTP runs and when
  the time is saved, formats `HH:MM` in local time (UTC plus the configured offset).
- `time/time_service` – binds `time_sync` to the lwIP SNTP client and `/coyote/clock.txt`.
- `tls/` – the mbedTLS build configuration (`mbedtls_config.h`), the trusted root certificates
  (`tls_trust.c`) and the clock mbedTLS checks certificate dates against (`tls_clock.c`), which
  only reports a time once SNTP has set it.

The original plan is in [docs/plans/2026-10-07-pico2w-ai-chat.md](docs/plans/2026-10-07-pico2w-ai-chat.md).

## Muse bridge

`tools/muse_bridge/` is an optional Python service that runs on a Linux machine, separate from
the firmware. It wraps the Meta Muse Gadget SDK and exposes it as a streaming HTTP endpoint that
requires a bearer token and validates and size-limits every request; it binds to `127.0.0.1`
unless told otherwise. It also answers Ollama's `POST /api/chat`, so the firmware reaches Muse by
choosing the **muse** provider: **Host** the bridge machine, **Port** `8765` and **Model** `muse`
come with it, and **API key** is the bridge token (see
[tools/muse_bridge/README.md](tools/muse_bridge/README.md#picocalc-setup)). The avatar Muse sends
for this device can be compiled into the firmware.

## Known limitations

- Wi-Fi is WPA2-AES or open only. WPA3 and WPA/TKIP networks are not supported.
- `.local` host names are resolved with one-shot mDNS queries, which is best-effort. An IP
  address is the reliable value for Host.
- Requests to an Ollama host, the Muse bridge included, are plaintext HTTP; the one key setting
  goes with them. Use it only on a LAN you trust.
- Ollama, DeepSeek, OpenAI, Mistral, Gemini and Claude are the providers in the firmware; Muse is
  reached through the bridge, which speaks Ollama's protocol.
- The USB disk mode is an all-or-nothing stop: while it runs, nothing else on the device can use
  the card, and the chat waits until it ends.
- The clock is UTC only and the NTP server is fixed. With no battery-backed RTC, a time restored
  after a reboot is stale until Wi-Fi comes back.
- The Wi-Fi password and every key the device has been given (the API key - or bridge token - of
  each provider, under `key_<provider>=`) are stored unencrypted in `/coyote/ai.ini` on the SD
  card.

## License

See [LICENSE](LICENSE).
