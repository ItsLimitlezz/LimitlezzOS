# T-Deck App Catalog Schema

This page is kept as a compatibility pointer for older roadmap links.

The canonical network App Store contract is now:

- `docs/tdeck-network-app-catalog.md`
- `docs/examples/app-catalog-index.json`
- `scripts/validate_app_catalog.py`

Package archives:

- The first firmware-native archive path is a `.zip` using ZIP method `0`
  only, meaning stored/uncompressed entries.
- Whole-package `sha256` and exact byte size must match before extraction.
- The package must include root `manifest.json`; the embedded manifest `id`
  must match the requested install id.
- File names must be relative, must not contain `..`, backslashes, colons,
  absolute roots, hidden path segments, or a top-level `data/` tree.
- Each file is capped at `256 KB`, each package at `2 MB`, and each package at
  `24` files.
- Extraction happens into a hidden staging directory, then promotion validates
  the manifest and rolls back on failure.

Serial diagnostics:
Firmware validates and loads the canonical `limitlezz.app.catalog.v1` schema.
The lower-level package transaction can install a verified package file already
present on SD/appfs.

```text
app catalog status
app catalog test
app catalog install-test
app catalog install <id> [package_path]
app package test
app package install <id> <path> <sha256> <bytes>
```

`app catalog status` validates a cached index if one exists and otherwise
reports that no cached catalog is present. `app catalog test` runs a built-in
valid/invalid schema selftest so hardware smoke can prove the parser without
requiring Wi-Fi or SD setup. `app catalog install-test` proves catalog metadata
drives install/update and that a bad catalog hash preserves the prior app.
`app package test` creates and installs a small stored-ZIP package on-device and
proves hash mismatch, id mismatch, unsafe path, unsupported compression,
rollback, and update behavior.
