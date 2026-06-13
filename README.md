# LimitlezzOS

A mesh-native handheld OS for the **LilyGO T-Deck** (ESP32-S3, SX1262 LoRa,
320×240 TFT, BlackBerry QWERTY, trackball) that unifies **Meshtastic** and
**MeshCore** into a single network-tagged inbox, driven entirely by the
trackball.

It is a working **Alpha 0.1**: real LVGL 8.3 firmware (display via LovyanGFX),
a desktop SDL2 simulator sharing the exact same UI code, and a live Meshtastic
radio stack on the SX1262 — flashed and tested on real T-Deck hardware. The UI
follows the design handoff (`docs/design/`) exactly as the master spec
prescribes: flat solid fills, 1px hairlines, a 2px near-white focus ring, baked
font tables, no images, no gradients, no alpha layering.

> ⚠️ **This is an ALPHA TEST (0.2).** It runs on real hardware and the core
> Meshtastic experience is usable, but some features are still unfinished (see
> below). The near-term goal is to make this a great **Meshtastic** OS — on par
> with the official device UI (MUI) — before fully building out **MeshCore**.
> Status reflects hands-on hardware testing as of **2026-06-13**.

## Alpha status

### ✅ Working (hardware-tested)
- **Display, screens & navigation** — renders cleanly, tear-free, screen-to-screen nav.
- **Trackball + QWERTY keyboard** — focus, scroll, typing, back gesture.
- **Touchscreen** — tap items/tabs/buttons/toggles; **3-tap on-screen calibration** (Settings → Calibrate touch) that any T-Deck can run, persisted across reboot.
- **Clock** — status-bar time, NTP sync over Wi-Fi, named timezone picker, automatic DST (US/EU), **12-hour (AM/PM) or 24-hour** format.
- **Meshtastic channel messaging (LongFast)** — send **and** receive on the public channel.
- **Node discovery** — heard nodes list (name, SNR, last-heard).
- **Message history** — persists across leaving a chat and across reboots (SD card).
- **Compose** — long drafts scroll within the input box instead of overflowing.
- **Wi-Fi** — scan, connect, saved password, auto-connect toggle, forget.
- **Battery & charging** — live percentage + charge state; System page telemetry.
- **Keyboard backlight** — Auto / On / Off (I²C).
- **Sleep & power saving** — idle dim/sleep, CPU down-clock.
- **MeshCore self-advert (TX)** — signed Ed25519 advert broadcasts (flood + zero-hop); persistent identity; Advertise buttons only shown when MeshCore is on.
- **Split airtime (TDM)** — both networks on → SX1262 alternates MC↔MT every 500 ms; one on → 100%.
- **Serial console** — USB-CDC command shell for control + diagnostics (`help`, `time`, `tz`, `net`, `rf`, `mc`, `companion`, `touch`, …).

### 🧪 In testing
- **Companion bridge (Meshtastic over USB)** — protocol self-test passes on-device,
  but **not yet connecting to the official app** (needs the Config/ModuleConfig
  messages the app waits for).
- **MeshCore (whole network path)** — adverts transmit + remote nodes decode, but
  **receiving is not working yet**: no Public channel, can't see/send/receive
  MeshCore messages.

### 🛠️ Roadmap — make Meshtastic great (in priority order)
1. ✅ **DM: tap the name at the top of a chat → open that contact's profile.**
2. ✅ **Channel: long-press a message → sender's profile → Message / Add to contacts.**
3. **Meshtastic DMs (send + receive)** — needs PKI (Curve25519 + AES-CCM), as
   modern Meshtastic encrypts DMs per-recipient. *(in progress)*
4. **USB companion → connect to the official Meshtastic app** (add Config msgs).
5. **BLE companion** — let phones connect wirelessly, not just over USB.
6. **Message delivery status** — show Delivered / Failed (via ROUTING_APP acks);
   long-press your own failed message to resend, resetting its status until it's
   determined delivered or failed.

### 🔭 Later
- **MeshCore**: receive + default Public channel, then a MeshCore companion bridge.
- **Security**: optional device **password/PIN**, and **encrypt the data files**
  (messages, identity, keys) when a password is set.
- **Hardening**: Wi-Fi passwords are stored in plaintext on the SD card
  (`/limitlezz/wifi.cfg`) — move to NVS or encrypt (covered by the above when a
  password is set).

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

Current footprint: ~1.2 MB flash (23% of the 5 MB OTA slot), 195 KB static RAM
(61%) — the rest of RAM is PSRAM-backed double framebuffers. Message history,
identity, the node database, and saved Wi-Fi credentials all live on the SD
card (`/sd/limitlezz`); without a card the OS runs RAM-only and seeds the demo
mesh.

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
  value-cycling rows; brightness slider (left/right while focused); functional
  TX power, GPS toggle, and keyboard backlight (Auto/On/Off, driven over I2C);
  **time zone picker** (named zones — EST, PST… — not raw UTC offsets) and a
  **manual clock editor**, with **NTP auto-sync** over Wi-Fi; **System & battery**
  page with a live arc gauge, stat bars, self-updating uptime, and battery-health
  readout derived from resting voltage.
- **Wi-Fi** — scan, join (masked password entry), remembers one network's
  credentials on the SD card, an **auto-connect** toggle (rejoin on boot / on
  reappearance / after a drop, or never), and long-press-to-forget so you can
  change a saved password.
