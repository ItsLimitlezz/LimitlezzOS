# T-Deck OTA Firmware Manifest

This is the first Phase 10 OTA increment: a bounded manifest contract and
diagnostics path. It validates update metadata before any Wi-Fi downloader,
inactive-slot writer, boot-partition switch, or rollback flow trusts it.

Implemented:

- `ota status` over the USB serial console.
- `ota test` over the USB serial console.
- Native simulator selftest coverage for valid and invalid manifests.
- Cached manifest discovery from SD/local storage and the `appfs` partition.

Still TODO:

- fetch the manifest over Wi-Fi
- download the firmware binary
- verify the binary SHA-256 after download
- write to the inactive OTA slot
- set the OTA boot partition and mark the new firmware healthy
- rollback UX and failure recovery
- user-facing update screen and Feedback Manager progress routing

## Cache Paths

The firmware looks for one cached JSON manifest at the first matching path:

1. `/sd/limitlezz/ota/manifest.json`
2. `/sd/ota/manifest.json`
3. `/appfs/ota/manifest.json`

The native simulator uses the same layout under its data directory, for example
`lzdata/ota/manifest.json` and `lzdata/appfs/ota/manifest.json`.

## Schema

The manifest is a tiny top-level JSON object. The parser is intentionally
bounded: no allocation, no recursion, a maximum file size of 2 KB, and
fail-closed validation.

Required fields:

```json
{
  "schema": "limitlezz.ota_manifest.v1",
  "version": "0.97.0",
  "channel": "beta",
  "board": "tdeck",
  "firmware_url": "https://updates.example/tdeck/0.97.0/firmware.bin",
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "size": 1539920
}
```

Optional fields:

```json
{
  "min_version": "0.96.0",
  "notes_url": "https://updates.example/tdeck/0.97.0/notes"
}
```

Validation rules:

- `schema` must be `limitlezz.ota_manifest.v1`.
- `board` must be `tdeck`.
- `version`, `channel`, and `min_version` use only letters, numbers, `_`, `-`,
  and `.`.
- `firmware_url` and `notes_url` must be `http://` or `https://` URLs without
  whitespace, quotes, or backslashes.
- `sha256` must be exactly 64 hex characters.
- `size` must be greater than 0 and no larger than the T-Deck OTA slot size
  from `partitions.csv` (`0x500000`, 5,242,880 bytes).

## Serial Diagnostics

Fresh hardware with no cached manifest:

```text
lz> ota status
ota manifest: no cached manifest
```

Valid cached manifest:

```text
lz> ota status
ota manifest: valid version=0.97.0 channel=beta board=tdeck size=1539920 source=/sd/limitlezz/ota/manifest.json
firmware: https://updates.example/tdeck/0.97.0/firmware.bin
```

Built-in parser proof:

```text
lz> ota test
OTA manifest selftest: PASS valid=1 invalid_error="bad sha256"
```
