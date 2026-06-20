# T-Deck Firmware Feature Inventory

Audit date: 2026-06-13

Refresh note: Wi-Fi credential rows were updated on 2026-06-19 from current
code evidence; full hardware smoke remains subject to the release evidence
workflow.

Status labels:

- Functional: implementation exists and is wired into the firmware path.
- Partial: important code exists, but the feature is incomplete or gated.
- Prototype: UI/demo behavior exists without the real backend.
- Planned: specified in README/design docs but not implemented in code.
- Needs validation: implementation exists but was not hardware-verified during this audit.

## Versioned Beta Milestone Mapping

| Version | Author milestone | Inventory areas touched |
| --- | --- | --- |
| V0.5 | BLE companion for Meshtastic | Meshtastic companion bridge |
| V0.6 | MeshCore public chat and split airtime config | MeshCore public channels, TDM, Settings airtime controls |
| V0.7 | MeshCore DMs and private chats | MeshCore private messaging, contacts, delivery status |
| V0.8 | MeshCore USB companion and MeshCore BLE companion | MeshCore companion bridge |
| V0.9 | Code review, optimization, and emoji polish | Build, memory, UI responsiveness, chat rendering |
| V0.95 | Basic app SDK/infrastructure and expandable Home UI | App runtime, Home launcher, multiple Home screens |
| V0.96 | Upgraded Wi-Fi password storage | Wi-Fi credential hardening |

## Build And Hardware Platform

| Feature | Status | Evidence | Gap / Next Action |
| --- | --- | --- | --- |
| T-Deck PlatformIO firmware target | Functional, CI-covered, needs hardware validation | `platformio.ini`, `src/main_tdeck.cpp`; `.github/workflows/firmware.yml` builds `pio run -e tdeck`, captures size output, and uploads firmware artifacts | Keep validating board profile against actual T-Deck flash/PSRAM during hardware smoke runs. |
| OTA-ready partition layout | Partial | `partitions.csv` has `ota_0`, `ota_1`, `otadata`, `config`, `appfs` | OTA service and update UI are not implemented. |
| Display and LVGL shell | Functional, needs validation | LovyanGFX ST7789 setup, LVGL buffers, UI screens | Use `docs/tdeck-release-checklist.md` for every release hardware pass. |
| Trackball and keyboard input | Functional, needs validation | GPIO interrupts and I2C keyboard polling in `main_tdeck.cpp` | Use the release checklist for hardware input proof; add input regression tests in simulator. |
| Display and LVGL shell | Functional, needs validation | LovyanGFX ST7789 setup, LVGL buffers, UI screens | Hardware flash/smoke checklist needed for every release. |
| Trackball and keyboard input | Functional, needs validation | GPIO interrupts and I2C keyboard polling in `main_tdeck.cpp`; native screenshot pass now asserts Home key navigation clamps, skips disabled tiles, pages, and opens the focused tile | Run COM8 hardware input smoke for keyboard, trackball roll/press, and wake-from-input behavior. |
| Touch and calibration | Functional, needs validation | GT911 driver and `touch.cfg` persistence | Add explicit touch calibration validation after flash. |
| SD-backed data directory | Functional, needs validation | SD mount and `/sd/limitlezz` store | Add corruption/power-loss tests and storage quota handling. |
| Battery/system telemetry | Partial | ADC battery reading, system screen stats | Charge state is inferred/limited; no dedicated charge-detect line. |
| Keyboard/display backlight | Functional, needs validation | LEDC display backlight and I2C keyboard PWM | Fold keyboard/display/buzzer/LED into Feedback Manager later. |
| Power saving/sleep | Partial | idle dim/lock and CPU down-clock | Needs hardware current measurements and background receive verification. |

## Meshtastic

