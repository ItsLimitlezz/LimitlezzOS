# T-Deck App Catalog Schema

This is the first Network App Store increment. It defines and validates the
`index.json` shape and can refresh a bounded catalog cache over Wi-Fi, but it
does not download, install, update, or uninstall packages yet.

Catalog indexes can be loaded from:

- `/sd/limitlezz/catalog/index.json`
- `/appfs/catalog/index.json`
- the refreshed cache file, `app_catalog.json`, written by `app catalog fetch`

The index must fit in `4096` bytes and use this top-level shape:

```json
{
  "schema": "limitlezz.app.catalog.v1",
  "generated_at": "2026-06-18T00:00:00Z",
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
  "summary": "Local weather dashboard",
  "description": "Local weather reports",
  "icon": "weather",
  "hue": 48,
  "api_version": "0.1",
  "permissions": ["display", "network_wifi"],
  "package_url": "https://apps.example.invalid/weather.mesh.zip",
  "package_sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "package_bytes": 32768,
  "compatibility": {
    "min_os": "0.95.0",
    "api_versions": ["0.1"],
    "targets": ["tdeck", "sim"]
  },
  "screenshots": [
    {
      "url": "https://apps.example.invalid/weather.bmp",
      "width": 320,
      "height": 240,
      "sha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
    }
  ]
}
```

Validation rules:

- `id` uses the same safe token rules as local app manifests.
- `api_version` must be supported by the local SDK compatibility gate.
- `permissions` must use the existing allowlist.
- `package_url` and screenshot URLs must be HTTPS in published catalogs.
- `package_sha256` must be exactly 64 lowercase hex characters.
- `package_bytes` must be nonzero and no more than `2 MB` for this first
  package path.
- `compatibility.api_versions` must include the entry's `api_version`, and
  `compatibility.targets` must include `tdeck` or `sim`.
- `hue`, if present, must be `-1` or `0..359`.
- The catalog can list up to `24` apps.

Serial diagnostics:

```text
app catalog status
app catalog test
app catalog fetch https://example.invalid/limitlezz/catalog/index.json
app catalog clear
```

`app catalog fetch` downloads bounded JSON over Wi-Fi, validates it, and only
then writes the refreshed cache. `app catalog status` validates that refreshed
cache first, then legacy `/catalog/index.json` files if present. `app catalog
test` runs a built-in valid/invalid schema selftest so hardware smoke can prove
the parser without requiring Wi-Fi or SD setup.
