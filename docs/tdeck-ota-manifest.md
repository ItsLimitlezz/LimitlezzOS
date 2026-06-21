# T-Deck OTA Firmware Manifest

This Phase 10 OTA slice defines a bounded manifest contract, a verified
candidate firmware cache, and an inactive-slot writer. It validates update
metadata and candidate binaries before any boot-partition switch or rollback
flow trusts them.

Implemented:

- `ota status` over the USB serial console.
- `ota fetch` over the USB serial console. This downloads the cached
  manifest's `firmware_url` over Wi-Fi, writes a bounded temporary file, verifies
  exact size and SHA-256, then atomically promotes it to the OTA cache.
- `ota stage <path>` over the USB serial console. This verifies a local
  candidate file against the cached manifest and promotes it to the same cache.
- `ota clear` over the USB serial console.
- `ota write` over the USB serial console. This writes the verified candidate
  cache into the inactive OTA slot and leaves the boot partition unchanged.
- `ota write-test` over the USB serial console. This copies the currently
  running valid firmware image into the inactive OTA slot as a hardware smoke
  path, also leaving the boot partition unchanged.
- `ota slot-status` over the USB serial console. This reports the running,
  configured boot, and inactive OTA partitions plus OTA image state when the
  bootloader exposes it.
- `ota set-test-boot` over the USB serial console. This copies the currently
  running image into the inactive slot, selects that inactive slot for the next
  boot, and returns without rebooting.
- `ota mark-valid` over the USB serial console. This calls the ESP-IDF
  mark-valid API for the currently running app and reports slot status.
- `ota test` over the USB serial console.
- Native simulator selftest coverage for valid/invalid manifests, candidate
  staging, inactive-slot writer dispatch, size mismatch rejection,
  prior-candidate preservation, and clearing.
- Cached manifest discovery from SD/local storage and the `appfs` partition.

Still TODO:

- fetch the manifest over Wi-Fi
- user-confirmed reboot orchestration after boot-slot selection
- rollback UX and failure recovery beyond the low-level mark-valid hook
- user-facing update screen and Feedback Manager progress routing

## Cache Paths

The firmware looks for one cached JSON manifest at the first matching path:

1. `/sd/limitlezz/ota/manifest.json`
2. `/sd/ota/manifest.json`
3. `/appfs/ota/manifest.json`

The native simulator uses the same layout under its data directory, for example
`lzdata/ota/manifest.json` and `lzdata/appfs/ota/manifest.json`.

Verified candidates are cached under the primary data directory:

```text
/sd/limitlezz/ota/firmware.bin
```

Downloads and local staging first write `firmware.bin.tmp`. A candidate is
promoted to `firmware.bin` only after exact byte-count and SHA-256 verification
against the cached manifest. A failed stage/download removes only the temporary
file and leaves any prior verified candidate intact.

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
ota candidate: none (no candidate)
```

Valid cached manifest:

```text
lz> ota status
ota manifest: valid version=0.97.0 channel=beta board=tdeck size=1539920 source=/sd/limitlezz/ota/manifest.json
firmware: https://updates.example/tdeck/0.97.0/firmware.bin
ota candidate: none (no candidate)
```

Verified candidate ready:

```text
lz> ota status
ota manifest: valid version=0.97.0 channel=beta board=tdeck size=1539920 source=/sd/limitlezz/ota/manifest.json
firmware: https://updates.example/tdeck/0.97.0/firmware.bin
ota candidate: ready version=0.97.0 channel=beta size=1539920 path=/sd/limitlezz/ota/firmware.bin sha=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
```

Fetch and verify the manifest's firmware URL:

```text
lz> ota fetch
[ok] OTA candidate downloaded and verified
ota candidate: ready version=0.97.0 channel=beta size=1539920 path=/sd/limitlezz/ota/firmware.bin sha=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
```

Stage and verify a local file:

```text
lz> ota stage /sd/limitlezz/ota/downloads/firmware.bin
[ok] OTA candidate staged and verified
ota candidate: ready version=0.97.0 channel=beta size=1539920 path=/sd/limitlezz/ota/firmware.bin sha=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
```

Write the verified candidate to the inactive OTA slot without changing boot:

```text
lz> ota write
[ok] OTA candidate written to inactive slot; boot unchanged
ota write: ok source=candidate running=ota_0 inactive=ota_1 addr=0x00510000 size=5242880 bytes=1539920 boot-set=no
```

Hardware smoke the same inactive-slot writer without preparing an SD candidate:

```text
lz> ota write-test
[ok] OTA inactive-slot write selftest passed; boot unchanged
ota write: ok source=running-copy running=ota_0 inactive=ota_1 addr=0x00510000 size=5242880 bytes=1539920 boot-set=no
```

Inspect and select the test boot slot:

```text
lz> ota slot-status
ota slots: running=ota_0@0x00010000 state=unset boot=ota_0@0x00010000 state=unset inactive=ota_1@0x00510000 boot-matches-running=yes pending=no

lz> ota set-test-boot
[ok] OTA copied current app and selected inactive slot for next boot; reset to test
ota write: ok source=running-copy running=ota_0 inactive=ota_1 addr=0x00510000 size=5242880 bytes=1539920 boot-set=yes
ota slots: running=ota_0@0x00010000 state=unset boot=ota_1@0x00510000 state=unset inactive=ota_1@0x00510000 boot-matches-running=no pending=no
```

After reset, confirm the running slot and mark the image valid:

```text
lz> ota slot-status
ota slots: running=ota_1@0x00510000 state=unset boot=ota_1@0x00510000 state=unset inactive=ota_0@0x00010000 boot-matches-running=yes pending=no

lz> ota mark-valid
[ok] OTA running app marked valid
ota slots: running=ota_1@0x00510000 state=unset boot=ota_1@0x00510000 state=unset inactive=ota_0@0x00010000 boot-matches-running=yes pending=no
```

Clear the candidate cache:

```text
lz> ota clear
[ok] OTA candidate cleared
```

Built-in parser proof:

```text
lz> ota test
OTA manifest selftest: PASS valid=1 invalid_error="bad sha256"
```