| Feature | Status | Evidence | Gap / Next Action |
| --- | --- | --- | --- |
| LongFast public channel | Functional, needs validation | `mtproto.*`, `backend_sx1262.cpp`, channel thread | Add interop test notes with stock Meshtastic devices. |
| Channel send/receive | Functional, needs validation | `lz_svc_channel_thread`, `lz_core_on_text`, `lz_backend_send`, immediate failed bubble for backend TX rejection | Add stock-device interop notes and hardware TX-failure validation. |
| Node discovery | Functional, needs validation | NodeInfo parse, heard-node table, 250-node cap | Add role/hardware enum coverage beyond current minimal parse. |
| Node table persistence | Functional | `nodes.db` save/load with schema header v2, legacy-row migration selftest, serial `nodes test`, and position/telemetry round-trip coverage | Add future migrations as the node schema grows. |
| Node discovery | Functional, needs validation | NodeInfo parse, heard-node table, 250-node cap, Meshtastic role/hardware enum labels in native scenario coverage | Add stock-device role/hardware validation beyond simulator fixtures. |
| Node table persistence | Functional | `nodes.db` save/load | Add versioning/migration for future schema changes. |
| Node table persistence | Functional | `nodes.db` save/load now writes schema-tagged v2 rows and selftests v2 plus legacy untagged row loading | Add future migrations as the node schema grows. |
| Direct messages | Functional, needs validation | DM threads, `lz_svc_send_text`, PKI path | Add stock-device interop validation. |
| PKI encrypted DMs | Functional, needs validation | `mtpki.cpp`, NodeInfo public key capture | Add test vectors or a deterministic host test. |
| Routing ACK/delivery status | Partial | `send_routing_ack`, `lz_core_on_ack`, persisted sent-DM status metadata, failure reasons, bubble status colors, serial `dm status`; simulator now asserts ROUTING ACK delivery clears pending diagnostics and ignores unrelated ACKs | Add timeout/retransmit tests and hardware interop coverage. |
| Retransmit/resend | Partial | Long-press failed bubble calls `lz_svc_resend`; expired pending sent DMs retry automatically from persisted log metadata up to the retry cap | Needs hardware ACK/retransmit validation. |
| Managed flood rebroadcast | Functional, needs validation | `rebroadcast` in `backend_sx1262.cpp` | Needs airtime/backoff validation in busy meshes. |
| USB companion bridge | Functional, needs validation | `mt_companion.cpp`, serial commands, UI toggle | Config coverage is minimal; USB remains the hardware-tested app bridge. |
| BLE companion bridge | Functional, needs soak | `mt_companion.cpp` exposes Meshtastic BLE `ToRadio`, `FromRadio`, and `FromNum` over NimBLE; Meshtastic Nodes screen has separate USB/BLE companion rows; `companion ble on\|off\|test` reports status and mailbox proof; the companion handshake reports Meshtastic-compatible firmware/min-app metadata for current Android builds; 2026-06-17 COM8 Android photo evidence shows `limitlessdeck` connected as firmware `2.7.15.567b8ea`, nodes populated, and LongFast send/receive through the official app; `companion` reports connect/disconnect counts, last GAP disconnect reason, MTU, and characteristic IO counters for follow-up soak captures | Repeat reconnect, disconnect, and coexistence validation on hardware. |
| Position/telemetry decode | Partial, needs validation | POSITION and TELEMETRY payloads decode into node detail, node DB, serial `nodes`, and codec selftest fixtures | Add stock-device validation plus app-facing map/weather consumers. |
| Emergency channel/beacon | Prototype/Planned | Emergency row appears but is disabled; design spec covers SOS | Implement after feedback manager and dual-network send. |

## MeshCore

