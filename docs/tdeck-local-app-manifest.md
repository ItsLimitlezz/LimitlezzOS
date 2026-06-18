# Local App Manifest

Local apps are the first V0.95 app-platform step. They are scanned from local
storage only; there is no network catalog, download, or script runtime in this
step.

## Search Paths

On T-Deck hardware, packages are discovered from:

- `/sd/limitlezz/apps/<app-id>/`
- `/sd/apps/<app-id>/`
- `/appfs/apps/<app-id>/`

In the native simulator, packages are discovered from:

- `<sim-data-dir>/apps/<app-id>/`
- `<sim-data-dir>/appfs/apps/<app-id>/` when the simulator appfs root exists

On hardware, `appfs` is the FAT flash partition from `partitions.csv`. The
firmware mounts it at `/appfs` without formatting; if the partition is absent or
unformatted, SD-backed app discovery still works and appfs apps simply do not
appear.

## Launcher Behavior

Accepted local apps appear after the built-in Home apps. The Home launcher keeps
the 4x2 tile grid and adds pages with page dots when more apps are available
than fit on the first screen. The App Store also lists accepted local apps as
installed local packages and opens the same manifest detail shell. Home launches
the app into the SDK 0.1 foreground shell; the detail shell also has an `OPEN`
action.
Closing the app from the foreground shell, or navigating back with Esc, clears
the active session while preserving the manifest selection for the detail view.

When Developer Mode is enabled, the App Store also shows rejected local package
folders with a short reason such as `missing manifest`, `unsafe id`, `bad
permissions`, or `missing entry file`. Rejected packages are not exposed on Home
and cannot be opened.

## Package Layout

Each package directory must contain:

- `manifest.json`
- the manifest `entry` file, relative to the package directory
- optional assets or per-app data files
- optional `data/` directory; for apps that declare `storage`, the firmware
  prepares this directory inside the app package before runtime storage APIs
  exist

The current SDK 0.1 foreground shell reads bounded display metadata and up to
two bounded foreground actions from the entry file. The entry metadata budget is
1 KB; larger entry files are shown as launch-blocked instead of being
truncated. It accepts optional `title:`, `status:`, `body:`, `text:`, and
`action:` lines, including Lua-comment style lines such as `-- body: Local
dashboard`. Script execution and richer API injection are still later runtime
work.

Action lines use pipe-separated fields:

```lua
-- action: Refresh | Forecast refreshed #{count} | Fresh local forecast rendered {count} times | counter:refreshes
```

The first field is the button label, the second replaces the foreground status
after activation, and the third replaces the body text. The optional fourth
field is a tiny SDK effect. The only supported effect is `counter:<safe-key>`,
which increments `<app>/data/<safe-key>.count` and expands `{count}` in the
status/body. Counter keys may contain up to 19 letters, numbers, `_`, and `-`
characters only. Unknown effects and malformed counter keys are launch-blocked
instead of being ignored.

Actions require the `input` permission; display-only apps that declare actions
are launch-blocked. Counter actions also require `storage` permission and stay
inside the scoped app `data/` directory and the 64 KB quota. Actions do not
execute arbitrary script and do not grant raw filesystem, radio, or hardware
access.

The SDK 0.1 foreground shell also supports tiny read-only value tokens in
`status:`, `body:`, `text:`, and action status/body fields:

- `{time}` expands to the current local clock string and requires
  `system_time`
- `{battery}` expands to the current device battery value and requires
  `battery`

Packages that use one of these tokens without the matching permission are
launch-blocked before the app shell opens.

Example:

```json
{
  "id": "weather.mesh",
  "name": "Weather Mesh",
  "version": "0.1.0",
  "author": "Limitless",
  "summary": "Local weather dashboard",
  "entry": "main.lua",
  "api_version": "0.1",
  "permissions": ["display", "input", "storage", "mesh_read", "system_time"],
  "icon": "weather",
  "hue": 48
}
```

## Accepted Fields

Required:

- `id`: letters, numbers, `_`, `-`, or `.`
- `name`: display name
- `entry`: relative entry file path

Optional:

- `version`: shown in App Store and app detail, defaults to `0.0.0`
- `author`: shown in App Store and app detail, defaults to `local`
- `api_version`: SDK compatibility gate, defaults to `0.1`; unsupported values
  are rejected
- `permissions`: array of API namespace names, defaults to `["display",
  "input"]` for simple legacy manifests
- `summary` or `description`: short display text
- `icon`: token such as `calculate`, `note`, `weather`, `map`, `game`,
  `terminal`, `folder`, or `description`
- `hue`: tile hue hint; unsupported values fall back to the neutral tile color

Supported permission names:

- `display`
- `input`
- `storage`
- `mesh_read`
- `mesh_send`
- `system_time`
- `battery`
- `notifications`
- `network_wifi`

## Safety Rules

The scanner rejects packages when:

- `manifest.json` is missing, empty, or larger than the bounded parser accepts
- required fields are missing
- `id` contains path separators or unsafe characters
- `entry` is absolute, contains `..`, contains a Windows drive separator, or is
  missing on disk
- `api_version` names an unsupported SDK version
- `permissions` is not an array of supported namespace strings

The current firmware scans local app manifests and can open them in a safe
foreground shell with bounded foreground actions, including a storage-scoped
counter effect and read-only `{time}` / `{battery}` value injection. Script
execution, richer sandbox API injection, richer data APIs, and network catalog
installs remain later app-platform work. Permission metadata is parsed and
displayed now so packages can fail closed before richer runtime APIs are added,
and apps that declare `storage` get a scoped `data/` directory prepared under
their own package.

Storage-enabled local apps have a 64 KB `data/` quota in this early shell. The
App Store detail and foreground shell show current usage, and over-quota apps
are launch-blocked before any future runtime code can run. The App Store detail
screen also provides `Clear local data` for storage-enabled apps; it removes
only files and folders inside that app's scoped `data/` directory and then
recreates the directory for later use.

## Sample App Pack

The repository includes copyable SDK 0.1 packages in `examples/local-apps/`:

- Calculator
- Field Notes
- Offline Maps
- Weather Mesh
- Mesh BBS
- Signal Scope
- LoRa Chess
- APRS Bridge

Validate the pack before copying it to a simulator or card-style root:

```sh
python scripts/validate_local_app_samples.py
```

For local simulator/data-root testing, install the samples under an app root:

```sh
python scripts/validate_local_app_samples.py --install-root .pio/local-app-samples --clean
```

Then copy `.pio/local-app-samples/apps/<sample>` folders to `/sd/limitlezz/apps/`,
`/sd/apps/`, or `/appfs/apps/` on hardware. The validator mirrors the firmware's
SDK 0.1 limits for manifest size, string fields, safe entry paths, supported
permissions, `{time}` / `{battery}` token permissions, foreground action count,
and storage-scoped counter effects.
