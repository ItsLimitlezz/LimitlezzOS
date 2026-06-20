# T-Deck App Catalog Schema

This is the first Network App Store increment. It defines and validates the
future `index.json` shape, but it does not fetch, download, install, update, or
uninstall packages yet.

Catalog indexes are expected at:

- `/sd/limitlezz/catalog/index.json`
- `/appfs/catalog/index.json`

The index must fit in `4096` bytes and use this top-level shape:

```json
{
  "schema": "limitlezz.app_catalog.v1",
  "updated": "2026-06-18T00:00:00Z",
  "apps": []
}
```

Each app entry is bounded and fail-closed:

```json
{
  "id": "weather.mesh",
  "name": "Weather Mesh",
  "version": "0.1.0",
  "author": "Limitless",
  "description": "Local weather reports",
  "icon": "weather",
  "hue": 48,
  "api_version": "0.1",
  "compatibility": "tdeck",
  "permissions": ["display", "network_wifi"],
  "download_url": "https://apps.example.invalid/weather.mesh.zip",
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "size": 32768,
  "screenshots": ["https://apps.example.invalid/weather.bmp"]
}
```

Validation rules:

- `id` uses the same safe token rules as local app manifests.
- `api_version` must be supported by the local SDK compatibility gate.
- `permissions` must use the existing allowlist.
- `download_url` and optional `screenshots` must be `http://` or `https://`
  URLs without whitespace or control characters.
- `sha256` must be exactly 64 hex characters.
- `size` must be nonzero and no more than `2 MB` for this first package path.
- `hue`, if present, must be `-1` or `0..359`.
- The catalog can list up to `24` apps.

Serial diagnostics:

```text
app catalog status
app catalog test
```

`app catalog status` validates a cached index if one exists and otherwise
reports that no cached catalog is present. `app catalog test` runs a built-in
valid/invalid schema selftest so hardware smoke can prove the parser without
requiring Wi-Fi or SD setup.
