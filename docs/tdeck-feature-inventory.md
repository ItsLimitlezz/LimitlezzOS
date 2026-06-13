# T-Deck Firmware Feature Inventory

Audit date: 2026-06-13

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
| T-Deck PlatformIO firmware target | Functional, needs validation | `platformio.ini`, `src/main_tdeck.cpp`; local build emitted `firmware.bin` | Clean up Windows build workflow and verify board profile against actual T-Deck flash/PSRAM. |
| OTA-ready partition layout | Partial | `partitions.csv` has `ota_0`, `ota_1`, `otadata`, `config`, `appfs` | OTA service and update UI are not implemented. |
| Display and LVGL shell | Functional, needs validation | LovyanGFX ST7789 setup, LVGL buffers, UI screens | Hardware flash/smoke checklist needed for every release. |
| Trackball and keyboard input | Functional, needs validation | GPIO interrupts and I2C keyboard polling in `main_tdeck.cpp` | Add hardware test checklist and input regression tests in simulator. |
| Touch and calibration | Functional, needs validation | GT911 driver and `touch.cfg` persistence | Add explicit touch calibration validation after flash. |
| SD-backed data directory | Functional, needs validation | SD mount and `/sd/limitlezz` store | Add corruption/power-loss tests and storage quota handling. |
| Battery/system telemetry | Partial | ADC battery reading, system screen stats | Charge state is inferred/limited; no dedicated charge-detect line. |
| Keyboard/display backlight | Functional, needs validation | LEDC display backlight and I2C keyboard PWM | Fold keyboard/display/buzzer/LED into Feedback Manager later. |
| Power saving/sleep | Partial | idle dim/lock and CPU down-clock | Needs hardware current measurements and background receive verification. |

## Meshtastic

| Feature | Status | Evidence | Gap / Next Action |
| --- | --- | --- | --- |
| LongFast public channel | Functional, needs validation | `mtproto.*`, `backend_sx1262.cpp`, channel thread | Add interop test notes with stock Meshtastic devices. |
| Channel send/receive | Functional, needs validation | `lz_svc_channel_thread`, `lz_core_on_text`, `lz_backend_send` | Add delivery/error handling for TX failures. |
| Node discovery | Functional, needs validation | NodeInfo parse, heard-node table, 250-node cap | Add role/hardware enum coverage beyond current minimal parse. |
| Node table persistence | Functional | `nodes.db` save/load | Add versioning/migration for future schema changes. |
| Direct messages | Functional, needs validation | DM threads, `lz_svc_send_text`, PKI path | Persist delivery state and queued-send state. |
| PKI encrypted DMs | Functional, needs validation | `mtpki.cpp`, NodeInfo public key capture | Add test vectors or a deterministic host test. |
| Routing ACK/delivery status | Partial | `send_routing_ack`, `lz_core_on_ack`, bubble status colors | ACK timeout/status is RAM-tail only and not persisted. |
| Retransmit/resend | Partial | Long-press failed bubble calls `lz_svc_resend` | No persisted retry queue; immediate backend failures not reflected. |
| Managed flood rebroadcast | Functional, needs validation | `rebroadcast` in `backend_sx1262.cpp` | Needs airtime/backoff validation in busy meshes. |
| USB companion bridge | Functional, needs validation | `mt_companion.cpp`, serial commands, UI toggle | Config coverage is minimal; Meshtastic BLE companion is V0.5. |
| Position/telemetry decode | Planned | README lists as still ahead | Needed for maps, weather, and richer node detail. |
| Emergency channel/beacon | Prototype/Planned | Emergency row appears but is disabled; design spec covers SOS | Implement after feedback manager and dual-network send. |

## MeshCore

| Feature | Status | Evidence | Gap / Next Action |
| --- | --- | --- | --- |
| MeshCore compile-time gate | Partial | `#define LZ_MESHCORE_ENABLED 0` | Gate should stay off until receive/send/unified inbox tests pass. |
| TDM radio scheduler | Partial, needs validation | `lz_backend_set_networks`, profile switcher, settings airtime bar | Needs hardware soak and latency/packet-loss measurements. |
| MeshCore RF profile | Partial, needs validation | 910.525 MHz / 62.5 kHz / SF7 / CR4/5 profile | Confirm target regions and RF compatibility with real MeshCore devices. |
| MeshCore ADVERT RX | Partial, needs validation | `mc_parse`, `mc_advert_decode`, `lz_core_on_mc_node` | Only ADVERTs are decoded; encrypted payloads are ignored. |
| MeshCore self-advert TX | Partial, needs validation | Ed25519 identity, self-advert builder, serial/UI advert commands | Needs interop proof with real MeshCore nodes. |
| MeshCore public channel / rooms | Planned | README says receive/default Public channel still ahead | V0.6: implement group text decode/send, room model, and split airtime config. |
| MeshCore DMs | Planned | MeshCore contacts are non-messageable while gated | V0.7: implement key/session model, send path, ACKs, and UI routing. |
| MeshCore companion bridge | Planned | README lists as later | V0.8: build MeshCore USB and BLE companions after MeshCore messaging is stable. |

