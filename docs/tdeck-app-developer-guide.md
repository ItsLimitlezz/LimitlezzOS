# T-Deck App Developer Guide

This guide explains how to build local LimitlezzOS apps for the current SDK
0.1 foreground shell. It documents what works today. Network catalog installs,
arbitrary script execution, richer Lua APIs, background tasks, and direct mesh
send/receive APIs are still roadmap work.

For the lower-level manifest reference, see
[`docs/tdeck-local-app-manifest.md`](tdeck-local-app-manifest.md).

## Current App Model

SDK 0.1 apps are local packages discovered from SD or appfs. Accepted packages
appear in Home and App Store, then launch into a safe foreground shell.

Current guarantees:

- apps are foreground-only
- manifests and entry files are bounded before launch
- permissions are parsed fail-closed
- storage is scoped to the app package
- app data has an early 64 KB quota
- entry text can display bounded status/body content
- up to two foreground actions can update the session
- the only write effect is a scoped `counter:<key>` file
- `{time}` and `{battery}` are read-only tokens gated by permissions

Not implemented yet:

- arbitrary Lua/script execution
- background tasks
- network catalog download/update
- app package signatures or SHA verification
- mesh send/receive APIs
- notifications API behavior
- raw hardware, radio, filesystem, or kernel access

## Package Locations

On T-Deck hardware, place packages in one of:

```text
/sd/limitlezz/apps/<app-id>/
/sd/apps/<app-id>/
/appfs/apps/<app-id>/
```

In the native simulator, packages are discovered from:

```text
<sim-data-dir>/apps/<app-id>/
<sim-data-dir>/appfs/apps/<app-id>/
```

Each package must contain:

```text
manifest.json
<entry file named by manifest.json>
data/              optional; prepared automatically for storage apps
assets/            optional; reserved for later richer runtimes
```

## Minimal App

Create a package directory:

```text
weather.mesh/
  manifest.json
  main.lua
```

`manifest.json`:

```json
{
  "id": "weather.mesh",
  "name": "Weather Mesh",
  "version": "0.1.0",
  "author": "Limitless",
  "summary": "Local weather dashboard",
  "entry": "main.lua",
  "api_version": "0.1",
  "permissions": ["display", "input", "storage", "system_time", "battery"],
  "icon": "weather",
  "hue": 48
}
```

`main.lua`:

```lua
-- title: Weather Mesh
-- status: Updated at {time}
-- body: Battery {battery}. Forecast refreshed {count} times.
-- action: Refresh | Refreshed #{count} | Local forecast refreshed {count} times at {time}. | counter:refreshes
```

The entry file is read as metadata today. The Lua-looking comment style is only
a convenient format; the firmware does not execute Lua code in SDK 0.1.

## Manifest Fields

Required fields:

- `id`: safe package id containing letters, numbers, `_`, `-`, or `.`
- `name`: display name
- `entry`: relative path to the package entry file

Optional fields:

- `version`: shown in app detail, defaults to `0.0.0`
- `author`: shown in app detail, defaults to `local`
- `summary` or `description`: short display text
- `api_version`: compatibility gate, defaults to `0.1`
- `permissions`: supported permission names, defaults to `display` and `input`
- `icon`: token such as `calculate`, `note`, `weather`, `map`, `game`,
  `terminal`, `folder`, or `description`
- `hue`: tile color hint; unsupported values fall back to neutral styling

Unsupported SDK versions and unknown permissions reject the package before it
appears on Home.

## Permissions

Supported permission names:

- `display`: required to show foreground content
- `input`: required for app-provided actions
- `storage`: prepares scoped `data/` and enables counter actions
- `mesh_read`: reserved for future mesh read APIs
- `mesh_send`: reserved for future mesh send APIs
- `system_time`: enables `{time}` token expansion
- `battery`: enables `{battery}` token expansion
- `notifications`: reserved for future notification APIs
- `network_wifi`: reserved for future Wi-Fi/network APIs

Permission rules:

