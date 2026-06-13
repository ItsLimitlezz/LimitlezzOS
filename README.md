# LimitlezzOS

A mesh-native handheld OS for the **LilyGO T-Deck** (ESP32-S3, SX1262 LoRa,
320×240 TFT, BlackBerry QWERTY, trackball) that unifies **Meshtastic** and
**MeshCore** into a single network-tagged inbox, driven entirely by the
trackball.

This repo implements the **UI layer** of the design handoff
(`docs/design/`) in C on **LVGL 8.3** — the real target stack for the
T-Deck — exactly as the master spec prescribes: flat solid fills, 1px
hairlines, a 2px near-white focus ring, baked font tables, no images, no
gradients, no alpha layering.

![screens](docs/screens.png)

## Layout

```
platformio.ini         two envs: tdeck (ESP32-S3 firmware) + native (SDL2 sim)
partitions.csv         OTA_0 / OTA_1 / otadata / config / appfs  (locked early, spec §11)
include/lv_conf.h      LVGL 8.3.11, aggressively stripped (spec §4.4)
src/ui/theme*.h        design tokens (colors computed from the design's oklch values)
src/ui/ui.{h,c}        state machine, nav stack, trackball focus engine, shared widgets
src/ui/screens/        the 13 screens (read live data from the mesh service)
src/ui/fonts/          Material Symbols Rounded subsets baked to LVGL C arrays
src/services/mesh.{h,c}  mesh service: node table, thread index, send/receive API
src/services/store.c   persistent message store (append-only logs + thread index)
src/services/mtproto.* Meshtastic wire codec: header, AES-CTR, channel hash, protobuf
src/services/aes_min.h portable AES-128/256-CTR for the simulator
src/services/mesh_seed.c demo mesh (matches the design's sample data)
src/backend_sx1262.cpp real Meshtastic radio over SX1262 (RadioLib) — T-Deck
sim/backend_sim.c      simulated radio (auto-reply) for the desktop sim
src/main_tdeck.cpp     hardware bring-up: ST7789, GT911 touch, keyboard, trackball, SD
sim/main_sim.c         SDL2 simulator + headless screenshots + --selftest
docs/design/           the original design handoff bundle (source of truth)
```

## Architecture: UI ↔ service ↔ radio

The UI never touches the radio. It reads nodes/threads/messages from the
**mesh service** (`src/services/`), which owns the node table and the
persistent message store and talks to a **radio backend** through one small
contract (`lz_backend_*` / `lz_core_on_*` in `mesh.h`). Two backends implement
that contract:

- **`backend_sx1262.cpp`** (T-Deck): real Meshtastic over the SX1262. Speaks
  the actual wire protocol on the default LongFast channel, so LimitlezzOS
  interoperates with stock Meshtastic devices: 16-byte `PacketHeader`,
  AES-128-CTR with the public default PSK, the `xor(name)^xor(psk)` channel
  hash (= `0x08` for LongFast), and the `Data` protobuf. Receives, decrypts,
  decodes text + NodeInfo, dedups, and does managed-flood rebroadcast.
- **`backend_sim.c`** (desktop): a fake radio that delivers sends and produces
  canned replies, so the whole receive pipeline is exercisable without hardware.

All RF/protocol constants are sourced to the Meshtastic firmware (master) and
cited in `mtproto.c`. `program --selftest` round-trips the codec
(header + AES-CTR + protobuf) and asserts the LongFast channel hash.

### First-boot onboarding

On a device with no saved identity, the OS opens a three-step onboarding
(spec §5): your **long name** ("What should people call you?"), a **short
4-character tag** (auto-derived from the long name, editable — this is the
Meshtastic short name), then the **networks** chooser (both on by default,
Continue focused so one click proceeds). The identity persists to the store
and is what the radio broadcasts as the node's Meshtastic `User`. Subsequent
boots skip straight to the lock screen.

### Messages, contacts, and roles

