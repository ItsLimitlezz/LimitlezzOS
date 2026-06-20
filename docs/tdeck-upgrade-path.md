# T-Deck Upgrade Path

This guide documents the upgrade path that is available today. LimitlezzOS has
an OTA-capable partition layout, but over-the-air firmware updates are still
roadmap work. Current releases upgrade through a USB flash of an exact release
or GitHub Actions artifact.

## Current Support

Supported today:

- fresh USB flash from a local PlatformIO build
- USB upgrade from an exact `Firmware CI` artifact
- same-version reflash for recovery or validation
- rollback by reflashing a previously saved known-good artifact
- persistent SD-backed user data when the SD card and store remain intact
- persistent NVS-backed Wi-Fi credentials on T-Deck hardware

Not supported yet:

- downloading firmware over Wi-Fi
- writing a candidate to the inactive OTA slot from the device UI
- automatic rollback based on a firmware health marker
- signed OTA manifests or release-channel selection

## Partition Layout

The current 16 MB T-Deck partition table keeps two 5 MB OTA app slots plus data
partitions:

```text
nvs       0x009000   0x005000
otadata   0x00e000   0x002000
ota_0     0x010000   0x500000
ota_1     0x510000   0x500000
config    0xa10000   0x100000
appfs     0xb10000   0x4e0000
```

The current flash helper writes:

```text
0x0      bootloader.bin
0x8000   partitions.bin
0xe000   boot_app0.bin
0x10000  firmware.bin
```

That means the normal release flash updates the bootloader, partition table,
OTA data image, and the primary firmware image. It does not intentionally erase
the whole chip, SD card, `config`, `appfs`, or NVS data.

## What Should Survive

Expected to survive a normal USB upgrade:

- SD-backed identity, settings, node database, message history, and local app
  packages under `/sd/limitlezz` when the card is present and healthy
- T-Deck hardware Wi-Fi credentials stored in ESP32 NVS
- local app packages in the FAT `appfs` partition when that partition is not
  reformatted
- user-visible settings that are persisted through `settings.cfg`

Not guaranteed to survive:

- RAM-only sessions when no SD card is present
- data from an old build when the release notes explicitly say a partition or
  store migration is required
- app data that exceeds the current SDK 0.1 quota or is rejected by newer
  manifest rules
- data after a full-chip erase, manual partition format, SD card replacement,
  or appfs reformat

## Preflight

Before upgrading a real device:

1. Record the current firmware version or commit.
2. Save the known-good artifact or release bundle that can be reflashed for
   rollback.
3. Confirm the target branch and commit.
4. Wait for `Firmware CI` to pass for that exact commit.
5. Download the matching artifact and keep its `FLASH_MANIFEST.txt`.
6. Confirm the intended serial port. On the maintainer Windows host, the T-Deck
   test port is `COM8`.
7. If the upgrade is for a release candidate, record whether SD, Wi-Fi, peers,
   companion, app platform, and sleep/wake rows from the hardware matrix are in
   scope.

## Upgrade From GitHub Actions

Use this path on slow local hosts and for PR/release evidence:

```sh
python scripts/fetch_tdeck_artifact.py --repo ItsLimitlezz/LimitlezzOS --branch <branch> --commit <sha> --out .pio/ci-artifacts/<name>
python scripts/tdeck_smoke.py --port COM8 --no-stub-upload --skip-build --artifact-dir .pio/ci-artifacts/<name>
```

For the opt-in MeshCore TDM validation image:

```sh
python scripts/fetch_tdeck_artifact.py --env tdeck-meshcore --repo ItsLimitlezz/LimitlezzOS --branch <branch> --commit <sha>
python scripts/tdeck_smoke.py --port COM8 --env tdeck-meshcore --no-stub-upload --skip-build --artifact-dir .pio/ci-artifacts/tdeck-meshcore
python scripts/tdm_airtime_smoke.py --port COM8
```

The artifact fetch is strict by default. It refuses to download an artifact when
there is no successful `Firmware CI` run for the requested commit.

Save this evidence:

- Actions run URL
- artifact name
- `FLASH_MANIFEST.txt`
- esptool hash verification output
- serial smoke output
- any no-reflash retry used after USB boot handoff

## Upgrade From A Local Build

Use this path when developing locally or when Actions is unavailable:

```sh
pio run -e tdeck
python scripts/tdeck_smoke.py --port COM8 --no-stub-upload
```

On this host, prefer the Actions artifact path for release evidence because the
T-Deck build is slow locally.

## Smoke Test

After every upgrade, run the standard serial smoke:

```sh
python scripts/tdeck_smoke.py --port COM8 --skip-upload
```

Minimum passing evidence:

- `id` reports the expected node identity
- `sys` reports battery/USB, CPU, RAM, and uptime
- `net` reports enabled mesh networks
- `rf` reports active radio mode and RF settings
- `stats` reports TX/RX counters
- `wifi` reports saved/connected state without printing passwords
- `companion test` reports a PASS for USB companion framing

If the first post-flash console attach times out after a successful flash,
retry without reflashing:

```sh
python scripts/tdeck_smoke.py --port COM8 --skip-upload --open-timeout 60 --boot-timeout 90 --timeout 90 --no-expect --commands id sys net rf stats wifi "companion test"
```

If Windows loses `COM8` during the USB boot handoff, wait for the T-Deck to
re-enumerate or reset/replug the device. Do not retarget the smoke test to
another COM port unless that device's ownership has been confirmed.

## Rollback

Rollback today means reflashing a previous known-good artifact over USB.

Procedure:

1. Identify the last known-good commit and artifact.
2. Download or locate its flash bundle.
3. Flash it with `scripts/tdeck_smoke.py --no-stub-upload --skip-build`.
4. Run the standard serial smoke.
5. Recheck the user-facing path that caused the rollback.
6. Record whether SD-backed history, settings, Wi-Fi credentials, and local apps
   survived the rollback.

Rollback is not automatic yet. A future OTA implementation must add a health
marker and boot-slot rollback before release notes can claim automatic recovery.

## Release Notes Checklist

Every release note should state:

- exact commit SHA
- artifact or release bundle name
- whether the partition table changed
- whether any store or app data migration is expected
- whether Wi-Fi credentials should survive
- whether appfs content should survive
- known rollback artifact or previous release
- hardware matrix rows completed for the release
- any rows that are `not implemented` or intentionally skipped

## Stop Conditions

Pause the upgrade and fix the release candidate when:

- no successful `Firmware CI` artifact exists for the intended commit
- `FLASH_MANIFEST.txt` does not match the artifact being flashed
- esptool hash verification fails
- serial smoke cannot reach the `lz>` prompt after the device re-enumerates
- a normal upgrade erases SD-backed state without a documented migration reason
- the release claims OTA or automatic rollback support before Phase 10 has
  implemented and tested it
