# Roadmap To Complete T-Deck Firmware

This roadmap starts from the Alpha 0.41 codebase audited on 2026-06-13 and ends at a complete, functional LimitlezzOS firmware with every major repo-listed feature implemented.

## Product Goal

LimitlezzOS should be a simple mesh handheld OS for non-experts:

- one friendly onboarding path
- LongFast Meshtastic by default, with no confusing radio setup in the main flow
- MeshCore and Meshtastic sharing the radio automatically
- one inbox with clear network tags
- optional apps for heavier features like maps
- developer tooling hidden behind Developer Mode

## Definition Of Done

The firmware is complete when:

1. A user can flash or OTA update a T-Deck and boot into a polished first-run flow.
2. Meshtastic channel messages and encrypted DMs work reliably with stock Meshtastic devices.
3. MeshCore discovery, public/group chat, and DMs work alongside Meshtastic through TDM.
4. Network enable/disable toggles are real and immediately rebalance airtime without losing history.
5. App Store can install, update, verify, and launch sandboxed apps.
6. Optional app examples cover the repo-listed app concepts: maps, APRS bridge, weather, BBS, signal scope, utilities, and games.
7. OTA firmware updates are safe, verified, and rollback-capable.
8. Local data can be protected by a PIN/password-backed encrypted store.
9. Hardware feedback, notifications, DND, and emergency behavior are consistent.
10. CI, simulator checks, hardware smoke tests, and release docs prove each release.

## Phase 0 - Stabilize The Baseline

Goal: make Alpha 0.41 trustworthy to build, test, and describe.

Deliverables:

- Confirm and encode the correct LilyGO T-Deck board profile: 16 MB flash, PSRAM, upload flash size, partition compatibility, and memory type.
- Add CI for `pio run -e tdeck` with firmware size reporting.
- Add a simulator CI path or clear host-specific setup docs for SDL2.
- Make `pio run -e native` fail clearly when `sdl2-config` is absent.
- Add a release checklist covering build, flash, boot log, display, touch, keyboard, trackball, SD, radio, Wi-Fi, companion, and sleep.
- Update README status wording so "working", "partial", "prototype", and "planned" are distinct.
- Persist user settings beyond identity/Wi-Fi/touch/keys: brightness, timeout, clock format, time zone, keyboard light, TX power, network toggles, power saving.
- Hide Terminal behind a temporary Developer Mode setting or remove it from the default launcher until Developer Mode exists.

Exit criteria:

- Fresh clone can produce a T-Deck firmware artifact through one documented command.
- README and docs no longer imply MeshCore messaging or App Store installs are complete.
- Every release has a reproducible evidence checklist.

## Phase 1 - Finish The Meshtastic Product

Goal: make the Meshtastic-only user experience feel complete enough to ship independently.

Deliverables:

- Implement roadmap items 0.42 through 0.45:
  - unread highlighting in Messages
  - Messages launcher badge with 1-9 and plus behavior
  - silence/mute for public/group chats
  - responsiveness pass for Settings, chat log, keyboard input, and long lists
- Make delivery state durable:
  - persist sent-message packet IDs and delivery status
  - reflect immediate backend send failures
  - retain failed/sending/delivered state across conversation reopen and reboot
- Expand routing/ACK behavior:
  - retransmit queue
  - retry limits
  - failure reason display
  - serial diagnostics for pending messages
- Decode Meshtastic position and telemetry packets for node detail and future apps.
- Keep radio settings simple in the primary UI; advanced region/preset/channel controls, if added, belong in Developer Mode.
- Replace static Files screen with a read-only SD/appfs browser.
- Add hardware dogfood checklist against stock Meshtastic devices.

Exit criteria:

- A non-technical user can onboard, send/receive LongFast, send/receive encrypted DMs, see delivery state, manage Wi-Fi/time/sleep, and recover from normal failures without using the terminal.
- Terminal is not part of the default consumer flow.

## Phase 2 - MeshCore End To End

Goal: turn the current MeshCore groundwork into a real second network without regressing Meshtastic.

Deliverables:

- Validate TDM on real hardware:
  - slot switching latency
  - missed-packet rate
  - Meshtastic delivery impact while MeshCore is enabled
  - MeshCore delivery impact while Meshtastic is enabled
- Confirm target MeshCore RF profiles by region and define how they coexist with the LongFast-only product goal.
- Finish MeshCore packet handling:
  - ADVERT interop with real MeshCore nodes
  - public/default channel receive
  - group/room text receive
  - DM receive
  - ACK/routing model
  - encrypted payload support
- Add MeshCore send path through the same `lz_svc_send_text` service boundary.
- Wire MeshCore contacts, rooms, and threads into the unified inbox.
- Make MeshCore network toggle active only after the full path passes tests.
- Add lock-screen and launcher unread badges that count both networks correctly.
- Add MeshCore companion bridge only after native MeshCore messaging is stable.

Exit criteria:

- With both networks enabled, the T-Deck receives and sends Meshtastic and MeshCore messages in the same session.
- Disabling either network gives the other 100 percent airtime and preserves conversation history.
- The user can always tell which network a message came from and which network a reply will use.

## Phase 3 - App Runtime And Local Apps

Goal: make "apps" real before adding network catalog complexity.