| Feature | Status | Evidence | Gap / Next Action |
| --- | --- | --- | --- |
| MeshCore compile-time gate | Partial | `#define LZ_MESHCORE_ENABLED 0` | Gate should stay off until receive/send/unified inbox tests pass. |
| TDM radio scheduler | Partial, needs validation | `lz_backend_set_networks`, profile switcher, settings airtime bar, serial `rf` diagnostics with delayed-switch average/max and RX/ACK hold counters | Needs hardware soak and packet-loss measurements with real simultaneous Meshtastic/MeshCore traffic. |
| TDM radio scheduler | Partial, serial smoke passed | `lz_backend_set_networks`, profile switcher, settings airtime bar, serial `rf` diagnostics, opt-in `tdeck-meshcore` CI artifact, and `scripts/tdm_airtime_smoke.py` for Windows/Linux dwell + switch-count smoke. The 2026-06-18 COM8 run passed 60/40, 50/50, 40/60 dwell checks and switch-count motion on a MeshCore-enabled image. | Needs hardware soak and latency/packet-loss measurements with real simultaneous Meshtastic/MeshCore traffic. |
| MeshCore RF profile | Partial, needs validation | 910.525 MHz / 62.5 kHz / SF7 / CR4/5 profile | Confirm target regions and RF compatibility with real MeshCore devices. |
| MeshCore ADVERT RX | Partial, needs validation | `mc_parse`, `mc_advert_decode`, `lz_core_on_mc_node` | Only ADVERTs are decoded; encrypted payloads are ignored. |
| MeshCore self-advert TX | Partial, needs validation | Ed25519 identity, self-advert builder, serial/UI advert commands | Needs interop proof with real MeshCore nodes. |
| MeshCore public channel / rooms | Planned | README says receive/default Public channel still ahead | V0.6: implement group text decode/send, room model, and split airtime config. |
| MeshCore DMs | Planned | MeshCore contacts are non-messageable while gated | V0.7: implement key/session model, send path, ACKs, and UI routing. |
| MeshCore companion bridge | Planned/In progress | `docs/tdeck-meshcore-companion-protocol.md` drafts the V0 USB serial line protocol; `companion mc ...` provides an initial firmware smoke surface for snapshots, sends, and self-test | V0.8: formalize USB first, mirror to BLE later, and do not claim external MeshCore app compatibility until the real app protocol is confirmed. |

## User Interface

| Feature | Status | Evidence | Gap / Next Action |
| --- | --- | --- | --- |
| First-boot onboarding | Functional | `scr_onboard.c`, identity persistence | MeshCore network row is visible but locked. |
| Lock screen | Functional | clock, battery, network icons, notification card | Add per-network badges once MeshCore is active. |
| Home launcher | Partial | filtered app grid, Developer Mode hides Terminal by default, Messages unread counter badge, scanned local apps flow across paged 4x2 Home screens | V0.95: add full app launch/runtime integration; run hardware visual regression for badge layout. |
| Unified inbox | Functional/Partial | Messages tabs, filters, unread highlighting, per-thread badges, mute indicator, channel tab | MeshCore filter is gated; finish hardware responsiveness pass. |
| Conversation view | Functional/Partial | compose, in-place draft text refresh, scroll-preserving chat rebuilds, bubbles, status colors, resend long-press, persisted sent-DM delivery metadata | Stock-device ACK/retry interop and hardware chat-log latency still need validation. |
| Meshtastic manager | Functional/Partial | identity card, virtualized node list, channels tab, separate USB and BLE companion toggles | Emergency channel row is disabled; BLE companion has Android connect/send/receive proof but still needs reconnect/disconnect soak. |
| MeshCore manager | Prototype/Partial | "Coming soon" unless gate is flipped; deeper screen exists behind gate | Do not enable until MeshCore message path works. |
| Contacts/detail | Functional/Partial | virtualized contacts list, add contact, messageable role check, bounded trace diagnostic for contact detail | MeshCore contacts locked; hardware long-list scroll and Trace-on-device need validation. |
| Settings | Functional/Partial | network toggles, Wi-Fi, in-place brightness slider updates, time, system, touch calibration, Developer Mode, `settings.cfg` persistence | Add migration/versioning if the settings schema grows; hardware latency pass still needed. |
| Wi-Fi setup | Functional for T-Deck, simulator file-backed, needs validation | async scan/connect, saved SSID/password, auto-connect; T-Deck credential backend reports `nvs` and legacy `wifi.cfg` migrates/removes after NVS save | Only one saved network; hardware validation should confirm NVS persistence, forget, and legacy migration on a real T-Deck. |
| Settings | Functional/Partial | network toggles, Wi-Fi, in-place brightness slider updates, time, system, touch calibration, Developer Mode, versioned `settings.cfg` schema v3 with v1/v2/v3 migration selftest and serial `settings test` | Hardware latency pass still needed. |
| Contacts/detail | Functional/Partial | virtualized contacts list, add contact, messageable role check | Trace action is a no-op; MeshCore contacts locked; hardware long-list scroll needs validation. |
| Settings | Functional/Partial | network toggles, Wi-Fi, in-place brightness slider updates, time, system, touch calibration, Developer Mode, versioned `settings.cfg` persistence with v1/v2/v3 selftest coverage | Hardware latency pass still needed. |
| Wi-Fi setup | Functional, needs validation | async scan/connect, saved SSID/password, auto-connect | Credentials are plaintext on SD; only one saved network. |
| System/battery page | Functional/Partial | live stats and battery arc | Hardware values need calibration/validation. |
| App Store | Prototype/Partial | Local manifests from SD/appfs are scanned, listed as installed local apps, open a manifest detail shell, show storage quota usage, clear scoped app data on request, uninstall local apps with keep-data or delete-data choices, launch into the SDK 0.1 foreground shell, expose bounded foreground actions with scoped storage counters plus read-only `{time}`/`{battery}` tokens and permission-gated `notify:` feedback requests, capture bounded launch/action faults, reject unsupported action effects, show rejected package diagnostics in Developer Mode, and render cached canonical network catalog entries as browse-only metadata rows with local update chips | Download/install/update, script execution/richer API injection, and full VM crash capture are still missing. |
| Terminal | Functional/Partial | interactive UI terminal behind Developer Mode; serial CLI always available over USB | Expand diagnostics once Developer Mode grows into a full power-user surface. |
| Files | Functional/Partial | read-only bounded filesystem browser rooted at mounted SD/local store or mounted FAT appfs; when both are present it starts at a Storage root picker | Add gated file actions later. |

