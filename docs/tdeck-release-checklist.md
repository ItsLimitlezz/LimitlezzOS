# T-Deck Release Checklist

Use this checklist before calling a LimitlezzOS T-Deck firmware PR or release
ready. It is intentionally biased toward slow maintainer hosts: run quick local
native checks, build the T-Deck binary in GitHub Actions, then flash the exact
Actions artifact on the COM8 T-Deck.

## Required Evidence

- Branch name, PR number, commit SHA, and whether the PR is draft or ready.
- GitHub Actions `Firmware CI` run URL for the exact PR head commit.
- Downloaded artifact directory and `FLASH_MANIFEST.txt` contents.
- Local native/simulator check results.
- COM8 flash log, including chip, MAC, flash byte counts, and hash verification.
- Serial smoke output for `id`, `sys`, `net`, `rf`, `stats`, `wifi`, and
  `companion test`.
- Manual hardware notes for display, touch, keyboard, trackball, SD/appfs,
  radio, Wi-Fi, companion mode, and sleep/wake.

Generate a PR-ready skeleton with:

```sh
python scripts/release_evidence.py --artifact-dir .pio/ci-artifacts/tdeck --port COM8
```

## Local Sanity Checks

Run local checks that do not require a T-Deck firmware build:

```sh
python -m py_compile scripts/tdeck_smoke.py scripts/fetch_tdeck_artifact.py scripts/release_evidence.py
pio run -e native
.pio/build/native/program --selftest
.pio/build/native/program --simtest
```

If SDL2 is missing on Windows, install the local bundle once:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\ensure_sdl2_windows.ps1
```

## GitHub Actions Build

Push the branch, wait for `Firmware CI`, then download the exact commit
artifact:

```sh
git push fork HEAD
gh run list --repo ItsLimitlezz/LimitlezzOS --workflow "Firmware CI" --branch <branch> --limit 5
gh run watch --repo ItsLimitlezz/LimitlezzOS <run-id> --exit-status --interval 10
python scripts/fetch_tdeck_artifact.py --repo ItsLimitlezz/LimitlezzOS --branch <branch> --commit <sha> --out .pio/ci-artifacts/tdeck
```

Confirm `FLASH_MANIFEST.txt` contains the expected `repo=`, `sha=`,
`workflow=`, `run_id=`, `flash_offsets=`, and size-budget lines.

Do not flash an artifact from a different commit unless the PR body explicitly
calls that out as a non-release diagnostic.

## COM8 Flash And Serial Smoke

Flash the downloaded Actions artifact:

```sh
python scripts/tdeck_smoke.py --port COM8 --no-stub-upload --skip-build --artifact-dir .pio/ci-artifacts/tdeck --open-timeout 60 --boot-timeout 60 --timeout 30
```

Capture:

- ESP32-S3 revision and MAC.
- Flash byte counts and hash verification for bootloader, partitions,
  boot app, and firmware.
- Device identity from `id`.
- Battery, uptime, storage, and heap/PSRAM from `sys`.
- Network toggles from `net`.
- Radio profile/status from `rf`.
- Packet/service counters from `stats`.
- Wi-Fi saved-state and credential backend from `wifi`; never record a
  password.
- `companion test` result.

If the post-flash serial console misses the first boot window, retry a
read-only smoke without reflashing:

```sh
python scripts/tdeck_smoke.py --port COM8 --skip-upload --open-timeout 60 --boot-timeout 90 --timeout 90 --no-expect --commands id sys net rf stats wifi "companion test"
```

## Manual Hardware Pass

After serial smoke, verify the physical device:

- Display renders the lock screen and Home without obvious tearing or inverted
  colors.
- Trackball moves focus in all directions and click selects.
- Keyboard types in a conversation composer; backspace and Enter work.
- Touch can tap a list row and a top-level navigation target.
- Files can enter the mounted SD/local store or appfs root when present.
- Wi-Fi screen shows saved state correctly and can scan/connect when testing a
  release candidate.
- Meshtastic USB companion mode can be enabled and disabled without losing the
  serial console after exit.
- Sleep/dim timeout wakes with the intended two-step wake/unlock behavior.
- For release candidates, include at least one stock Meshtastic peer and any
  MeshCore peer or split-airtime scenario claimed by the release notes.

## Release Gates

- `Firmware CI` is green for the exact head commit.
- COM8 exact-artifact flash and serial smoke pass.
- Firmware and static-RAM size budgets pass in the artifact manifest.
- No known P0/P1 issue is being hidden by release wording.
- README, roadmap, and inventory status match the actual tested state.
- User-facing release notes distinguish working, partial, prototype, planned,
  and needs-validation features.
- Attached binaries or release assets come from the same Actions run recorded
  in the evidence.

## PR Evidence Template

```md
## Validation

- Local: `python -m py_compile ...`
- Local: `pio run -e native`
- Local: `.pio/build/native/program --selftest`
- Local: `.pio/build/native/program --simtest`
- Actions: Firmware CI <run URL> for `<sha>`
- Artifact: `.pio/ci-artifacts/tdeck`, manifest `sha=<sha>`
- Hardware: COM8 exact-artifact flash/smoke passed

## Hardware Notes

- Flash: ESP32-S3 rev <rev>, MAC <mac>, hashes verified
- Identity: `<id output>`
- System: `<sys summary>`
- Radio/network: `<net/rf/stats summary>`
- Wi-Fi: `<wifi summary>`
- Companion: `<companion test summary>`
- Manual: display/touch/keyboard/trackball/SD/Wi-Fi/sleep checked
```
