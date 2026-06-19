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

## Author Beta Milestones

These maintainer-provided beta labels are the canonical near-term sequence. The broader phases below preserve that order, then add post-V0.96 completion work for OTA, the full App Store, security, feedback, emergency, and release hardening.

**Current release: Beta 0.6.** MeshCore public chat (V0.6) and encrypted DMs (V0.7) are implemented and hardware-verified against a live mesh. **Two open items:** (1) split airtime (the Meshtastic↔MeshCore TDM scheduler) may not be working reliably and needs re-verification; (2) V0.5 BLE companion advertises/serves GATT and the official app connects, but the session drops immediately (connect-then-disconnect). Also delivered this cycle (outside the milestone list): a desktop SDL2 simulator with a 50+ assertion codec/scenario self-test harness, and Wi-Fi/BLE mutual exclusion (they share scarce internal DMA RAM on the ESP32-S3, so only one is resident at a time).

| Version | Milestone | Status |
| --- | --- | --- |
| V0.5 | BLE companion for Meshtastic | 🚧 Firmware done — advertises + GATT (ToRadio/FromRadio/FromNum) works on hardware; **connect-then-disconnect** with the official app is open |
| V0.6 | MeshCore public chat and split airtime config | 🚧 Public chat send/receive hardware-verified; **split airtime may not be working — needs re-verification**; config UI still TODO |
| V0.7 | MeshCore DMs and private chats | ✅ Encrypted DMs (X25519 ECDH + AES) send/receive hardware-verified against a real MeshCore peer |
| V0.8 | MeshCore USB companion and MeshCore BLE companion | ⬜ Not started |
| V0.9 | Code review, optimization, and emoji polish | ⬜ Not started |
| V0.95 | Basic app SDK and infrastructure; Home UI supports adding apps and multiple home screens | 🚧 Local manifest scanner, Home paging, and detail shell started; runtime/catalog still TODO |
| V0.96 | Upgraded Wi-Fi password storage | ⬜ Not started |

## Phase 0 - Stabilize The Baseline

Goal: make Alpha 0.41 trustworthy to build, test, and describe.

Deliverables:

- Confirm and encode the correct LilyGO T-Deck board profile: 16 MB flash, PSRAM, upload flash size, partition compatibility, and memory type.
- Add CI for `pio run -e tdeck` with firmware size reporting. Implemented in `.github/workflows/firmware.yml`; CI builds the T-Deck target, captures `pio run -t size`, and uploads firmware artifacts.
- Make CI artifacts usable for local hardware smoke on slow hosts. Implemented: Firmware CI now uploads a flash bundle with bootloader, partitions, boot app, firmware, ELF/map, and manifest; `scripts/fetch_tdeck_artifact.py` downloads the exact-commit artifact for local `scripts/tdeck_smoke.py --skip-build` flashing.
- Add a simulator CI path or clear host-specific setup docs for SDL2. Implemented in `.github/workflows/firmware.yml`; Ubuntu CI installs SDL2, Windows can install the local SDL2 bundle with `scripts/ensure_sdl2_windows.ps1`, and `pio run -e native` runs through `scripts/pio_native_sdl2.py`.
- Make `pio run -e native` fail clearly when SDL2 is absent. Implemented by `scripts/pio_native_sdl2.py`; Linux/macOS use `sdl2-config` or `pkg-config`, while Windows uses `SDL2_DIR` or the local `.deps` bundle.
- Add a release checklist covering build, flash, boot log, display, touch, keyboard, trackball, SD, radio, Wi-Fi, companion, and sleep.
- Update README status wording so "working", "partial", "prototype", and "planned" are distinct.
- Persist user settings beyond identity/Wi-Fi/touch/keys: brightness, timeout, clock format, time zone, keyboard light, TX power, network toggles, power saving. Implemented through `settings.cfg`; Wi-Fi credential hardening remains V0.96.
- Hide Terminal behind a temporary Developer Mode setting or remove it from the default launcher until Developer Mode exists. Implemented: Terminal is hidden from Home until Developer Mode is enabled in Settings.

Exit criteria:

- Fresh clone can produce a T-Deck firmware artifact through one documented command.
- README and docs no longer imply MeshCore messaging or App Store installs are complete.
- Every release has a reproducible evidence checklist.

