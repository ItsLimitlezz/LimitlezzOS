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
- Record the firmware artifact path, size, and timestamp.
- Confirm a native simulator sanity pass with `.pio\build\native\program.exe --selftest`.
- Confirm the SD card is mounted or deliberately test RAM-only behavior.
- Confirm peer devices are on the intended Meshtastic channel and can exchange messages with each other.

## Flash And Boot Evidence

- Flash with `pio run -e tdeck -t upload`.
- If the normal upload path is flaky on the local T-Deck/host pair, fall back to
  the known reliable Windows direct-flash path with `esptool.py --no-stub`.
- Open the USB console at 115200 baud.
- Capture the boot banner and every `[ok]` or failure line.
- Confirm display, touch, keyboard, trackball, SD, SX1262, Wi-Fi state, battery, and time source are reported.
- Run `help` and confirm diagnostics include `dm status`, `rxlog`, `nodes`, `net`, `rf`, and `companion`.

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

