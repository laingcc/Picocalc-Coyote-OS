# Coyote OS for PicoCalc

Calculator firmware for the [PicoCalc](https://www.clockworkpi.com/picocalc) kit, targeting the
Raspberry Pi Pico 2 W (RP2350). It is a graphing calculator and text editor with a
keyboard-driven AI chat mode that streams replies from an [Ollama](https://ollama.com) host on
the local network.

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
- **SD persistence** – chat and Wi-Fi settings are saved to `/coyote/ai.ini` and reloaded at
  boot. A card that fails to mount is left alone, never formatted.

### Keys

| Key | Calculator | Text editor | Chat |
| --- | --- | --- | --- |
| Home (Shift+Tab) | Mode menu: Text / Calculator / Chat | same | same |
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
`11917 checks, 0 failures`.

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

## Architecture

The firmware is a single super-loop: `main.c` reads a key, dispatches it to the active mode
(`MODE_CALCULATOR`, `MODE_TEXT`, `MODE_CHAT`), and polls the network. There are no threads and
no RTOS; lwIP runs in `NO_SYS` mode with raw callbacks, driven from that same loop. The AI stack
is C11 with fixed-size static storage and makes no heap allocations. The SD card is mounted
read/write as FAT at `/`, and everything Coyote OS writes goes under `/coyote`.

Only `ai/app_services.c` includes Pico SDK or lwIP headers. Every other module in `ai/`
reaches the network through function-pointer adapters, which is what lets the host tests drive
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
- `ai/json_stream`, `ai/http_parser` – incremental, allocation-free JSON and HTTP parsers.
- `ai/http_stream` – the HTTP request state machine: DNS, connect, send, receive, timeouts,
  cancel.
- `ai/wifi_manager` – Wi-Fi station state machine with backoff.
- `ai/wifi_scan` – collects and de-duplicates scan results.
- `ai/app_services` – binds the above to CYW43 and lwIP and is polled from the main loop.

The original plan is in [docs/plans/2026-10-07-pico2w-ai-chat.md](docs/plans/2026-10-07-pico2w-ai-chat.md).

## Muse bridge

`tools/muse_bridge/` is an optional Python service that runs on a Linux machine, separate from
the firmware. It wraps the Meta Muse Gadget SDK and exposes it as a streaming HTTP endpoint that
requires a bearer token and validates and size-limits every request; it binds to `127.0.0.1`
unless told otherwise. The firmware does not use it yet: Ollama is the only provider compiled
in. See [tools/muse_bridge/README.md](tools/muse_bridge/README.md).

## Known limitations

- Wi-Fi is WPA2-AES or open only. WPA3 and WPA/TKIP networks are not supported.
- `.local` host names are resolved with one-shot mDNS queries, which is best-effort. An IP
  address is the reliable value for Host.
- Requests are plaintext HTTP. Use it only on a LAN you trust.
- Ollama is the only provider in the firmware.
- The Wi-Fi password is stored unencrypted in `/coyote/ai.ini` on the SD card.

## License

See [LICENSE](LICENSE).
