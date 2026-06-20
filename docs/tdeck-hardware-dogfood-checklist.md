# T-Deck Hardware Dogfood Checklist

Use this checklist before calling the Meshtastic-only Phase 1 experience shippable.
It is written for stock Meshtastic interop first; MeshCore-only and split-airtime
dogfood belong to the later roadmap phases.

## Test Rig

- Device under test: LilyGO T-Deck running the current LimitlezzOS firmware.
- Peer A: stock Meshtastic device on the same region/preset/channel.
- Peer B: optional second stock Meshtastic device for routing and multi-hop checks.
- Host: Windows machine with PlatformIO and USB serial access.
- Preferred serial port on the maintainer test host: `COM8`, re-verified each run.

## Preflight

- Record the git commit, branch, and dirty/clean state.
- Build a fresh artifact with `pio run -e tdeck`.
- On slow local hosts, prefer the GitHub Actions artifact for the exact pushed
  commit: `python scripts/fetch_tdeck_artifact.py`, then flash with
  `python scripts/tdeck_smoke.py --no-stub-upload --skip-build --artifact-dir .pio/ci-artifacts/tdeck --port COM8`.
- Record the firmware artifact path, size, and timestamp.
- Keep the default smoke command set unless a test needs a narrower probe:
  `id`, `sys`, `net`, `rf`, `stats`, `wifi`, `dm status`, `nodes`,
  `companion test`, and `companion ble`.
- Confirm a native simulator sanity pass:
  - Linux/macOS: `pio run -e native && .pio/build/native/program --selftest`
  - Windows: `pio run -e native; .pio\build\native\program.exe --selftest`
- Confirm the SD card is mounted or deliberately test RAM-only behavior.
- Confirm peer devices are on the intended Meshtastic channel and can exchange messages with each other.

## Flash And Boot Evidence

- Flash with `pio run -e tdeck -t upload`.
- If the normal upload path is flaky on the local T-Deck/host pair, fall back to
  the direct-flash helper with `python scripts/tdeck_smoke.py --port COM8 --no-stub-upload`
  or the same command with the Linux serial device path.
- For serial CLI smoke without flashing, run `python scripts/tdeck_smoke.py --skip-upload --port COM8`
  on the Windows rig or pass the Linux/macOS device path such as `/dev/ttyACM0`.
- The standard read-only smoke command set retries one no-reset serial reattach
  automatically after a timeout, which covers the COM8 post-flash USB handoff
  case without reflashing. For custom command lists, pass `--reattach-retries N`
  only when repeating the command sequence is safe.
- Open the USB console at 115200 baud.
- Capture the boot banner and every `[ok]` or failure line.
- Confirm display, touch, keyboard, trackball, SD, SX1262, Wi-Fi state, battery, and time source are reported.
- Run `help` and confirm diagnostics include `dm status`, `rxlog`, `nodes`, `net`, `rf`, `companion`, and `companion ble`.
- For split-airtime runs, capture `rf` before and after traffic. The `timing:`
  line reports delayed-switch count, average/max lateness, and whether expired
  slots were held by in-flight RX or MeshCore ACK dwell; this proves scheduler
  hold behavior but not packet-loss rate by itself.
- For BLE companion phone-app drops, run `companion ble on`, attempt the official app connection, then run `companion` after the drop and capture the whole BLE line. The key fields are `c`/`d` (connect/disconnect counts), `r` (last GAP disconnect reason), `mtu`, `to` (ToRadio writes/last bytes), `fr` (FromRadio reads), and `fn` (FromNum reads/writes).
- The default smoke now captures delivery state, heard nodes, USB companion
  loopback, and BLE companion status. Run `help` afterward and confirm
  diagnostics include `dm status`, `rxlog`, `nodes`, `net`, `rf`, `companion`,
  and `companion ble`.

## Hardware Evidence Log

### 2026-06-18 COM8 TDM Timing Diagnostic Smoke

