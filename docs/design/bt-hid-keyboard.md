# Bluetooth HID keyboard mode

The PicoCalc can be used as a wireless keyboard for a phone, tablet or computer: its own
keyboard is forwarded over Bluetooth Low Energy as a standard HID keyboard, so the host needs
no software and no driver. This document pins the behaviour before the code is written.

## What is being built, and what is not

- **Built:** a BLE HID-over-GATT (HOGP) *keyboard peripheral*. The PicoCalc advertises, a host
  pairs with it, and keystrokes typed on the PicoCalc appear on the host.
- **Not built:** the PicoCalc as a HID *host* (accepting an external Bluetooth keyboard), a
  mouse, consumer-control keys, or Bluetooth Classic HID. Classic HID would work for desktop
  PCs but not for iOS, which is the main reason to pick BLE here. If Classic is ever added it
  is a second transport behind the same key-mapping module.

## Mode

A new application mode, `MODE_BT_KEYBOARD`, reachable from the mode menu as **` Bluetooth `**,
placed after **` Chat `** and before **` Connection settings `**.

While the mode is active:

- The screen owns itself: it shows the adapter state and nothing else needs the panel.
- Keys typed on the PicoCalc are **forwarded to the host**, not interpreted locally.
- **Esc is local**: it leaves the mode and returns to the previous mode. It is never sent to the
  host. This is the only escape hatch, so it must work in every state, including while the link
  is down.
- Wi-Fi may stay up; the radio is shared, and the chat/service loop keeps running.

States shown on screen, one line each:

| State | Screen |
|---|---|
| radio not started | ` Starting Bluetooth ` |
| advertising | ` Pair from your device: PicoCalc Keyboard ` |
| connected, not bonded | ` Connected - finishing setup ` |
| bonded and connected | ` Connected to <peer address> ` |
| failed | ` Bluetooth failed ` |

There is no scrolling, no animation, and no redraw per poll: draw on a state change only, the
way the USB-disk screen does.

## Key mapping

`i2ckbd` reports **presses only**: a press arrives as the character in the high byte with `0x01`
in the low byte, and a release produces no event at all. Ctrl is folded into the character by
the driver (`ctrl+a` arrives as `0x01`), and Shift is implicit in the character (`A` vs `a`,
`!` vs `1`). There is no separate modifier event to latch.

That means the keyboard is driven as a **tap**: one HID report with the key and its modifiers,
followed by an all-zero report. Holding a key does not repeat, and there is no key-up event to
report. This is a limitation of the hardware path, not a choice; do not invent a repeat timer.

Mapping, from `keyboard_definition.h`:

| Input | HID usage | Modifier |
|---|---|---|
| `0x20`–`0x7E` printable ASCII | the usage for that character on a US layout | `0x02` (Left Shift) when the character is uppercase or a shifted symbol |
| `0x01`–`0x1A` (ctrl+letter folded by the driver) | `0x04` + (value − 1), i.e. `0x01` → `a` | `0x01` (Left Ctrl) |
| `KEY_BACKSPACE` `0x08` | `0x2A` | none |
| `KEY_TAB` `0x09` | `0x2B` | none |
| `KEY_ENTER` `0x0A` | `0x28` | none |
| `KEY_ESC` `0x1B` | local: leaves the mode | — |
| `KEY_DEL` `0xD4` | `0x4C` (Delete forward) | none |
| `KEY_INSERT` `0xD1` | `0x49` | none |
| `KEY_HOME` `0xD2` | `0x4A` | none |
| `KEY_UP`/`DOWN`/`LEFT`/`RIGHT` `0xB5`/`0xB6`/`0xB4`/`0xB7` | `0x52`/`0x51`/`0x50`/`0x4F` | none |
| `KEY_F1`–`KEY_F6` `0x81`–`0x86` | `0x3A`–`0x3F` | none |
| `KEY_CAPS_LOCK` `0xC1` | `0x39` | none |
| `KEY_MOD_*` `0xA1`–`0xA5`, `KEY_BREAK` `0xD0`, `KEY_NONE` `0` | ignored | — |

Anything unmapped is **ignored and reported as unmapped by the pure mapping function**, so a
host test can enumerate the gaps rather than the firmware silently swallowing keys.

The mapping lives in `bt/bt_hid_keycode.{c,h}`: a pure function
`bt_hid_keycode_map(int key, bt_hid_report_t *out)` returning `BT_HID_MAPPED` or
`BT_HID_UNMAPPED`, with **no Pico SDK, BTstack or stdio dependency**, so it compiles into the
host test target exactly as `ai/search_index.c` does.