Contacts are people you **purposely add** — not every node ever heard. A node's
**role** decides whether it can be messaged: only `Client` (Meshtastic) and
`Chat` (MeshCore) get a Message button; `Router`, `Repeater`, `Sensor`, and
`Room` are observable but show **Add contact / Trace** instead. The unified
inbox lists conversations newest-first; history is kept when a network is
disabled (spec §6.5) and persists across reboots via the SD-backed store.

## Build & run

**Simulator** (needs SDL2: `brew install sdl2`):

```sh
pio run -e native
.pio/build/native/program                      # interactive, 2x scale window
.pio/build/native/program --shots out/         # dump every screen as BMP
.pio/build/native/program --selftest           # verify the Meshtastic codec
```

Keys: arrows = trackball roll · Page Up = trackball press · Enter = select/send ·
Esc/Backspace = back · 1/2/3 = Messages network filter · mouse = touchscreen ·
typing goes into the conversation composer.

**T-Deck firmware**:

```sh
pio run -e tdeck -t upload                     # flash over USB-C
```

Current footprint: 682 KB flash (13% of the 5 MB OTA slot), 167 KB static RAM.
Message history lives on the SD card (`/sd/limitlezz`); without a card the OS
runs RAM-only and seeds the demo mesh.

## What's implemented (UI portion of spec Stage 1/2)

- **Trackball-first focus engine** — exactly one focused element; row-major
  grid nav (`up=i-cols, down=i+cols`, clamped, no row wrap); focusless
  screens scroll; focused row auto-scrolls into view. Left/right also
  switches tabs on tabbed screens so everything is reachable by trackball.
- **Lock** — clock, network presence, unlock via ball click.
- **Home** — single iOS-style 4×2 grid, solid color tiles, near-white ring.
- **Messages** — unified inbox; Direct/Channels tabs; All/Meshtastic/MeshCore
  filter chips (keys 1/2/3); per-thread network dot + unread badge; threads
  of a disabled network are dimmed, not removed, with a "history kept" note.
- **Conversation** — network-bound thread: tag in the nav bar, encrypted
  caption, bubbles, and a send button that names the outgoing network so
  reply routing is never ambiguous. QWERTY types into the composer; Enter
  appends the bubble and pins the thread to the bottom.
- **Meshtastic / MeshCore managers** — per-network identity cards,
  Nodes/Channels and Contacts/Rooms tabs, SNR color coding, role badges,
  online dots.
- **App Store** — featured card + install flow (GET → "…" → OPEN).
- **Contacts / detail** — unified directory with network dots; detail page
  with Message (jumps into the bound conversation) and spec table.
- **Settings** — airtime scheduler bar that rebalances live when the
  first-class network toggles flip (and drives the Messages dimming);
  value-cycling rows; brightness slider (left/right while focused);
  System & battery page (arc gauge + stat bars).
- **Terminal / Files** — mono console with blinking cursor; /sdcard listing.

## Status against the master-spec roadmap

Stage 1 (Meshtastic-only) is the focus, per the spec's hard staging rule
(get Meshtastic rock-solid before adding MeshCore + TDM). Done so far: the full
UI, the messaging data model wired to a real Meshtastic stack, persistent
history, and the SX1262 radio backend (text + NodeInfo, dedup, managed flood).

Still ahead: on-hardware RF validation against a stock node, ACK/routing
(ROUTING_APP) and retransmit, position/telemetry decode, the Lua app sandbox,
App Store networking, OTA, and the Feedback Manager (LED/buzzer/backlight).
**MeshCore + the TDM airtime arbiter are Stage 2** — the amber side of the UI
and the airtime split bar are in place, but MeshCore DMs are intentionally
read-only until the second radio stack lands on the proven Meshtastic base.

## Hardware notes

`src/main_tdeck.cpp` powers the peripheral rail (GPIO 10) before init,
drives the ST7789 via TFT_eSPI (pins in `platformio.ini`), polls the I2C
keyboard (0x55) and counts trackball pulses with interrupts. Pin map is per
LilyGO's published T-Deck reference; verify against your board revision on
first flash. `partitions.csv` is locked per spec §11 — change it only
before real deployments exist.