- Firmware flashed: PR #12 `tdeck-firmware-7aecfc7aa47565a0228143befb63406abe223cf4` artifact from upstream Firmware CI run `27730027088`.
- Artifact manifest recorded `budget_status=pass`, `firmware_bytes=1535408`, and `static_ram_bytes=274708`.
- Port boundary: only `COM8` was opened/flashed/probed during this validation.
- Direct ROM flashing with `python -m esptool --chip esp32s3 --port COM8 --baud 460800 --no-stub ... write-flash ...` succeeded; bootloader, partitions, `boot_app0.bin`, and firmware hashes all verified.
- COM8 serial smoke passed with `python scripts/tdeck_smoke.py --skip-upload --port COM8 --open-timeout 60 --boot-timeout 60 --timeout 30 --commands id rf stats`.
- `rf` reported the new timing diagnostic line on hardware: `timing: late 0 avg 0ms max 0ms | holds rx 0 ack 0`.
- This run used the default Meshtastic-only image, so it proves the diagnostic is present and stable in serial output; split-airtime RX/ACK hold counts still need a MeshCore-enabled soak run after the TDM artifact branch lands.
- For MeshCore-enabled Phase 3 builds, fetch and flash the opt-in CI artifact
  with `python scripts/fetch_tdeck_artifact.py --env tdeck-meshcore`, then
  `python scripts/tdeck_smoke.py --port COM8 --env tdeck-meshcore --no-stub-upload --skip-build --artifact-dir .pio/ci-artifacts/tdeck-meshcore`.
  Run the split-airtime probe after the basic serial smoke:
  `python scripts/tdm_airtime_smoke.py --port COM8` on the Windows rig, or pass
  the Linux/macOS serial path such as `/dev/ttyACM0`. This asserts the 60/40,
  50/50, and 40/60 dwell presets, checks that `switches:` advances between `rf`
  samples, and confirms `net mc off` returns the radio to `Meshtastic 100%`. If
  the firmware still has MeshCore gated, the probe exits with a clear failure
  instead of recording a false TDM pass.

## Hardware Evidence Log

### 2026-06-18 COM8 MeshCore TDM Smoke

- Firmware flashed: opt-in `tdeck-meshcore` GitHub Actions artifact from `n30nex/LimitlezzOS` run `27728176574`, commit `1552fa8`.
- Artifact manifest recorded `meshcore_enabled=1`, `budget_status=pass`, `firmware_bytes=1536832`, and `static_ram_bytes=274676`.
- Port boundary: only `COM8` was opened/flashed/probed during this validation.
- Direct ROM flashing with `python scripts/tdeck_smoke.py --port COM8 --env tdeck-meshcore --no-stub-upload --skip-build --artifact-dir .pio/ci-artifacts/tdeck-meshcore --upload-baud 460800` succeeded; bootloader, partitions, `boot_app0.bin`, and firmware hashes all verified.
- Split-airtime smoke passed with `python scripts/tdm_airtime_smoke.py --port COM8 --open-timeout 60 --boot-timeout 60 --timeout 30`.
- TDM evidence: `airtime mt` reported 60/40 with 300/200 ms dwell and `switches: 2`; `airtime balanced` reported 50/50 with 250/250 ms dwell and `switches: 3`; `airtime mc` reported 40/60 with 200/300 ms dwell and `switches: 4`.
- Switch motion evidence: after returning to balanced mode, `rf` advanced from `switches: 5` to `switches: 10` during the settle window and the active side flipped from MeshCore to Meshtastic.
- Restore evidence: `net mc off` returned `rf` to `mode: Meshtastic 100%` with no additional switch-count growth.
- Follow-up serial smoke passed with `python scripts/tdeck_smoke.py --skip-upload --port COM8 --env tdeck-meshcore --open-timeout 60 --boot-timeout 60 --timeout 30`.
- Serial smoke evidence: identity `!a20d1428` / `limitlessdeck`, battery 100% on USB, `net` reported Meshtastic on and MeshCore off after restore, `rf` reported `mode: Meshtastic 100%`, `stats` reported radio TX/RX counters, Wi-Fi reported `cred=nvs`, and `companion test` reported `63 frames ... -> PASS`.
- Remaining gap: this proves the opt-in MeshCore TDM image reports correct dwell presets and live switch motion on COM8; it does not yet prove packet loss, latency, or real simultaneous Meshtastic/MeshCore traffic impact.

### 2026-06-14 COM8 Smoke Attempt

- Branch/commit flashed: `codex/tdeck-firmware-audit-roadmap` at `d5f69e0`.
- Port boundary: only `COM8` was opened/flashed/probed during this attempt.
- COM8 open preflight passed with `python scripts/serial_harness.py --port COM8 --baud 115200 --open-only --open-timeout 5`.
- Normal PlatformIO upload reached the ESP32-S3 on `COM8` but failed during the stub baud-rate handoff with `No serial data received`.
- Direct ROM flashing with PlatformIO's packaged `esptool.py v4.5.1 --no-stub` on `COM8` succeeded; bootloader, partitions, `boot_app0.bin`, and firmware hashes all verified.
- A local Windows COM8 attach with DTR/RTS preset low before opening the serial handle avoided the host reset loop and reached the LimitlezzOS `lz>` prompt; no Windows helper was added to the repo.
- COM8 CLI smoke passed for `id`, `sys`, `net`, `rf`, `stats`, `wifi`, and `companion test`.
- BLE companion firmware build proof is present in the V0.5 branch; still run official Meshtastic app pairing, reconnect, send, receive, and disconnect validation before marking BLE hardware-tested.
- Companion self-test evidence: `27 frames | my_info=1 metadata=1 config=1 channel=1 complete=1 nonce=1234abcd -> PASS`.
- Post-flash serial before the local attach fix showed ESP-ROM `SPI_FAST_FLASH_BOOT` and app entry at `0x403c98d0`, then `COM8` disconnected during USB handoff before the LimitlezzOS `lz>` prompt appeared.
- The ROM saved PC `0x420c67ae` decoded against the flashed ELF to `esp_pm_impl_waiti`, which indicates the previous reset happened while the app was idle rather than at a decoded crash site.
- Remaining gap: do not count stock Meshtastic peer dogfood as complete from this serial-only evidence.

