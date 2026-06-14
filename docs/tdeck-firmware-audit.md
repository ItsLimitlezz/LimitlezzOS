# T-Deck Firmware Audit

Audit date: 2026-06-13

Repository baseline: `ItsLimitlezz/LimitlezzOS` `main` at `58b5853` (`Alpha 0.41 (first public release): stability + iPhone-style UI`).

## Scope

This audit reviews the current T-Deck firmware from the repository contents, local PlatformIO builds, and the design/README claims. It does not include a live flash or RF hardware test because no T-Deck hardware was attached during this pass.

Primary source files reviewed:

- `platformio.ini`, `partitions.csv`
- `src/main_tdeck.cpp`, `src/backend_sx1262.cpp`, `src/wifi_tdeck.cpp`
- `src/services/mesh.h`, `src/services/mesh_core.c`, `src/services/store.c`
- `src/services/mtproto.*`, `src/services/mcproto.*`, `src/mtpki.cpp`
- `src/mt_companion.cpp`, `src/serial_cli.cpp`
- `src/ui/**`, `sim/**`
- `README.md`, `docs/design/master_spec_v1.0.txt`

## Validation Performed

| Check | Result | Notes |
| --- | --- | --- |
| `pio run -e tdeck` | Passed on Windows | Firmware builds cleanly after the settings-persistence pass. Current size report: RAM 245,744 bytes / 327,680 bytes (75.0%); flash 1,277,609 bytes / 5,242,880 bytes (24.4%). |
| `pio run -e tdeck -t upload --upload-port COM8` | Flash verified on COM8 | COM8 is an ESP32-S3 USB Serial/JTAG device. The ROM loader verifies each flashed image. This board can remain in `ESP-ROM` download mode after upload, so post-flash CLI checks may require a physical RESET/power-cycle first. |
| COM8 serial smoke | Passed after power-cycle | Commands validated: `id`, `sys`, `net`, `rf`, `stats`, `wifi`, and `companion test`. The board booted LimitlezzOS, mounted `/sd/limitlezz`, initialized SX1262 with `RadioLib begin=0`, and companion loopback reported `PASS`. |
| `pio run -e native` | Local-only validation | Host SDL2 prerequisites were satisfied outside the committed change. Native simulator selftest and screenshots passed locally, but repo-level SDL2 setup/CI remains Phase 0 work. |
| CI/workflows | Not present | No `.github` workflow directory exists. |

## Executive Summary

LimitlezzOS is a strong alpha firmware base rather than a complete OS. The Meshtastic path is the most real: there is T-Deck hardware bring-up, a real SX1262 backend, Meshtastic LongFast framing, NodeInfo parsing, public channel messaging, PKI direct-message crypto, routing ACK handling, SD-backed message/node persistence, Wi-Fi, USB companion mode, and a polished LVGL UI shell.

The incomplete areas are also clear. MeshCore has important groundwork, including a second RF profile, TDM scheduling, ADVERT parsing, and signed self-adverts, but it is globally gated off from the product UI (`LZ_MESHCORE_ENABLED 0`) and does not yet provide MeshCore public channels, DMs, group messaging, encrypted payload handling, or a MeshCore companion flow. The App Store is currently a UI prototype with fake install state; the Home launcher disables it. Lua apps, app catalog download, OTA, hardware feedback manager, emergency beacon, BLE companion, full Developer Mode diagnostics, and encrypted local storage are not implemented yet.

The roadmap should therefore focus on preserving the simple end-user product while turning the existing alpha into a reliable firmware: first harden the build/test/release loop, then finish Meshtastic polish, then bring MeshCore online end-to-end, then build the app platform/store and OTA/update lifecycle.

## Product Direction From Maintainer Notes

The roadmap should preserve these constraints:

- The OS is for ordinary users who want to "just play with mesh", not radio enthusiasts.
- LongFast-only Meshtastic defaults are intentional; avoid exposing region/preset complexity in first-run or everyday UI.
- Meshtastic and MeshCore should coexist automatically through the radio scheduler.
- Heavy or novelty features, especially maps, should be optional App Store installs rather than built into the base OS.
- Terminal and power-user diagnostics should move behind Developer Mode.
- The app system is a core product goal, not a decorative screen.

## Current Architecture

### Hardware Target

`src/main_tdeck.cpp` brings up the LilyGO T-Deck target:

- peripheral power rail, shared SPI bus, ST7789 display through LovyanGFX
- LVGL display buffers with PSRAM fallback
- GT911 touch and calibration persistence
- I2C keyboard and keyboard backlight control
- trackball input
- SD card mount and persistence probe
- battery/system telemetry
- Wi-Fi initialization and NTP sync
- radio/service/UI initialization

### Mesh Service

`src/services/mesh_core.c` owns:

- node table
- thread index and newest-first ordering
- opened conversation tail
- SD-backed persistence through `store.c`
- inbound events from radio backends
- outgoing text sends
- delivery status state for open-tail messages

The UI reads service state rather than talking directly to radio code, which is the right boundary for adding MeshCore and apps later.

### Meshtastic Backend

`src/backend_sx1262.cpp`, `src/services/mtproto.*`, and `src/mtpki.cpp` implement:

- SX1262 LongFast radio profile
- Meshtastic packet header read/write
- default LongFast channel hash and AES-CTR payload crypto
- Data protobuf encode/decode
- NodeInfo parse and public-key capture
- PKI DM encrypt/decrypt using X25519 plus AES-256-CCM
- channel text receive/send
- direct text receive/send
- routing ACK send/receive
- managed-flood rebroadcast
- USB companion bridge forwarding hooks