- **MeshCore via TDM** — MeshCore runs as a second RF profile time-division
  multiplexed with Meshtastic on the one SX1262: the radio listens on one
  profile, retunes, listens on the other, round-robin. Both networks on → 50/50
  airtime split (the scheduler the airtime bar visualizes); only one on → that
  profile gets the radio 100% with no switching. Discovery works both ways:
  inbound ADVERTs (signed, unencrypted) are decoded to learn nodes by name +
  role (Chat/Repeater/Room/Sensor), and we **broadcast our own Ed25519-signed
  self-advert** so other MeshCore nodes discover us (the device holds a
  persistent MeshCore keypair; signatures verify under the same rweather/Crypto
  Ed25519 MeshCore uses). MeshCore US profile: 910.525 MHz / 62.5 kHz / SF7 /
  CR4-5 / sync PRIVATE.
- **Real status everywhere** — the status bar clock, battery %, and charge
  state are live; identity, node table, and message history persist across
  reboots; nothing on screen is hard-coded demo data on hardware.
- **Serial console** — a USB-CDC command shell (`help`, `time`, `tz`, `net`,
  `rf`, `nodes`, `send`, `stats`, `wifi`, `sys`, …) for control + diagnostics.
- **Terminal / Files** — mono console with blinking cursor; /sdcard listing.

## Status against the master-spec roadmap

Stage 1 (Meshtastic-only) is the focus, per the spec's hard staging rule
(get Meshtastic rock-solid before adding MeshCore + TDM). Done so far: the full
UI, the messaging data model wired to a real Meshtastic stack, persistent
history/identity/nodes, the SX1262 radio backend (text + NodeInfo on LongFast,
dedup, managed flood), live clock (manual + NTP + named time zones), Wi-Fi with
saved credentials and auto-connect, keyboard backlight, and real
battery/system telemetry.

**Stage 2 (MeshCore) has landed**: MeshCore runs as a second RF profile,
time-division multiplexed with Meshtastic on the one SX1262 (round-robin
listen/retune; both on = 50/50 split, one on = 100%). MeshCore ADVERTs are
decoded so nodes appear by name + role on the amber side of the UI; the airtime
split bar reflects the live schedule.

Still ahead: ACK/routing (ROUTING_APP) and retransmit, position/telemetry
decode, MeshCore encrypted-payload (DM/channel) decode, the Lua app sandbox,
App Store networking, OTA, and the Feedback Manager (LED/buzzer/backlight).

## Flashing & first hardware test

```sh
pio run -e tdeck -t upload        # build + flash over USB-C
pio device monitor -b 115200      # watch the boot diagnostics
```

`setup()` follows the exact init order verified against the LilyGO T-Deck and
Meshtastic sources, and prints a result line for every subsystem so a single
flash tells you the whole story:

```
=== LimitlezzOS boot ===
[ok] peripheral power (GPIO10) HIGH
[ok] shared SPI bus up (SCK40/MISO38/MOSI41)
[ok] ST7789 display init + backlight on
[ok] LVGL double full-frame buffers in PSRAM (tear-free)
[ok] keyboard @0x55
[ok] GT911 touch @0x5D
[ok] trackball + keyboard input
[ok] microSD mounted -> /sd/limitlezz
[ok] message store read/write
[ok] SX1262 radio (RadioLib begin=0)
[ok] node id !a1b2c3d4
=== boot complete ===
```

If a line shows `--`/`FAIL`, the cause is isolated:
- **Everything fails at once** → GPIO10 power rail (shouldn't happen; it's first).
- **SD/radio fail, display works** → a CS pin (the three are driven HIGH before
  `SPI.begin()` to prevent exactly this) or a swapped MISO/MOSI.
- **`begin=` is non-zero** → RadioLib error code (e.g. `-2` chip-not-found,
  `-707` SPI timeout) — points at radio wiring/TCXO, not the OS.
- **Photo-negative colors** → `TFT_INVERSION_ON` (it's set; flag if your panel rev differs).

Key hardware facts (all in `platformio.ini` / `src/main_tdeck.cpp`, sourced to
LilyGO `utilities.h` + Meshtastic `variant.h`): shared SPI SCK40/MOSI41/MISO38;
CS — TFT 12, SD 39, radio 9 (all HIGH at boot); ST7789 landscape rotation 1 with
inversion; backlight LEDC PWM on GPIO 42 (driven by the Brightness setting);
I2C SDA18/SCL8 for keyboard (0x55) + GT911 touch; trackball GPIOs up3/down15/
left1/right2/click0; SX1262 CS9/DIO1-45/BUSY13/RST17, DIO2-as-RF-switch, TCXO 1.8 V.

This is for the standard **T-Deck / T-Deck Plus (ESP32-S3 + SX1262)** — the
**T-Deck Pro** is a different board with a different pin map. `partitions.csv`
is locked per spec §11.

## Sleep & power

The **Sleep after** setting (Settings → Display: 15 s / 30 s / 1 m / 5 m /
Never) idles the screen: after the timeout with no trackball/keyboard/touch
input, the backlight goes dark and the OS returns to the lock screen; any
input wakes it. The Brightness slider drives the same LEDC backlight live.