## App Platform And Ecosystem

| Feature | Status | Evidence | Gap / Next Action |
| --- | --- | --- | --- |
| Lua sandbox | Planned | Design spec section 9 | Choose Lua/eLua/minimal interpreter after memory profiling. |
| App manifest | Partial | `docs/tdeck-local-app-manifest.md`; bounded manifest parser requires `id`, `name`, and relative `entry`, with optional version/author/summary/icon/hue plus SDK `api_version` and permission metadata | Extend once the runtime lifecycle and package actions are chosen. |
| App permissions | Partial | Local manifests can declare allowlisted SDK namespaces (`display`, `input`, `storage`, mesh, time, battery, notifications, Wi-Fi); unknown permission names reject the package before Home/App Store; `storage` prepares a scoped package `data/` directory with a 64 KB launch-time quota guard, SDK action counters require both `input` and `storage`, `notify:` actions require `notifications`, and `{time}`/`{battery}` tokens require matching `system_time`/`battery` permission before launch | Implement least-privilege API injection when the runtime is selected. |
| Local app scanner | Partial | `lz_store_scan_apps` scans `/sd/limitlezz/apps`, `/sd/apps`, `/appfs/apps`, simulator `<datadir>/apps`, and simulator `<datadir>/appfs/apps`; accepted apps appear in the paged Home launcher and App Store; rejected packages are exposed through Developer Mode diagnostics; simulator selftest covers appfs-only discovery, valid metadata, storage sandbox prep, quota usage, clear-data behavior, foreground launch metadata/actions, storage counter persistence, read-only time/battery token gating, unsupported action-effect blocking, oversized entry blocking, and rejected unsafe packages | Add script execution, richer app lifecycle hooks, and broader user-facing data actions once memory profiling picks a runtime. |
| Local app sample pack | Functional, CI-covered | `examples/local-apps/` provides copyable SDK 0.1 packages for Calculator, Field Notes, Offline Maps, Weather Mesh, Mesh BBS, Signal Scope, LoRa Chess, and APRS Bridge; `scripts/validate_local_app_samples.py` checks manifest limits, supported permissions, token permissions, foreground actions, and scoped counter effects in CI | Keep samples aligned with the eventual richer runtime/API injection and add richer example behavior once the interpreter is selected. |
| Local app scanner | Partial | `lz_store_scan_apps` scans `/sd/limitlezz/apps`, `/sd/apps`, `/appfs/apps`, simulator `<datadir>/apps`, and simulator `<datadir>/appfs/apps`; accepted apps appear in the paged Home launcher and App Store; rejected packages are exposed through Developer Mode diagnostics; simulator selftest covers appfs-only discovery, valid metadata, storage sandbox prep, quota usage, clear-data behavior, foreground launch metadata/actions, runtime memory-budget enforcement, storage counter persistence, read-only time/battery token gating, unsupported action-effect blocking, oversized entry blocking, and rejected unsafe packages | Add script execution, richer app lifecycle hooks, and broader user-facing data actions once memory profiling picks a runtime. |
| App permissions | Partial | Local manifests can declare allowlisted SDK namespaces (`display`, `input`, `storage`, mesh, time, battery, notifications, Wi-Fi); unknown permission names reject the package before Home/App Store; `storage` prepares a scoped package `data/` directory with a 64 KB launch-time quota guard, SDK action counters require both `input` and `storage`, and `{time}`/`{battery}` tokens require matching `system_time`/`battery` permission before launch | Implement least-privilege API injection when the runtime is selected. |
| Local app scanner | Partial | `lz_store_scan_apps` scans `/sd/limitlezz/apps`, `/sd/apps`, `/appfs/apps`, simulator `<datadir>/apps`, and simulator `<datadir>/appfs/apps`; accepted apps appear in the paged Home launcher and App Store; rejected packages are exposed through Developer Mode diagnostics; simulator selftest covers appfs-only discovery, valid metadata, storage sandbox prep, quota usage, clear-data behavior, foreground launch metadata/actions, storage counter persistence, bounded launch/action fault snapshots, read-only time/battery token gating, unsupported action-effect blocking, oversized entry blocking, and rejected unsafe packages | Add script execution, richer app lifecycle hooks, and broader user-facing data actions once memory profiling picks a runtime. |
| Network app catalog | Partial | `docs/tdeck-network-app-catalog.md`, `docs/examples/app-catalog-index.json`, and `scripts/validate_app_catalog.py` define and CI-validate `limitlezz.app.catalog.v1`; firmware validates the same canonical fields, loads typed cached entries, exposes serial `app catalog status\|test`, and feeds cached rows into App Store browsing/update metadata | Wire Wi-Fi fetch into source selection, verify TLS/source metadata, then add package download/install/update. |
| App download/install/update | Planned | App Store prototype only | SHA256 verify, extract, version updates, rollback failed installs. |
| App download/install/update | Planned/Partial | App Store prototype plus bounded package-file SHA256 helper | Wire the verifier into download/staging, then extract, version updates, and rollback failed installs. |
| Optional map app | Planned | Store data includes maps; maintainer notes prefer maps as optional | Keep maps out of the base firmware. |
| APRS/weather/BBS/scope/game apps | Planned/Prototype sample apps | `examples/local-apps/` sample packages and canonical catalog example metadata | Implement richer sandboxed behavior once runtime APIs exist. |

