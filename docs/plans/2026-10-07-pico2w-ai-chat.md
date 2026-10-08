# Pico 2 W AI Chat Implementation Plan

> **For Hermes:** Use subagent-driven-development to implement this plan task-by-task. Verify every agent edit independently.

**Goal:** Add a keyboard-driven AI chat mode to Coyote OS on Pico 2 W, with direct streaming Ollama support and an authenticated Linux bridge for Muse.

**Architecture:** Keep the current calculator and text modes intact. Add a fixed-memory chat model, provider abstraction, streaming HTTP client over CYW43/lwIP, and SD-backed AI configuration. Ollama is the first direct provider. Muse is reached through a separate Python bridge that embeds the official Muse Gadget Linux SDK, subscribes to Muse reply events, and exposes bounded authenticated NDJSON over the LAN.

**Tech stack:** Raspberry Pi Pico SDK C11, CYW43/lwIP raw TCP, FAT SD storage, host-native C tests, Python 3.11+/asyncio/aiohttp, official `facebookincubator/muse-gadget-sdk` Linux package pinned by commit.

**Hard constraints:**
- Build with `-DPICO_BOARD=pico2_w`.
- Preserve calculator, graphing, sound, text editing, and SD file behavior.
- Never format an SD card automatically after a mount failure.
- No heap allocation in the firmware AI request/response path.
- Never print Wi-Fi passwords or bridge bearer tokens.
- Initial firmware HTTP transport is LAN-only plaintext HTTP; document this.
- Existing tests may not be weakened. If a test conflicts with this plan, stop and report it.
- Keep new static firmware storage under 40 KiB and verify linker/size output.

---

### Task 1: Establish Pico 2 W baseline and safe branch

**Files:** Existing tree only.

1. Check out `feature/pico2w-ai-chat` from upstream commit `e86cf36d26e90e4891615991c1689b76fb2f90b1`.
2. Configure with `cmake -S . -B build -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=$HOME/pico/pico-sdk -DCMAKE_BUILD_TYPE=Release`.
3. Run `cmake --build build -j2` and require `coyote.uf2`.
4. Record baseline size and clean status.

### Task 2: Add host-test harness and pure parsers

**Objective:** Make protocol logic testable without Pico hardware.

**Files:**
- Create: `tests/CMakeLists.txt`
- Create: `tests/test_main.c`
- Create: `tests/test_json_stream.c`
- Create: `tests/test_http_parser.c`
- Create: `ai/json_stream.h`, `ai/json_stream.c`
- Create: `ai/http_parser.h`, `ai/http_parser.c`
- Modify: root `CMakeLists.txt`

1. Write tests for JSON escaping/unescaping and Ollama NDJSON records split at every byte boundary.
2. Write tests for HTTP status/headers plus content-length, chunked, and close-delimited bodies split at every byte boundary.
3. Run native tests and verify failure before implementation.
4. Implement bounded allocation-free parsers.
5. Run native tests and require all pass.
6. Commit the parser/test slice.

### Task 3: Add fixed-capacity chat model and provider boundary

**Files:**
- Create: `ai/provider.h`
- Create: `ai/ollama_provider.h`, `ai/ollama_provider.c`
- Create: `ai/chat_model.h`, `ai/chat_model.c`
- Create: `tests/test_ollama_provider.c`
- Create: `tests/test_chat_model.c`

1. Test exact request length versus emitted bytes, JSON escaping, CONTENT/DONE/ERROR events, four-turn eviction, in-flight protection, cancellation, oversized prompts/responses, and partial response markers.
2. Implement offset-readable provider requests so TCP backpressure never requires a complete heap buffer.
3. Implement fixed storage: eight messages, 4096 bytes each, 512-byte composer, four complete retained turns.
4. Require host tests to pass and commit.

### Task 4: Add non-destructive AI configuration

**Files:**
- Create: `ai/ai_config.h`, `ai/ai_config.c`
- Create: `tests/test_ai_config.c`
- Modify: `config.h`, `main.c`

1. Test defaults, valid round-trip, unknown/duplicate keys, overlong values, bad ports/timeouts, failed write/rename, and preservation of the old file.
2. Store `/coyote/ai.ini` with `version`, SSID, password, provider, host, port, model, bridge token, and timeouts.
3. Save via temporary file plus rename and never log secrets.
4. Change `fs_init()` to report mount failure rather than formatting the card.
5. Require host tests and firmware build to pass; commit.