## User Interface

| Feature | Status | Evidence | Gap / Next Action |
| --- | --- | --- | --- |
| First-boot onboarding | Functional | `scr_onboard.c`, identity persistence | MeshCore network row is visible but locked. |
| Lock screen | Functional | clock, battery, network icons, notification card | Add per-network badges once MeshCore is active. |
| Home launcher | Partial | 4x2 app grid | V0.95: support added apps and multiple Home screens; Terminal should move behind Developer Mode. |
| Unified inbox | Functional/Partial | Messages tabs, filters, unread badges, channel tab | MeshCore filter is gated; unread icon badge is roadmap. |
| Conversation view | Functional/Partial | compose, bubbles, status colors, resend long-press | Long drafts handled, but delivery persistence is incomplete. |
| Meshtastic manager | Functional/Partial | identity card, virtualized node list, channels tab, companion toggle | Emergency channel row is disabled. |
| MeshCore manager | Prototype/Partial | "Coming soon" unless gate is flipped; deeper screen exists behind gate | Do not enable until MeshCore message path works. |
| Contacts/detail | Functional/Partial | contacts list, add contact, messageable role check | Trace action is a no-op; MeshCore contacts locked. |
| Settings | Functional/Partial | network toggles, Wi-Fi, brightness, time, system, touch calibration | Most settings are not persisted. |
| Wi-Fi setup | Functional, needs validation | async scan/connect, saved SSID/password, auto-connect | Credentials are plaintext on SD; only one saved network. |
| System/battery page | Functional/Partial | live stats and battery arc | Hardware values need calibration/validation. |
| App Store | Prototype | `LZ_STORE` static catalog and timer-based GET -> OPEN | Disabled from Home; no catalog, download, install, manifest, or launch. |
| Terminal | Functional/Partial | interactive UI terminal and serial CLI | Should move behind Developer Mode. |
| Files | Prototype | static `LZ_FILES` rows | Needs real SD/appfs listing and navigation. |

## App Platform And Ecosystem

| Feature | Status | Evidence | Gap / Next Action |
| --- | --- | --- | --- |
| Lua sandbox | Planned | Design spec section 9 | Choose Lua/eLua/minimal interpreter after memory profiling. |
| App manifest | Planned | Design spec manifest schema | Define stable manifest format and validator. |
| App permissions | Planned | Design spec API namespaces | Implement least-privilege API injection. |
| Local app scanner | Planned | `appfs` partition and design spec | V0.95: first useful App SDK milestone is scanning `/apps` on SD/appfs. |
| Network app catalog | Planned | Wi-Fi service notes; design spec | Fetch `index.json`, verify TLS/metadata, cache results. |
| App download/install/update | Planned | App Store prototype only | SHA256 verify, extract, version updates, rollback failed installs. |
| Optional map app | Planned | Store data includes maps; maintainer notes prefer maps as optional | Keep maps out of the base firmware. |
| APRS/weather/BBS/scope/game apps | Planned/Prototype catalog entries | Static `LZ_STORE` rows | Implement as sandboxed apps once runtime exists. |

## Security, Updates, And Feedback

| Feature | Status | Evidence | Gap / Next Action |
| --- | --- | --- | --- |
| Device PIN/password | Planned | README later/security section | Needed before encrypted local data UX. |
| Encrypted local store | Planned | README hardening section | Encrypt messages, keys, identity, and app data when password is set. |
| Wi-Fi credential hardening | Planned | `wifi.cfg` plaintext in `store.c` | V0.96: move to NVS or encrypted store with migration. |
| OTA firmware update | Planned | Partition table and design spec | Implement download, hash verify, inactive-slot write, rollback UX. |
| Feedback Manager | Planned | Design spec section 8 | Centralize LED, buzzer, keyboard/display feedback and DND. |
| Emergency beacon | Planned | Design spec section 12, disabled Emergency UI row | Requires Feedback Manager and dual-network messaging. |
| BLE companion | Planned | Author V0.5 beta milestone | Add Meshtastic BLE companion after USB companion and core Meshtastic messaging are stable. |
| CI and release checks | Planned | No `.github` workflows | Add tdeck build, simulator/selftest where available, artifact/size reporting. |

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