### 2026-06-17 COM8 Android BLE Companion Validation

- Firmware artifact flashed before this validation: fork CI artifact for commit `33c8836` on branch `codex/ble-companion-disconnect-diagnostics`.
- COM8 smoke passed after flashing with `scripts/tdeck_smoke.py --no-stub-upload --skip-build`; companion self-test reported Meshtastic-compatible metadata with `fw=2.7.15.567b8ea` and `min_app=30200`.
- Official Android app connected over Bluetooth to `limitlessdeck` and showed firmware version `2.7.15.567b8ea`, clearing the previous "radio firmware is too old" block.
- Android Nodes tab populated through the BLE companion session, showing 2 nodes of 55 total in the captured view.
- Android LongFast chat sent `Test` and `Ping` through the connected T-Deck and received `Pong! --> limitlessdeck`, proving app-side LongFast send/receive over BLE on this firmware.
- Remaining V0.5 soak gap: repeat reconnect, intentional disconnect, and coexistence testing while capturing the serial `companion` line fields (`c`, `d`, `r`, `mtu`, `to`, `fr`, `fn`).

## Meshtastic Channel Interop

- Receive a LongFast text from Peer A on the T-Deck.
- Send a LongFast text from the T-Deck and confirm Peer A receives it.
- Repeat while Peer B is present and confirm no duplicate spam appears after managed flood/dedup.
- Turn `rxlog on`, repeat one receive, and capture decoded packet evidence.
- Reboot the T-Deck and confirm channel history remains visible when the SD card is present.

## Encrypted DM And Delivery State

- Send an encrypted DM from Peer A to the T-Deck.
- Reply from the T-Deck to Peer A.
- Confirm the sent bubble transitions from sending to delivered when ACK is observed.
- Run `dm status` while a message is pending, after delivery, and after a forced failure.
- Force a failed send by temporarily removing the peer or changing the peer channel.
- Confirm the UI shows the failure reason and long-press resend is capped by the retry limit.
- Reboot after sending, then reopen the conversation and confirm persisted delivery metadata is still shown.

## Node, Position, And Telemetry Decode

- Confirm `nodes` lists stock Meshtastic peers with name, ID, last-heard, and SNR.
- From a peer with GPS enabled, send or wait for a POSITION packet.
- Confirm the node list marks GPS presence and the contact detail shows latitude/longitude and altitude when present.
- From a peer with telemetry enabled, wait for device/environment metrics.
- Confirm contact detail shows voltage, battery/uptime when available, and temperature/humidity/pressure when available.
- Reboot and confirm node position/telemetry fields reload from the node database.

## UI Regression Pass

- Lock screen: receive one unread message, tap the notification, and confirm it opens the correct thread.
- Lock screen: receive multiple unread threads and confirm the `+N more` text is accurate.
- Home: confirm Messages badge shows 1-9 and then `9+`.
- Messages list: confirm unread rows are highlighted and read rows return to normal after opening.
- Long-press a chat to mute it; confirm the crescent indicator appears and the chat is excluded from lock/Home badges.
- Long-press again to unmute and confirm unread badge behavior returns.
- Conversation: type a long draft, use Enter to send, and confirm keyboard latency remains acceptable.
- Settings: scroll every page, change brightness/time/Wi-Fi/sleep settings, and confirm focus does not jump.
- Long lists: scroll Meshtastic nodes and contacts with trackball and touch; confirm rows recycle without visual corruption.

## Consumer Flow

- Confirm first-run onboarding remains understandable without opening Terminal.
- Confirm Terminal is hidden on Home until Developer Mode is enabled.
- Confirm Developer Mode persists across reboot and disabling it hides Terminal again.
- Confirm a non-technical user can send/receive LongFast, send/receive DMs, inspect delivery state, join Wi-Fi, set time, change sleep timeout, and recover from a failed message without serial commands.

## Evidence To Save

- Git commit/branch and firmware artifact metadata.
- Serial boot log.
- `dm status` output for pending, delivered, failed, and retry-limit states.
- `nodes` output showing name/ID/SNR plus GPS/telemetry hints.
- Photos or screenshots of lock notification, Home badge, unread row, muted chat, delivery states, and contact telemetry.
- Pass/fail notes for every item above, including device models and Meshtastic firmware versions for peers.