Deliverables:

- Decide runtime after memory profiling: Lua 5.4, eLua, or a smaller interpreter.
- Define app package layout under SD/appfs:
  - `manifest.json`
  - app script entrypoint
  - optional assets
  - per-app data directory
- Implement local app scanner for `/apps`.
- Add app launcher integration for installed apps.
- Enforce foreground-only app lifecycle.
- Enforce memory cap through the runtime allocator or equivalent guard.
- Implement a small initial SDK:
  - UI primitives compatible with the T-Deck screen
  - mesh send/receive API through the service, not radio hardware
  - storage API scoped to app directory
  - notification request API routed through Feedback Manager
  - no direct hardware access
- Add Developer Mode app diagnostics and crash/error display.
- Convert prototype catalog examples into installable sample apps where practical:
  - Calculator
  - Notes
  - Offline Maps shell
  - Weather Mesh
  - Mesh BBS
  - Signal Scope
  - LoRa Chess
  - APRS Bridge shell

Exit criteria:

- A user can copy an app to SD/appfs, see it in the launcher/store, open it, use it, leave it, and have it terminate cleanly.
- A broken app cannot crash the OS, access another app's files, or touch radio hardware directly.

## Phase 4 - Network App Store

Goal: let users install and update apps from a repository.

Deliverables:

- Define catalog `index.json` schema: app id, name, version, author, description, icon id/color, permissions, download URL, SHA256, size, compatibility, screenshots if desired.
- Fetch catalog over Wi-Fi.
- Cache catalog for offline browsing.
- Download app zip/package.
- Verify SHA256 before install.
- Extract to app staging directory, then atomically promote to installed directory.
- Show update badges on installed apps.
- Support uninstall/delete with data retention choice.
- Add plain-language permission prompts.
- Add catalog/source settings suitable for community repos without confusing first-time users.

Exit criteria:

- App Store Home tile is enabled.
- GET/UPDATE/OPEN reflects real package state.
- Failed downloads or verification failures leave the prior app intact.

## Phase 5 - OTA Firmware Updates

Goal: update the OS without USB flashing.

Deliverables:

- Implement firmware update manifest alongside the app catalog.
- Download firmware binary over Wi-Fi.
- Verify SHA256 before writing.
- Write to inactive OTA partition.
- Set OTA boot partition and reboot.
- Support rollback if new firmware fails to mark itself healthy.
- Add update UI with simple confirmation language.
- Route OTA progress and failure state through Feedback Manager.

Exit criteria:

- A device can update from one release to the next over Wi-Fi and recover safely from a failed update.

## Phase 6 - Feedback, Notifications, And Emergency

Goal: make physical feedback coherent and safety-oriented.

Deliverables:

- Implement Feedback Manager as the only owner of LED, buzzer, keyboard backlight notification pulses, and screen wake feedback.
- Add DND modes and priority queue:
  - normal messages
  - direct messages
  - system critical
  - emergency
  - OTA progress/failure
- Add low-battery and critical-battery behaviors.
- Implement emergency beacon:
  - key combo or guarded UI action
  - send on Meshtastic and MeshCore when available
  - lock-screen takeover
  - feedback pattern that bypasses DND
  - received beacon behavior
- Add notification settings with beginner-safe defaults.

Exit criteria:

- Screen, LED, buzzer, and keyboard backlight agree about message, warning, and emergency state.
- Emergency behavior is hard to trigger accidentally but impossible to miss once triggered.

## Phase 7 - Security And Privacy

Goal: protect the user's local data without making setup hard.

Deliverables:

- Add optional device PIN/password.
- Use the secret to encrypt:
  - message history
  - identity
  - Meshtastic PKI key
  - MeshCore key
  - app data
- Move Wi-Fi credentials to NVS or encrypted storage.
- Add migration from plaintext store.
- Add "forgot password" recovery language that is honest about data loss.
- Consider secure boot/flash encryption as an advanced build option after the app/update path stabilizes.

Exit criteria:

- A lost SD card does not reveal messages, identity, keys, or Wi-Fi credentials when the user has enabled protection.

## Phase 8 - Beta And Complete Firmware Release

Goal: finish the repo-listed feature set and make releases dependable.

Deliverables:

- BLE companion for Meshtastic.
- MeshCore companion if still product-relevant after native MeshCore support.
- Full docs:
  - user guide
  - app developer guide
  - hardware flashing/recovery guide
  - release checklist
  - troubleshooting
- Automated checks:
  - T-Deck compile
  - simulator selftest
  - screenshot generation where host supports SDL2
  - size budget
  - protocol unit tests/test vectors
- Hardware test matrix:
  - T-Deck and T-Deck Plus
  - SD present/absent
  - Wi-Fi present/absent
  - one Meshtastic peer
  - multiple Meshtastic peers
  - one MeshCore peer
  - both networks enabled
  - sleep/wake while receiving
  - OTA rollback
- Release gates:
  - no known P0/P1 bugs
  - README status matches code and test evidence
  - binaries attached to release
  - upgrade path documented

Exit criteria:

- LimitlezzOS is a complete firmware, not a prototype shell: both mesh networks work, apps install and run, updates are safe, security is available, and the user experience stays simple.
