# T-Deck Flashing And Recovery Guide

This guide covers the supported LimitlezzOS flash path for LilyGO T-Deck and
T-Deck Plus hardware. T-Deck Pro uses a different board layout and is not
covered by this firmware target.

The maintainer workflow favors GitHub Actions for the T-Deck build, then local
COM8 validation with the exact artifact. That keeps slow Windows hosts from
spending time on full ESP32 builds while still proving the binary on hardware.

## Known Hardware Target

- Board: LilyGO T-Deck or T-Deck Plus with ESP32-S3 and SX1262.
- Flash: 16 MB.
- Default maintainer serial port: `COM8`.
- Linux/macOS examples usually use `/dev/ttyACM0` or `/dev/ttyUSB0`.
- Firmware offsets: `0x0 bootloader.bin`, `0x8000 partitions.bin`,
  `0xe000 boot_app0.bin`, `0x10000 firmware.bin`.
- App/user data on the SD card is separate from ESP32 flash, but appfs and OTA
  partitions are on ESP32 flash.

## Preflight

- Close `pio device monitor`, serial terminals, Meshtastic clients, and other
  tools that may own the serial port.
- Use a USB data cable, not a charge-only cable.
- Keep the device powered from USB during the whole flash.
- On Windows, confirm the expected COM port in Device Manager after plug-in.
- Confirm GitHub CLI auth if downloading Actions artifacts:

```sh
gh auth status
```

## Preferred Slow-Host Flash Path

Push the branch and let GitHub Actions build the firmware:

```sh
git push fork HEAD
gh run list --repo ItsLimitlezz/LimitlezzOS --workflow "Firmware CI" --branch <branch> --limit 5
gh run watch --repo ItsLimitlezz/LimitlezzOS <run-id> --exit-status --interval 10
```

Download the artifact for the exact commit:

```sh
python scripts/fetch_tdeck_artifact.py --repo ItsLimitlezz/LimitlezzOS --branch <branch> --commit <sha> --out .pio/ci-artifacts/tdeck
```

Check `FLASH_MANIFEST.txt` before flashing:

```powershell
Get-Content .pio\ci-artifacts\tdeck\FLASH_MANIFEST.txt
```

The manifest `sha=` must match the PR head commit. The `budget_status=` line
must be `pass` for a release candidate.

Flash and run the standard serial smoke:

```sh
python scripts/tdeck_smoke.py --port COM8 --no-stub-upload --skip-build --artifact-dir .pio/ci-artifacts/tdeck --open-timeout 60 --boot-timeout 60 --timeout 30
```

Use `/dev/ttyACM0` or `/dev/ttyUSB0` instead of `COM8` on non-Windows hosts.

## Local Build Flash Path

Use local T-Deck builds only when you intentionally need a host-built binary:

```sh
pio run -e tdeck
python scripts/tdeck_smoke.py --port COM8 --no-stub-upload
```

The no-stub path is preferred on the maintainer T-Deck because the ROM stub
upload path can be flaky on this host.

## Serial Smoke Proof

A passing smoke should include:

- ESP32-S3 chip revision and MAC during flash.
- Hash verification for every flashed image.
- `=== boot complete ===` during boot.
- `id` reporting the expected node identity.
- `sys` reporting battery, CPU, RAM, and uptime.
- `net` reporting network toggles.
- `rf` reporting the active radio profile.
- `stats` reporting radio counters.
- `wifi` reporting saved state and credential backend without printing a
  password.
- `companion test` ending in `PASS`.
- `[serial] smoke PASS`.

## Read-Only Retry After Flash

USB CDC can briefly disappear during boot. If flash succeeded but the first
serial smoke times out, do not reflash. Reattach and run a read-only smoke:

```sh
python scripts/tdeck_smoke.py --port COM8 --skip-upload --open-timeout 60 --boot-timeout 90 --timeout 90 --no-expect --commands id sys net rf stats wifi "companion test"
```

If this passes, record both facts: the exact-artifact flash completed, and the
read-only retry completed the serial proof.

## Recovery Playbook

### Port Missing

- Unplug and replug USB, then recheck the port name.
- Try another USB data cable and port.
- Close any monitor or phone companion tool that may hold the port.
- If the port changes after boot, rerun the command with the new port.

### Upload Will Not Connect

- Use `--no-stub-upload`.
- Lower upload speed if a local command was using a faster baud.
- Power-cycle the device and rerun the command.
- If the board exposes physical boot/reset controls, enter the ROM bootloader
  with the board controls, then rerun the same flash command.

### Flash Succeeds But No Prompt Appears

- Run the read-only retry command above.
- Increase `--boot-timeout` to `120` on especially slow boot attempts.
- Confirm the device is not left in USB companion mode; the text console smoke
  needs the LimitlezzOS serial prompt.

### Boot Diagnostics Show Hardware Failures

- If SD and radio fail while display works, check SPI chip-select behavior and
  the SD card state.
- If `SX1262 radio (RadioLib begin=...)` reports a non-zero code, treat it as a
  radio bring-up problem rather than a UI problem.
- If display colors look inverted, verify the panel revision and inversion
  setting.
- If touch fails, rerun touch calibration from Settings after a successful boot.

### Last-Resort Flash Recovery

Only erase flash when ordinary reflashing cannot recover the device. This wipes
ESP32 flash partitions, including OTA/appfs/config data, but does not erase the
removable SD card:

```sh
pio run -e tdeck -t erase --upload-port COM8
python scripts/tdeck_smoke.py --port COM8 --no-stub-upload
```

Record that an erase was used, because it changes the upgrade/recovery evidence
for the run.

## Release Evidence

For PRs and releases, record:

- Branch, commit, PR number, and Actions run URL.
- Artifact path and manifest contents.
- Flash chip/MAC/hash verification.
- Serial smoke output.
- Any read-only retry or recovery step used.
- Any manual hardware checks performed after boot.