This is the most complete firmware subsystem.

### MeshCore Groundwork

MeshCore support is partially present:

- second RF profile: 910.525 MHz, 62.5 kHz, SF7, CR4/5
- TDM switcher for Meshtastic/MeshCore slots
- MeshCore ADVERT parser
- Ed25519 persistent MeshCore identity
- signed self-advert builder and forced send command
- serial diagnostics for MeshCore identity, RF tuning, advert selftest

MeshCore support is not product-enabled:

- `LZ_MESHCORE_ENABLED` is `0`
- onboarding/settings/home/messages/contacts all gate or dim MeshCore
- MeshCore screen returns "Coming soon"
- only ADVERT packets are decoded; encrypted text/group payloads are counted but not opened
- no MeshCore send path exists in `lz_svc_send_text`

### UI

The LVGL UI is broad and polished for alpha:

- onboarding
- lock screen and notification card
- home launcher
- unified Messages and Conversation screens
- Meshtastic manager with virtualized node list
- gated MeshCore manager
- Contacts and contact detail
- Settings, Wi-Fi, System/Battery, touch calibration
- App Store prototype
- Terminal
- Files prototype

The UI still mixes real product surfaces with demos/prototypes. The biggest examples are App Store fake install state and static Files rows. Terminal now sits behind Developer Mode instead of the default Home launcher.

## Findings And Risks

| Priority | Area | Finding | Impact | Recommended Action |
| --- | --- | --- | --- | --- |
| P0 | MeshCore | MeshCore is compiled as groundwork but product-disabled with `LZ_MESHCORE_ENABLED 0`. | The README's "two meshes" product goal is not complete. | Treat MeshCore as the central Stage 2 program: receive, send, encrypted payloads, public channel/rooms, inbox routing, tests, then flip the gate. |
| P0 | App system | App Store is disabled in Home and uses mutable demo rows/timers. Lua VM, manifest loading, install, catalog, SHA verification, permissions, and app launch are absent. | The repo advertises an app ecosystem, but the implementation is currently a prototype. | Build the app runtime and local app scanner before network catalog polish. |
| P1 | Build configuration | PlatformIO reports the generic `esp32-s3-devkitc-1` N8/no-PSRAM board while the project assumes 16 MB flash, OTA partitions, and PSRAM buffers. | Upload/boot/memory behavior may diverge from the real T-Deck unless the board config is pinned to the actual hardware. | Verify LilyGO T-Deck flash/PSRAM settings and encode them in `platformio.ini`; add CI size/build checks. |
| P1 | Build workflow | T-Deck build emitted artifacts locally but hit Windows file-lock/timeouts. Native simulator cannot run without SDL2 tooling. | Contributors cannot rely on a single clean command across common host setups. | Add CI plus documented Windows/macOS/Linux prerequisites; make simulator dependency detection fail with a clear message. |
| P1 | Persistence | Identity, Wi-Fi, touch calibration, keys, nodes, threads, logs, and user settings persist. Wi-Fi and keys are still plaintext on SD. | Credentials and keys are exposed if the SD card is read. | Move credentials to NVS or encrypted store; add optional device PIN/password before encrypting all local data. |
| P1 | Delivery status | Sent DMs track `SENDING/DELIVERED/FAILED` only in the open in-memory tail; `lz_svc_send_text` does not act on the immediate `lz_backend_send` return value. | Delivery UI can be wrong after reopen/reboot or after immediate TX failure. | Persist sent-message status, record backend failures, and add retransmit/ACK tests. |
| P2 | App launcher | Terminal is hidden behind Developer Mode; App Store is shown but disabled. | The power-user flow is now gated, but the app ecosystem is still a prototype. | Make App Store real or remove it until usable; expand Developer Mode diagnostics later. |
| P2 | Files | Files screen renders static sample rows from `LZ_FILES`. | It looks like a file browser but does not inspect SD/appfs. | Implement a read-only SD/appfs browser, then gated file actions. |
| P2 | GPS/position/telemetry | GPS toggle and sample map/weather data exist, but GPS driver and Meshtastic position/telemetry decode are not present. | Map, weather, telemetry, and emergency features lack data plumbing. | Add decoders and platform hooks before building user-facing apps that depend on them. |
| P2 | Feedback/emergency | Master spec calls for LED, buzzer, DND, priority feedback, and SOS, but no service owns those outputs. | Notifications and emergency behavior are screen-only. | Add a Feedback Manager service before emergency beacon and OTA status UX. |
| P3 | Documentation consistency | README says MeshCore is both "landed" and "not receiving"; feature claims mix hardware-tested, code-present, and prototype states. | Contributors may work from an inaccurate mental model. | Link this audit and feature inventory from README; keep README status labels aligned with code gates. |

## Near-Term Recommendations

1. Make the build trustworthy: real T-Deck board config, CI, simulator setup docs, firmware size budget, and a repeatable release command.
2. Normalize status language: "working", "partial", "prototype", "planned", and "not hardware verified in this audit".
3. Finish Meshtastic release polish before expanding scope: unread highlighting, badge, mute/silence, persistent delivery state, and ACK/retransmit.
4. Bring MeshCore online behind tests, not optimism: TDM soak, ADVERT interop, public channel, DM/group message decode/send, and unified inbox behavior.
5. Turn App Store into an actual platform incrementally: local SD app scanning first, then Lua sandbox, then network catalog/download/update.
6. Expand Developer Mode diagnostics only after the primary OS remains simple by default.