## Phase 1 - Finish The Meshtastic Product

Goal: make the Meshtastic-only user experience feel complete enough to ship independently.

Deliverables:

- Implement roadmap items 0.42 through 0.45:
  - unread highlighting in Messages. Implemented: unread rows use the dark-mint emphasis and brighter sender text.
  - Messages launcher badge with 1-9 and plus behavior. Implemented: the Home Messages tile shows 1-9, then `9+`, excluding muted chats.
  - silence/mute for public/group chats. Implemented: long-press toggles mute and shows the crescent indicator.
  - responsiveness pass for Settings, chat log, keyboard input, and long lists. Implemented in software: conversation typing/backspace now refreshes the compose label in place instead of rebuilding the full chat log; conversation rebuilds preserve scroll position unless already pinned to bottom; Contacts now uses the virtualized list helper and virtual-list touch scroll stays bounded; Settings brightness left/right updates the slider in place instead of rebuilding the whole screen. Still open: hands-on hardware latency regression proof; the 2026-06-14 COM8 run proved no-stub flash and USB CLI smoke.
- Make delivery state durable:
  - persist sent-message packet IDs and delivery status. Implemented for newly sent DMs through message-log metadata.
  - reflect immediate backend send failures. Implemented: failed transmit attempts mark the sent bubble failed.
  - retain failed/sending/delivered state across conversation reopen and reboot. Implemented for newly sent DMs, including queued retry metadata.
- Expand routing/ACK behavior:
  - retransmit queue. Implemented for tracked sent-DM records: expired pending messages recover their text from the log and retry automatically up to the cap.
  - retry limits. Implemented for manual failed-DM resend with a capped retry count.
  - failure reason display. Implemented for radio-send failure, ACK timeout, and retry-limit exhaustion.
  - serial diagnostics for pending messages. Implemented as `dm status` in the USB console.
- Decode Meshtastic position and telemetry packets for node detail and future apps.
  Implemented for basic GPS position, altitude, precision, device battery/voltage/uptime,
  and environment temperature/humidity/pressure metrics.
- Keep radio settings simple in the primary UI; advanced region/preset/channel controls, if added, belong in Developer Mode.
- Replace static Files screen with a read-only SD/appfs browser. Implemented:
  Files exposes mounted SD/local storage and the mounted FAT `appfs` partition
  through a storage-root picker, with browsing constrained under the selected
  root.
- Add hardware dogfood checklist against stock Meshtastic devices. Created as `docs/tdeck-hardware-dogfood-checklist.md`; 2026-06-14 COM8 flash and USB CLI smoke evidence is logged there, and `scripts/tdeck_smoke.py` now gives both Windows and Linux developers a repeatable serial-smoke path. Stock Meshtastic peer dogfood still needs a passing run before calling Phase 1 shippable.

Exit criteria:

- A non-technical user can onboard, send/receive LongFast, send/receive encrypted DMs, see delivery state, manage Wi-Fi/time/sleep, and recover from normal failures without using the terminal.
- Terminal is not part of the default consumer flow.

## Phase 2 - V0.5 Meshtastic BLE Companion

Goal: let the official Meshtastic app connect wirelessly to the T-Deck radio after the USB companion path is stable.

**Status (Beta 0.6): mostly done — one open bug.** BLE transport, the GATT service, the USB/BLE companion UI rows, USB↔BLE arbitration, and the serial selftest are implemented and on hardware. The official app discovers and connects to the radio, but the session drops immediately (**connect-then-disconnect**) — the one open item, tracked as the current top bug (likely BLE bonding/security or the want_config handshake).

Deliverables:

- Add BLE transport for the Meshtastic companion protocol. Implemented in firmware with NimBLE-Arduino and the official Meshtastic BLE GATT service UUIDs: `ToRadio` writes, `FromRadio` reads, and `FromNum` read/notify/write.
- Reuse the USB companion handshake/model where possible so node DB, channel, config, and packet forwarding behavior stay consistent. Implemented: USB and BLE both feed the same `ToRadio` handler and `FromRadio` builders.
- Add clear UI state for USB companion, BLE companion, and normal serial console mode. Implemented: Meshtastic -> Nodes now has separate USB and BLE companion rows.
- Define what happens when USB and BLE companion clients compete for the radio. Implemented: only one external app bridge is active at a time; enabling BLE disables USB companion mode, and enabling USB turns BLE advertising/connection off.
- Add serial diagnostics and a loopback/selftest equivalent for BLE where practical. Implemented: `companion ble on|off|test`, BLE status reporting, and a BLE mailbox/fromnum selftest.
- Hardware-test pairing, reconnect, send, receive, and disconnect flows with the official app.