- An app that declares actions without `input` is launch-blocked.
- A counter action without `storage` is launch-blocked.
- `{time}` without `system_time` is launch-blocked.
- `{battery}` without `battery` is launch-blocked.
- Unknown permissions are rejected at scan time.

Declare only the namespaces the app needs. Future runtimes will use the same
principle: undeclared APIs should be absent from the app environment.

## Entry Metadata

The SDK 0.1 foreground shell reads these entry lines:

```text
title: <short title>
status: <short status>
body: <main body text>
text: <fallback body text>
action: <label> | <status after press> | <body after press> | <optional effect>
```

Lua-comment style is accepted:

```lua
-- body: Local dashboard
```

Rules:

- The entry metadata budget is 1 KB.
- Larger entry files are launch-blocked.
- At most two actions are exposed.
- Missing body text falls back to a generic safe-shell message.
- Action fields are separated with `|`.
- Action text can include `{time}`, `{battery}`, and `{count}` when the
  relevant permissions/effect are present.

## Foreground Actions

Action format:

```text
action: Label | New status | New body | counter:safe_key
```

The first field is the button label. The second replaces the foreground status
after activation. The third replaces the body text. The fourth is optional.

Supported effect:

- `counter:<safe-key>` increments `<package>/data/<safe-key>.count`

Counter key rules:

- up to 19 characters
- letters, numbers, `_`, and `-` only
- no path separators
- no `..`

Unknown effects such as `radio:raw` and malformed counter keys are
launch-blocked instead of ignored.

## Storage

Apps that declare `storage` get a scoped `data/` directory inside their package.
Current storage behavior is intentionally narrow:

- `data/` is created inside the app package
- the foreground shell reports usage
- the quota is 64 KB
- over-quota apps are launch-blocked
- App Store detail can clear only that app's `data/` directory
- counter actions write only safe `<key>.count` files under `data/`

Do not depend on file layouts outside `data/`. Future SDKs may add richer
storage APIs while preserving the isolation rule.

## Rejection Reasons

Developer Mode App Store diagnostics can show rejected packages. Common reasons
include:

- `missing manifest`
- `empty manifest`
- `manifest too large`
- `missing id`
- `missing name`
- `missing entry`
- `unsafe id`
- `unsafe entry`
- `unsupported SDK`
- `bad permissions`
- `missing entry file`
- launch errors such as `bad action effect`, `unsupported action effect`,
  `missing input permission`, `missing storage permission`, or `app data over quota`

Rejected packages do not appear on Home and cannot be opened.

## Testing Workflow

For quick local checks:

```sh
pio run -e native
.pio/build/native/program --selftest
.pio/build/native/program --simtest
```

The native selftest creates sample local app packages and checks:

- valid manifest scanning
- unsafe id rejection
- bad permission rejection
- storage sandbox preparation
- quota reporting and clearing
- foreground launch metadata
- foreground actions
- counter persistence
- over-quota blocking
- token permission gating
- appfs-only discovery

For release or PR validation, use the project workflow:

1. Run the native checks locally.
2. Push the branch.
3. Wait for `Firmware CI`.
4. Download the exact Actions artifact.
5. Flash and smoke-test the T-Deck on COM8.

## Design Guidelines

- Keep app names short enough for a 320x240 screen.
- Treat body text as glanceable status, not a scrolling document.
- Keep actions obvious and reversible.
- Prefer one or two meaningful actions over a menu.
- Do not claim mesh, notification, catalog, or background behavior until those
  APIs exist in firmware.
- Keep user data under the app's scoped `data/` directory.
- Make failures explicit through status/body text.

## Compatibility Notes

SDK `0.1` is intentionally small so package metadata and permissions can settle
before a richer runtime lands. Build apps so they fail closed:

- include `api_version`
- include explicit permissions
- avoid relying on unspecified parser behavior
- keep entry files small
- keep package IDs stable
- treat reserved permissions as declarations for future APIs, not current
  capabilities
