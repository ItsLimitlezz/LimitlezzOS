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

## Launcher Behavior

Accepted local apps appear after the built-in Home apps. The Home launcher keeps
the 4x2 tile grid and adds pages with page dots when more apps are available
than fit on the first screen. The App Store also lists accepted local apps as
installed local packages and opens the same manifest detail shell. Home launches
the app into the SDK 0.1 foreground shell; the detail shell also has an `OPEN`
action.

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

The current SDK 0.1 foreground shell reads only bounded display metadata from
the entry file. It accepts optional `title:`, `status:`, `body:`, or `text:`
lines, including Lua-comment style lines such as `-- body: Local dashboard`.
Script execution and API injection are still later runtime work.

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
foreground shell. Script execution, sandbox API injection, app data quotas, and
network catalog installs remain later app-platform work. Permission metadata is
parsed and displayed now so packages can fail closed before richer runtime APIs
are added, and apps that declare `storage` get a scoped `data/` directory
prepared under their own package.