Exit criteria:

- A phone can pair over BLE, see the T-Deck as a Meshtastic companion radio, and send/receive through the T-Deck without USB. Firmware path is implemented; official app hardware validation remains open.
- Normal on-device messaging still works when BLE companion is off.

## Phase 3 - V0.6 MeshCore Public Chat And Split Airtime Config

Goal: make MeshCore visible in the real product through public chat first, while giving users a simple way to understand and control split airtime.

**Status (Beta 0.6): mostly done — split airtime suspect.** MeshCore ADVERT interop, public/default channel receive, group/room text, the send path through `lz_svc_send_text`, unified-inbox wiring, the public-chat network toggle, and dual-network unread badges all work on a live mesh. **Open:** the split-airtime TDM scheduler (time-sharing the one SX1262 between Meshtastic and MeshCore) **may not be working reliably and needs re-verification** — earlier hardware checks looked OK but the behavior is now in question. Also remaining: a user-facing split-airtime *config UI* (currently a fixed 60/40 split toward Meshtastic).

Deliverables:

- Validate TDM on real hardware:
  - slot switching latency
  - missed-packet rate
  - Meshtastic delivery impact while MeshCore is enabled
  - MeshCore delivery impact while Meshtastic is enabled
- Confirm target MeshCore RF profiles by region and define how they coexist with the LongFast-only product goal.
- Build the split airtime config UI around simple choices, not raw radio parameters. Implemented: Settings now exposes Meshtastic first, Balanced, and MeshCore first presets, persists the choice, and reports the active dwell split through serial diagnostics.
- Finish MeshCore packet handling:
  - ADVERT interop with real MeshCore nodes
  - public/default channel receive
  - group/room text receive
- Add MeshCore send path through the same `lz_svc_send_text` service boundary.
- Wire MeshCore public chat and rooms into the unified inbox.
- Make the MeshCore network toggle active for public chat only after the path passes tests.
- Add lock-screen and launcher unread badges that count both networks correctly for public threads.

Exit criteria:

- With both networks enabled, the T-Deck receives and sends Meshtastic messages and MeshCore public chat in the same session.
- Split airtime settings are understandable to a non-expert and preserve the "just works" default.
- Disabling either network gives the other full airtime and preserves conversation history.

## Phase 4 - V0.7 MeshCore DMs And Private Chats

Goal: complete private MeshCore messaging after the public-chat path has proven the radio scheduler and inbox integration.

**Status (Beta 0.6): done (ahead of sequence).** MeshCore private receive + send (through the mesh service boundary), per-pair X25519 ECDH + AES key handling, ACK/routing, retry/failure status, unambiguous reply routing, role/key-gated messageability, and serial diagnostics are implemented and hardware-verified against a real MeshCore peer ("Limitlezz"). Note: this V0.7 work landed during the V0.6 cycle.

Deliverables:

- Implement MeshCore private chat receive.
- Implement MeshCore private chat send through the mesh service boundary.
- Add key/session handling, ACK/routing behavior, retry limits, and failure status for MeshCore private messages.
- Keep reply routing unambiguous in the conversation header and composer.
- Make MeshCore contacts messageable only when the node role and key/session state support it.
- Add serial diagnostics for private-chat state and delivery failures.

Exit criteria:

- MeshCore DMs/private chats can be sent and received with real MeshCore peers.
- Meshtastic DM behavior does not regress while MeshCore private messaging is enabled.

## Phase 5 - V0.8 MeshCore USB And BLE Companions

Goal: expose MeshCore companion functionality only after native MeshCore messaging is stable.

Deliverables:

- Define the MeshCore companion protocol surface for node DB, public chat, private chats, and send/receive forwarding.
- Implement MeshCore USB companion mode.
- Implement MeshCore BLE companion mode.
- Add UI and serial commands that distinguish Meshtastic companion from MeshCore companion.
- Decide whether one companion session or one network can own the external-app bridge at a time.
- Hardware-test pairing, reconnect, send, receive, and disconnect flows.

