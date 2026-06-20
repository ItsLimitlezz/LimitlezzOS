# Network App Catalog

This is the V0.95/V0.96 bridge contract for turning the App Store from local
manifest scanning into a downloadable catalog. Firmware now validates cached
catalog rows, renders them in App Store, and can install or update a catalog
entry through the verified stored-ZIP package transaction.

The catalog is intentionally stricter than a web store feed. Every app entry
must expose enough metadata for the T-Deck to show permissions, check SDK
compatibility, verify the downloaded package, and fail closed before replacing
anything on local storage.

## Index Shape

Top-level fields:

- `schema`: required, currently `limitlezz.app.catalog.v1`
- `generated_at`: optional ISO-style timestamp string for diagnostics
- `apps`: required array of app entries

Each app entry must include:

- `id`: safe package id, matching the local manifest id rules; 23 characters
  or fewer, using letters, numbers, `_`, `-`, or `.`
- `name`: display name, 31 characters or fewer
- `version`: compact package version, 15 characters or fewer
- `author`: display author, 27 characters or fewer
- `summary`: one-line store summary, 71 characters or fewer
- `description`: longer store description, 240 characters or fewer
- `api_version`: SDK gate, currently `0.1` or `0.1.0`
- `permissions`: non-empty array using the same names as local manifests
- `icon`: symbolic icon token, 19 characters or fewer
- `hue`: tile hue hint, `-1` for neutral or `0..359`
- `package_url`: HTTPS URL for the app package
- `package_sha256`: lowercase 64-character SHA256 digest of the package
- `package_bytes`: positive package size in bytes, no more than `2 MB` for
  the first firmware install path
- `compatibility`: object with `api_versions`, `targets`, and optional `min_os`
- `screenshots`: optional array of up to `4` HTTPS screenshot metadata objects

The firmware should treat unknown fields as forward-compatible display metadata
only after the required fields pass validation. Required-field failures,
unsupported permissions, unsupported SDKs, unsafe ids, non-HTTPS package URLs,
or malformed hashes must reject the entry before it reaches the user-facing
install path.

## Permissions

Catalog permissions use the existing local manifest namespace list:

- `display`
- `input`
- `storage`
- `mesh_read`
- `mesh_send`
- `system_time`
- `battery`
- `notifications`
- `network_wifi`

The catalog validator requires `display` so every downloadable app can render a
foreground shell. Additional runtime API injection remains gated by the same
least-privilege permission checks as local packages.

## Compatibility

`compatibility.api_versions` lists SDK versions the package supports and must
include the entry's `api_version`. For the first network catalog step, accepted
values are `0.1` and `0.1.0`.

`compatibility.targets` lists supported runtime targets:

- `tdeck`
- `sim`

`compatibility.min_os` is optional until releases have a stable public version
field, but when present it must be a compact version string such as `0.95.0`.

## Package Rules

The package URL must be HTTPS. On T-Deck builds, catalog installs stream the
package URL to a bounded temp file under local storage when Wi-Fi is connected;
serial diagnostics may also provide an already-copied package path for
deterministic smoke tests. In both cases the installer verifies
`package_sha256` and `package_bytes`, extracts a stored-only `.zip` package into
a hidden staging directory, validates its embedded `manifest.json` with the
local manifest rules, and then atomically promotes the staged package into the
app directory. Any download, hash, manifest, unsupported compression, unsafe
path, or extraction error leaves the previous installed package intact.

The first firmware package subset deliberately avoids a decompressor: ZIP
method `0` is accepted, and compressed ZIP entries are rejected until flash/RAM
budget work says otherwise. Packages may contain up to 24 files, each file may
be up to 256 KB, and the complete archive must fit in the catalog's 2 MB
package limit. Packages must not ship a top-level `data/` tree; runtime data is
prepared by the OS for apps that request the `storage` permission.

## Validation

Run the offline validator before publishing a catalog:

```sh
python scripts/validate_app_catalog.py docs/examples/app-catalog-index.json
```

Firmware CI runs the same validator against the checked-in example catalog so
schema drift is caught with the normal simulator and T-Deck build gates.

Serial hardware diagnostics:

```text
app catalog status
app catalog test
app catalog install-test
app catalog install <id> [package_path]
```

`app catalog install-test` creates a synthetic catalog and stored-ZIP package on
device, proves install/update, and proves a bad catalog hash preserves the
previous live app.
