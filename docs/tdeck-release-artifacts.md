# T-Deck Release Artifacts

This runbook documents how release binaries are produced, attached, and proven
for LimitlezzOS T-Deck releases. It covers the current GitHub Actions artifact
flow. GitHub Release attachment is a maintainer action done after a release
candidate has passed the required checks.

## Source Of Truth

Release binaries must come from the `Firmware CI` workflow for the exact commit
being released. Do not attach locally built firmware as the release binary
unless GitHub Actions is unavailable and the release notes explicitly say so.

The workflow uploads three artifact families:

- `tdeck-firmware-<sha>`
- `tdeck-meshcore-firmware-<sha>` for opt-in Phase 3 split-airtime validation
- `native-screenshots-<sha>`

For pull requests, `<sha>` is the PR head SHA. The workflow also records the
GitHub pull-request merge SHA in `FLASH_MANIFEST.txt` as `github_sha`.

## Firmware Bundle Contents

The T-Deck firmware artifacts must include:

- `bootloader.bin`
- `boot_app0.bin`
- `firmware.bin`
- `firmware.elf`
- `firmware.map`
- `partitions.bin`
- `FLASH_MANIFEST.txt`
- `SIZE_BUDGET.md`
- `SIZE_BUDGET.txt`
- `size-budget.json`
- `tdeck-build.txt`
- `tdeck-size.txt`

`FLASH_MANIFEST.txt` must include:

- repository
- firmware source SHA
- GitHub workflow SHA
- workflow name
- run ID
- flash offsets
- firmware size
- OTA slot size and percentage
- static RAM size
- budget status

The MeshCore-enabled artifact manifest must also include `env=tdeck-meshcore`
and `meshcore_enabled=1`.

## Release Candidate Checklist

Before attaching binaries to a release:

1. Confirm the target commit and tag.
2. Confirm `Firmware CI` is green for that commit.
3. Download the matching `tdeck-firmware-<sha>` artifact.
4. Confirm `FLASH_MANIFEST.txt` names the expected repository, commit, workflow,
   run ID, and flash offsets.
5. Confirm `budget_status=pass`.
6. Flash the exact artifact on the hardware validation host.
7. Run the required hardware smoke and matrix rows for the release.
8. Download the matching `native-screenshots-<sha>` artifact.
9. Attach both artifacts, or archives derived directly from them, to the GitHub
   Release.
10. Copy key manifest and hardware evidence into the release notes.

## Download Commands

For the current checkout:

```sh
python scripts/fetch_tdeck_artifact.py --repo ItsLimitlezz/LimitlezzOS
```

For an explicit release candidate:

```sh
python scripts/fetch_tdeck_artifact.py --repo ItsLimitlezz/LimitlezzOS --branch <branch> --commit <sha> --out .pio/ci-artifacts/<release>
```

For the opt-in MeshCore TDM validation bundle:

```sh
python scripts/fetch_tdeck_artifact.py --env tdeck-meshcore --repo ItsLimitlezz/LimitlezzOS --branch <branch> --commit <sha>
```

To inspect the manifest:

```sh
cat .pio/ci-artifacts/<release>/FLASH_MANIFEST.txt
```

On Windows PowerShell:

```powershell
Get-Content .pio\ci-artifacts\<release>\FLASH_MANIFEST.txt
```

## Attachment Shape

Attach release files with names that keep the tag and commit visible:

```text
limitlezzos-tdeck-<tag>-<short-sha>-flash.zip
limitlezzos-native-screenshots-<tag>-<short-sha>.zip
```

The flash archive should contain the full `tdeck-firmware-<sha>` artifact, not
only `firmware.bin`. Keeping the ELF, map, build logs, and budget files attached
makes crash triage and size regression reviews possible after the workflow
artifacts expire.

## Suggested Release Note Evidence

Include this block in release notes:

```text
Firmware CI:
T-Deck artifact:
Native screenshots artifact:
source_sha:
github_sha:
run_id:
budget_status:
firmware_bytes:
firmware_slot_pct:
static_ram_bytes:
hardware_port:
hardware_smoke:
hardware_matrix_rows:
known_skips:
rollback_artifact:
```

## Hardware Proof

For the maintainer Windows host, flash the release artifact on `COM8`:

```sh
python scripts/tdeck_smoke.py --port COM8 --no-stub-upload --skip-build --artifact-dir .pio/ci-artifacts/<release>
```

For Phase 3 MeshCore TDM validation, flash the opt-in artifact and then run
the split-airtime probe:

```sh
python scripts/tdeck_smoke.py --port COM8 --env tdeck-meshcore --no-stub-upload --skip-build --artifact-dir .pio/ci-artifacts/tdeck-meshcore
python scripts/tdm_airtime_smoke.py --port COM8
```

If the first serial attach times out after a verified flash, retry without
reflashing:

```sh
python scripts/tdeck_smoke.py --port COM8 --skip-upload --open-timeout 60 --boot-timeout 90 --timeout 90 --no-expect --commands id sys net rf stats wifi "companion test"
```

Do not publish release notes that claim hardware validation until the smoke and
required hardware matrix rows are attached to the release evidence.

## Rollback Artifact

Every release should name the previous known-good artifact or release tag. Until
OTA rollback exists, rollback means reflashing that known-good USB artifact.

Keep the rollback artifact available anywhere the release binary is attached.

## Stop Conditions

Do not attach binaries to a public release when:

- `Firmware CI` is missing or failed for the release commit.
- `FLASH_MANIFEST.txt` is missing or names a different source SHA.
- `budget_status` is not `pass`.
- the bundle lacks `bootloader.bin`, `partitions.bin`, `boot_app0.bin`, or
  `firmware.bin`.
- hardware smoke is required for the release but not complete.
- release notes claim OTA, rollback, BLE, MeshCore, or app behavior that was not
  verified by the recorded evidence.