## Security, Updates, And Feedback

| Feature | Status | Evidence | Gap / Next Action |
| --- | --- | --- | --- |
| Device PIN/password | Partial | `docs/tdeck-device-security.md`; serial `security status`, `security set`, `security check`, `security clear`, and `security test`; `security.cfg` stores a salted, iterated SHA-256 verifier instead of plaintext PINs | Add Settings/lock-screen UI, unlock state, forgotten-PIN recovery language, and encrypted-store integration. |
| Encrypted local store | Planned | README Device security note; Phase 12 roadmap | Encrypt messages, keys, identity, and app data when password is set; migrate existing plaintext stores. |
| Wi-Fi credential hardening | Functional for T-Deck, sim file-backed | T-Deck `lz_store_save_wifi/load_wifi` use ESP32 NVS with legacy `wifi.cfg` migration/removal; serial `wifi` reports `cred=nvs` without printing the password | Native simulator intentionally keeps file-backed credentials for repeatable desktop tests; broaden later if encrypted whole-store support lands. |
| OTA firmware update | Planned | Partition table and design spec | Implement download, hash verify, inactive-slot write, rollback UX. |
| Feedback Manager | Partial | A minimal service boundary records app notification requests and exposes serial `feedback status|test` plus `app notify test` diagnostics | Centralize LED, buzzer, keyboard/display feedback, DND, priority queues, and emergency behavior. |
| OTA firmware update | Partial | `docs/tdeck-ota-manifest.md`; `partitions.csv`; serial `ota status` and `ota test`; bounded cached-manifest validator rejects bad schema, board, URL, SHA-256, and oversized binaries before any updater trusts them | Implement Wi-Fi fetch/download, binary hash verify, inactive-slot write, boot-partition switch, rollback UX, update UI, and feedback routing. |
| OTA firmware update | Planned/Partial | Partition table; OTA boot health/rollback policy selftest plus serial diagnostics | Wire manifest download, SHA256 verification, inactive-slot writes, ESP32 OTA state calls, and update UI. |
| Feedback Manager | Planned | Design spec section 8 | Centralize LED, buzzer, keyboard/display feedback and DND. |
| Emergency beacon | Planned | Design spec section 12, disabled Emergency UI row | Requires Feedback Manager and dual-network messaging. |
| BLE companion | Partial, needs validation | NimBLE-based Meshtastic GATT service, official UUIDs, raw `ToRadio` writes, queued `FromRadio` reads, `FromNum` notifications, UI toggle, and serial selftest/status | Validate with the official Meshtastic app over BLE before calling V0.5 complete. |
| CI and release checks | Partial | `.github/workflows/firmware.yml` runs native simulator build, native protocol selftest, deterministic simulator scenario, screenshot generation, T-Deck build, size reporting, an explicit firmware/static-RAM budget gate, and artifact upload with budget metadata plus screenshots; `docs/tdeck-release-checklist.md` and `scripts/release_evidence.py` define the exact-artifact COM8 evidence path | Add protocol vectors beyond the native selftest and keep expanding hardware evidence gates. |
| CI and release checks | Partial | `.github/workflows/firmware.yml` runs native simulator build, native protocol selftest, deterministic simulator scenario, screenshot generation, T-Deck build, size reporting, an explicit firmware/static-RAM budget gate, and artifact upload with budget metadata plus screenshots. The codec selftest now includes Meshtastic channel/hash, frame-boundary, and malformed protobuf guard vectors alongside existing MeshCore crypto references. | Add broader stock-device packet captures and hardware evidence gates. |
| BLE companion | Functional, needs soak | NimBLE-based Meshtastic GATT service, official UUIDs, raw `ToRadio` writes, queued `FromRadio` reads, `FromNum` notifications, UI toggle, serial selftest/status, Meshtastic-compatible firmware metadata, Android app connection/nodes/LongFast send-receive photo proof, and session counters for reconnect/drop diagnostics | Repeat reconnect/disconnect/coexistence soak before closing the V0.5 hardware checklist. |
| CI and release checks | Partial | `.github/workflows/firmware.yml` runs native simulator build, native protocol selftest, deterministic simulator scenario, screenshot generation, T-Deck build, size reporting, an explicit firmware/static-RAM budget gate, and artifact upload with budget metadata plus screenshots | Add protocol vectors beyond the native selftest and hardware evidence gates. |
| CI and release checks | Partial | `.github/workflows/firmware.yml` runs native simulator build, native protocol selftest with fixed Meshtastic header/Data protobuf vectors, deterministic simulator scenario, screenshot generation, T-Deck build, size reporting, an explicit firmware/static-RAM budget gate, and artifact upload with budget metadata plus screenshots | Add hardware evidence gates once COM8 validation is available. |

## Completion Criteria

The firmware should not be called complete until all of the following are true:

1. T-Deck build, upload, boot, and hardware smoke tests are repeatable.
2. Meshtastic public channel and DMs work with stock devices, including ACK/retry UX.
3. MeshCore public channel, rooms, DMs, ADVERTs, and TDM run alongside Meshtastic without regressions.
4. App Store installs and launches sandboxed apps from a real catalog or local app directory.
5. Optional apps cover repo-listed app goals without bloating the base OS.
6. OTA can update the firmware safely.
7. Security settings protect credentials, keys, identity, and message history.
8. Terminal/diagnostics are hidden behind Developer Mode.
9. CI and release docs prove the state on every release.