### Task 5: Add asynchronous Wi-Fi and streaming HTTP transport

**Files:**
- Create: `ai/wifi_manager.h`, `ai/wifi_manager.c`
- Create: `ai/http_stream.h`, `ai/http_stream.c`
- Create: `ai/app_services.h`, `ai/app_services.c`
- Create: `tests/test_http_stream.c`
- Modify: `CMakeLists.txt`, `main.c`, `UI/ui.h`, `UI/ui.c`

1. Test transport state transitions with fake DNS/TCP/clock adapters: DNS failure, refusal, backpressure, header/chunk fragmentation, idle/absolute timeout, remote close, cancellation races, and Wi-Fi loss.
2. Link `pico_cyw43_arch_lwip_poll` and lwIP networking for Pico 2 W.
3. Implement CYW43 station states OFF/NEEDS_CONFIG/CONNECTING/ONLINE/BACKOFF/ERROR.
4. Implement DNS plus raw TCP HTTP states with 1024-byte headers and 2048-byte NDJSON line cap.
5. Add a UI idle hook so modal menus continue polling networking.
6. Require native tests and cross-build; commit.

### Task 6: Add chat UI and settings

**Files:**
- Create: `ai/chat_mode.h`, `ai/chat_mode.c`
- Modify: `UI/ui.h`, `UI/ui.c`, `main.c`

1. Add `MODE_CHAT` to the Home mode menu.
2. Route input without changing calculator/text dispatch.
3. Render 40-column wrapped transcript, composer, status, and errors with dirty/coalesced redraws.
4. Bind Enter=send, Backspace=edit, Up/Down=scroll, Esc/Break=cancel, F5=settings, F6=clear, Home=cancel then mode menu.
5. Mask password/token prompts and validate before saving.
6. Cross-build, inspect `arm-none-eabi-size`, and commit.

### Task 7: Build the Muse bridge against the official SDK

**Files:**
- Create: `tools/muse_bridge/pyproject.toml`
- Create: `tools/muse_bridge/src/coyote_muse_bridge/{__init__,__main__,session,broker,http}.py`
- Create: `tools/muse_bridge/tests/test_{session,broker,http}.py`
- Create: `tools/muse_bridge/systemd/coyote-muse-bridge.service`
- Create: `tools/muse_bridge/README.md`

1. Pin `facebookincubator/muse-gadget-sdk` commit `86cf33fb4092ba700b4dc33928966d1bcb31556d`, `subdirectory=linux`.
2. Write failing tests around the official fake Noise transport for registration, `POST /chat/subscribe`, fragmented NDJSON, ACK/event races, correlation, duplicates, unrelated traffic, busy state, settle timeout, session loss, and disconnect quarantine.
3. Subclass `LinkSession` only where the public SDK lacks extension hooks; preserve its Noise, token refresh, command handling, and Unix socket behavior.
4. Expose authenticated `POST /v1/chat` and `GET /health`; use constant-time bearer comparison, 16 KiB request cap, one active turn, and immediately flushed compact NDJSON.
5. Default bind to `127.0.0.1`; require explicit configuration for LAN binding. Document plaintext-LAN risk and reverse-proxy option.
6. Run bridge tests and official SDK LinkSession/Service contract tests; commit.

### Task 8: Documentation and CI

**Files:**
- Modify: `README.md`
- Modify: `.github/workflows/build-release.yml`

1. Document Pico 2 W build/flash steps, keyboard controls, `ai.ini`, `wang.local:11434`, IP fallback, transcript bounds, credentials-at-rest warning, and bridge installation.
2. Make CI run host tests then cross-compile for `pico2_w` and retain UF2 plus size/map artifacts.
3. Verify workflow syntax and commit.

### Task 9: Independent review and final verification

1. Run formatting checks, all host C tests, all Python bridge tests, official SDK contract tests, and clean Pico 2 W cross-build.
2. Inspect every changed pre-existing test for deleted/weakened assertions.
3. Grep every new provider/config/transport API for real callers.
4. Review cancellation and lwIP callback lifetimes separately.
5. Confirm `coyote.uf2`, ELF, map, and measured SRAM/flash size.
6. Run an Ollama integration request against `wang.local:11434` from a host-side protocol fixture where possible.
7. State hardware-only gaps honestly: Wi-Fi/mDNS, LCD interaction, and real Muse pairing require the physical PicoCalc/account.
8. Push the branch, open a PR to `laingcc/Picocalc-Coyote-OS:master`, and attach or deliver the verified UF2.