## HID report

Boot-protocol keyboard report, 8 bytes:

```
byte 0  modifiers   bit0 LeftCtrl, bit1 LeftShift, bit2 LeftAlt, bit3 LeftGUI,
                    bit4 RightCtrl, bit5 RightShift, bit6 RightAlt, bit7 RightGUI
byte 1  reserved    0
byte 2-7 keycodes   up to six, zero-filled
```

`bt_hid_report_t` holds the modifier byte and one keycode; the transport sends it, waits about
5 ms so the host's HID stack samples it, then sends the all-zero report.

## Transport

- BTstack, **BLE only** (`ENABLE_BLE`, no Classic), HID device role via the SDK's
  `ble/gatt-service/hids_device.h`.
- GATT database from a `.gatt` file compiled at build time with
  `pico_btstack_make_gatt_header`, the way the SDK's `hog_keyboard_demo` does it. Do not
  hand-roll an `att_db`.
- Report map: the standard boot keyboard descriptor (modifiers, reserved, 6 keys), report id 1,
  input report, with the HID information characteristic advertising boot-device support.
- Device name **`PicoCalc Keyboard`**, appearance `0x03C1` (keyboard).
- Pairing: **Just Works**, no passkey, no MITM. State this in the README: anyone in radio range
  can pair while the mode is advertising. It is a keyboard, on a handheld, in a pocket.
- Bonding: keys persist across power cycles through `pico_btstack_flash_bank`, so a paired host
  reconnects without pairing again. Advertising resumes after a disconnect.
- `bt_hid_init()` must be idempotent, and `bt_hid_poll()` is called once per main-loop pass
  while the mode is active. It calls `cyw43_arch_poll()` itself, because
  `app_services`' `platform_poll()` only polls the radio while Wi-Fi is powered.

## Radio ownership

- The radio is initialised once. `ai/app_services.c` already guards this with
  `radio_initialised`; expose that guard as an idempotent
  `int app_services_radio_ensure(void)` (0 when the radio is up) and have `bt_hid_init()` call
  it, rather than calling `cyw43_arch_init()` a second time.
- Bluetooth and Wi-Fi share the CYW43439. Nothing else in the firmware may assume it owns the
  radio during this mode.
- No filesystem, UI or network calls from BTstack callbacks. Callbacks set state and return;
  the main loop draws and forwards.

## Build gating

- New sources are firmware-only: `bt/bt_hid.c`, `bt/bt_hid_pico.c` (transport) and
  `bt/bt_hid_keycode.c` (pure mapping). Only `bt_hid_keycode.c` goes into `tests/CMakeLists.txt`.
- Link `pico_btstack_ble`, `pico_btstack_cyw43`, `pico_btstack_flash_bank`, and
  `pico_btstack_run_loop_async_context`.
- Gate on the radio being present, not on a new option: the mode exists only when
  `PICO_CYW43_SUPPORTED` (the `pico2_w` build). `pico2`, `pico_w` with `-DCOYOTE_NO_AI=ON`, and
  the no-AI build must all still compile, with the mode row absent or stubbed the way
  `MODE_ROW_CHAT`/`MODE_ROW_SETTINGS` already are.
- **Measure `.bss` against the `554a047` baseline** and report both numbers. The full build is
  ~256 KB of 520 KB; BTstack plus the GATT database is expected to cost tens of KB, and if the
  build will not link, the honest outcome is to report the number and stop, not to shave
  unrelated buffers.

## Acceptance criteria

1. `cmake -S tests -B build-tests && cmake --build build-tests && ./build-tests/coyote_ai_tests`
   passes, including new tests for the mapping table: every printable ASCII character, every
   control code `0x01`–`0x1A`, the named keys above, and that unmapped values report unmapped.
2. A host test proves the report is well formed: modifiers in byte 0, keycode in byte 2, bytes
   3–7 zero, reserved byte zero.
3. `-DPICO_BOARD=pico2_w` cross-build succeeds with `-Werror` and no new warnings; `pico2` and
   `-DCOYOTE_NO_AI=ON` cross-builds still succeed.
4. The mode is reachable from the mode menu, Esc leaves it, and the menu still fits its frame
   (the item list and the menu height are separate constants).
5. `arm-none-eabi-size build/coyote.elf` is reported alongside a `554a047` baseline build.
6. Nothing is claimed about real pairing or typing until it has been tried on a board: the
   deliverable is "compiles, maps correctly, is wired into the mode menu", and the PR says
   plainly which parts were not hardware-tested.
