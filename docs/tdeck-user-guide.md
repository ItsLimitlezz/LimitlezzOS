# T-Deck User Guide

This guide is for people using LimitlezzOS on a LilyGO T-Deck or T-Deck Plus.
It focuses on the normal on-device experience and avoids Developer Mode unless
you need diagnostics.

## What LimitlezzOS Is For

LimitlezzOS is a handheld mesh OS for the T-Deck. The main experience is:

- send and receive Meshtastic LongFast channel messages
- send and receive encrypted Meshtastic direct messages
- keep a unified inbox with clear network labels
- use the trackball, keyboard, and touch screen without a phone
- optionally bridge to companion apps over USB or BLE when those paths are
  enabled

Some roadmap features are still partial or planned. The README and firmware
roadmap are the authority for current release status.

## Controls

- Trackball roll: move focus or scroll lists.
- Trackball click: select the focused item.
- Touch: tap rows, tabs, buttons, and toggles.
- Keyboard: type in message composers and text fields.
- Enter: send or confirm when the composer/action expects it.
- Backspace or Escape in the simulator: go back.
- `sym + L`: lock the device from most screens.

The interface is designed so every common workflow can be completed with the
trackball and keyboard.

## First Boot

On first boot, LimitlezzOS asks for:

1. Long name: the name other mesh users should see.
2. Short tag: a compact Meshtastic short name, usually four characters.
3. Network choice: Meshtastic is the beginner-safe default path.

After onboarding, the device opens to the lock screen. Future boots skip
onboarding unless local identity storage is reset.

## Lock Screen And Home

The lock screen shows time, battery, active network indicators, and unread
message notifications.

- Wake once with touch, key, or trackball input.
- Unlock with a second input.
- Tap a notification to open the relevant conversation.
- Multiple unread threads show a combined notification count.

Home uses a paged launcher grid. Built-in apps and accepted local apps appear as
tiles. Terminal is hidden until Developer Mode is enabled.

## Messages

Messages is the main inbox.

- Direct threads and channel threads are separated by tabs.
- Network filters let you view all messages, Meshtastic, or MeshCore when
  enabled.
- Unread conversations are highlighted.
- The Home Messages tile shows a badge for unread conversations.
- Long-press a conversation to mute or unmute it.

Muted conversations keep history but do not contribute to the lock-screen or
Home unread badges.

## Sending A Channel Message

1. Open Messages.
2. Choose the channel thread, such as LongFast.
3. Type with the keyboard.
4. Press Enter or the focused send action.

If sending fails, the message state and diagnostics should make the failure
visible instead of silently losing the message.

## Sending A Direct Message

1. Open Messages or Contacts.
2. Choose a contact that is messageable on the active network.
3. Type your message.
4. Send from the composer.

Sent direct messages can show sending, delivered, failed, retry, and failure
reason states. Long-press failed messages to retry when that action is
available.

## Contacts And Nodes

The Meshtastic manager shows heard nodes, signal hints, and node details.
Contacts are people you deliberately add, not every node ever heard.

Use Contacts when you want a stable person-centric list. Use Meshtastic Nodes
when you are inspecting live mesh presence and diagnostics.

## Wi-Fi

Open Settings, then Wi-Fi, to scan and join a network.

- The device remembers one network.
- Auto-connect can rejoin the saved network.
- Forget removes the saved network so you can enter a new password.
- On T-Deck hardware, saved Wi-Fi credentials use the hardware credential
  backend reported by serial diagnostics as `cred=nvs`.

Wi-Fi and BLE share scarce ESP32-S3 resources. If a BLE companion workflow is
being tested, keep Wi-Fi expectations modest and test one transport at a time.

## Time, Display, And Sleep

Settings includes common device preferences:

- brightness
- sleep timeout
- keyboard light mode
- time zone
- 12-hour or 24-hour clock
- TX power
- network toggles
- power saving

Sleep is two-step: the first input wakes the screen while staying locked, and a
second input unlocks. This helps prevent accidental pocket interaction.

## Files And Storage

Files is a read-only browser for mounted storage roots.

- SD/local storage holds user data such as identity, messages, settings, and
  local apps when an SD card is present.
- Appfs is a separate flash-backed app partition when mounted.
- If both are present, Files starts at a storage root picker.
- Without SD, the OS can still run, but persistence tests should not be counted
  as passing.

## Local Apps

Local apps can be copied to supported app directories such as SD or appfs app
roots. Accepted app manifests appear in Home and App Store views.

Current local app support is intentionally limited:

- app metadata and permissions are validated before launch
- apps run in a foreground shell
- storage is scoped to the app
- unsupported actions fail closed
- rejected packages can be inspected in Developer Mode diagnostics

Network catalog install/update, richer script execution, and the full runtime
API are still roadmap work.

## Companion Modes

Meshtastic companion mode lets an external app use the T-Deck radio through the
firmware bridge.

- USB companion mode is the hardware-tested companion path.
- BLE companion firmware support exists, but phone app validation is still a
  tracked gap in the roadmap.
- Only one external companion transport should own the bridge at a time.
- Leaving companion mode should return the serial console for diagnostics.

## Developer Mode

Developer Mode reveals Terminal and extra diagnostics. Normal users should not
need it for everyday messaging.

Use Developer Mode when you need to inspect:

- serial-style diagnostics on device
- rejected local app packages
- advanced state while testing a PR or release candidate

Turn Developer Mode off again to return Home to the simpler consumer layout.

## Basic Recovery

If something looks wrong:

1. Reboot the device.
2. Confirm the battery and USB power are stable.
3. Check whether the SD card is present if identity, settings, messages, or
   apps disappeared.
4. Check Settings for network toggles and Wi-Fi state.
5. Use Developer Mode or USB serial diagnostics only when the normal UI does not
   explain the issue.

For build, flash, boot, radio, companion, or storage diagnostics, use the
troubleshooting guide once it is present in your branch or release docs.

## What To Report

When reporting a problem, include:

- device model: T-Deck or T-Deck Plus
- firmware branch and commit if known
- whether an SD card was installed
- what screen you were on
- what you expected to happen
- what actually happened
- whether the issue repeats after reboot
- any serial `id`, `sys`, `net`, `rf`, `stats`, `wifi`, or `companion test`
  output available from the maintainer/debug workflow