Exit criteria:

- MeshCore companion works over USB and BLE without breaking on-device messaging or Meshtastic companion behavior.

## Phase 6 - V0.9 Code Review, Optimization, And Emoji Polish

Goal: pause feature expansion long enough to make the firmware smaller, faster, easier to review, and nicer in everyday chats.

Deliverables:

- Full code review of the hardware, radio, service, storage, and UI boundaries.
- Optimize slow screens, keyboard input latency, radio loop timing, and memory hot spots.
- Add size and memory budgets to CI/release notes. Implemented: Firmware CI now
  checks `firmware.bin` against a 2,200,000-byte budget and static RAM against a
  307,200-byte budget, records the result in the artifact manifest, and uploads
  the detailed budget report with the flash bundle.
- Add screenshot and deterministic scenario coverage to CI. Implemented:
  Firmware CI now runs `--simtest`, generates native simulator screenshots, and
  uploads the screenshot artifact for release review.
- Clean up dead demo data and stale comments that no longer match product state.
- Add basic emoji rendering/input support appropriate for the T-Deck screen and memory budget.
- Re-run hardware dogfood tests on Meshtastic-only, MeshCore-only, and split-airtime modes.

Exit criteria:

- No known P0/P1 regressions remain from V0.5-V0.8.
- Firmware memory and flash usage are measured and documented before app SDK work starts.

## Phase 7 - V0.95 Basic App SDK, Infrastructure, And Home UI

Goal: make "apps" real at the OS level before adding full network catalog complexity.

Deliverables:

- Decide runtime after memory profiling: Lua 5.4, eLua, or a smaller interpreter.
- Define app package layout under SD/appfs:
  - `manifest.json`
  - app script entrypoint
  - optional assets
  - per-app data directory
  Implemented for the first local scanner step in
  `docs/tdeck-local-app-manifest.md`: each package has a bounded manifest, a
  relative entry file, optional assets, a local package directory, and scoped
  `data/` preparation for apps that declare the `storage` permission.
- Implement local app scanner for `/apps`. Implemented for local storage:
  firmware scans `/sd/limitlezz/apps`, `/sd/apps`, and `/appfs/apps`; the
  simulator scans `<datadir>/apps` and optional `<datadir>/appfs/apps`. Unsafe
  IDs/entry paths and packages without an entry file are rejected before
  reaching the UI. Appfs-only discovery is supported when SD persistence is
  absent.
- Fix the Home UI so installed apps can be added as launcher icons. Partially
  implemented: scanned local apps fill the Home launcher after built-ins and
  continue across 4x2 pages with page dots when needed.
- Add multiple Home screens/pages so the built-in app grid can grow without
  cluttering the first screen. Implemented for local manifest apps; full app
  lifecycle and sandbox execution remain below.
- Add app launcher integration for installed apps. Partially implemented:
  Home opens scanned local apps in an SDK 0.1 foreground shell that reads
  bounded display metadata and up to two bounded foreground actions from the
  entry file, including a storage-scoped counter effect for apps that request
  `storage`; unknown action effects and malformed counter effects are
  launch-blocked instead of ignored. App sessions terminate on exit; App Store
  opens the manifest detail shell with a trackball-accessible `OPEN` action.
  Script execution and richer injected runtime APIs remain below, with initial
  read-only `{time}` and `{battery}` token injection now routed through
  declared `system_time` and `battery` permissions.
- Parse app SDK metadata and permission namespaces from manifests. Implemented
  for SDK `0.1`: unknown permissions and unsupported SDK versions are rejected
  before reaching the launcher; API injection remains below.
- Enforce foreground-only app lifecycle.
  Initial implementation: local apps open in a single foreground session and
  exit back to the launcher/detail path; foreground actions can update only the
  current session body/status plus scoped counter state, and background
  execution is not exposed.
- Enforce memory cap through the runtime allocator or equivalent guard.
- Implement a small initial SDK:
  - UI primitives compatible with the T-Deck screen
  - mesh send/receive API through the service, not radio hardware
  - storage API scoped to app directory. Groundwork implemented: accepted local
    apps that declare `storage` get `<package>/data` prepared and surfaced in
    the detail shell, and launch is blocked when the current `data/` tree
    exceeds the early 64 KB quota. SDK 0.1 foreground actions can increment a
    safe counter file inside that scoped data directory, and App Store detail
    can clear only that scoped data directory; richer runtime API calls and
    richer quota controls remain below. Unsupported action effects and malformed
    counter keys are rejected before the foreground shell opens.
  - read-only system values. Groundwork implemented: SDK 0.1 entry and action
    text can use `{time}` and `{battery}` only when the manifest declares the
    matching `system_time` or `battery` permission; missing permissions block
    launch before the app shell opens.
  - notification request API routed through Feedback Manager
  - no direct hardware access
- Add Developer Mode app diagnostics and crash/error display. Partially
  implemented: rejected local package folders appear in App Store under
  Developer Mode with bounded rejection reasons; launch-blocked errors render in
  the local app foreground shell for oversized entry files and over-quota app
  data, and display-only apps that declare actions are blocked for missing input
  permission, while runtime crash capture remains below.
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

## Phase 8 - V0.96 Upgraded Wi-Fi Password Storage

Goal: replace plaintext Wi-Fi password storage with a safer storage path that still feels invisible to normal users.

**Status:** implemented for T-Deck hardware: saved Wi-Fi credentials now use ESP32 NVS, legacy `wifi.cfg` is migrated and removed after a successful NVS write, and serial `wifi` diagnostics show the credential backend without printing the password. The desktop simulator intentionally keeps file-backed credentials for local repeatability.

Deliverables:

- Move saved Wi-Fi credentials out of plaintext SD files. Implemented for T-Deck through NVS.
- Prefer NVS or an encrypted storage layer that can survive reboot and common SD-card workflows. Implemented with ESP32 NVS.
- Add migration from the existing `wifi.cfg` format. Implemented: legacy credentials are imported and the plaintext file is removed after NVS accepts the save.
- Keep the current "remember one network, auto-connect, forget" UX working. Implemented through the existing Wi-Fi service API.
- Add diagnostics for stored network state that never print the password. Implemented: serial `wifi` reports saved SSID, auto-connect, and credential backend only.

Exit criteria:

- A remembered Wi-Fi password is no longer readable as plaintext from the SD card.
- Existing users can upgrade without losing the saved network when migration succeeds.

## Phase 9 - Post-V0.96 Network App Store

Goal: let users install and update apps from a repository.

Deliverables:

- Define catalog `index.json` schema: app id, name, version, author, description, icon id/color, permissions, download URL, SHA256, size, compatibility, screenshots if desired.
- Fetch catalog over Wi-Fi.
- Cache catalog for offline browsing.
- Download app zip/package.
- Verify SHA256 before install.
- Extract to app staging directory, then atomically promote to installed directory.
- Show update badges on installed apps.
- Support uninstall/delete with data retention choice. Initial implementation:
  local App Store details expose keep-data and delete-data removal paths; retained
  data is hidden from app scanning and restored on reinstall.
- Add plain-language permission prompts.
- Add catalog/source settings suitable for community repos without confusing first-time users.

Exit criteria:

- App Store Home tile is enabled.
- GET/UPDATE/OPEN reflects real package state.
- Failed downloads or verification failures leave the prior app intact.

## Phase 10 - Post-V0.96 OTA Firmware Updates

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

## Phase 11 - Post-V0.96 Feedback, Notifications, And Emergency

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

## Phase 12 - Post-V0.96 Security And Privacy

Goal: protect the user's local data without making setup hard.

Deliverables:

- Add optional device PIN/password.
- Use the secret to encrypt:
  - message history
  - identity
  - Meshtastic PKI key
  - MeshCore key
  - app data
- Build on the V0.96 Wi-Fi credential migration so the remaining local secrets are protected consistently.
- Add migration from plaintext store.
- Add "forgot password" recovery language that is honest about data loss.
- Consider secure boot/flash encryption as an advanced build option after the app/update path stabilizes.

Exit criteria:

- A lost SD card does not reveal messages, identity, keys, or Wi-Fi credentials when the user has enabled protection.

## Phase 13 - Complete Firmware Release

Goal: finish the repo-listed feature set and make releases dependable.

Deliverables:

- Close remaining gaps from the audit and feature inventory.
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
