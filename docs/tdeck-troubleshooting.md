# T-Deck Troubleshooting Guide

Use this guide when a LimitlezzOS T-Deck build, flash, boot, radio session, or
hardware smoke does not behave as expected. Start by capturing evidence before
changing state; most failures can be separated into host/USB, boot hardware,
storage, radio, Wi-Fi, companion, or app-platform issues.

## First Evidence To Capture

Record these before reflashing or erasing anything:

- Branch, commit, and dirty/clean state:

```sh
git status -sb
git rev-parse HEAD
```

- Exact firmware artifact source:

```powershell
Get-Content .pio\ci-artifacts\tdeck\FLASH_MANIFEST.txt
```

- Serial smoke or read-only smoke:

```sh
python scripts/tdeck_smoke.py --port COM8 --skip-upload --open-timeout 60 --boot-timeout 90 --timeout 90 --no-expect --commands id sys net rf stats wifi "companion test"
```

- Native sanity on the host:

```sh
pio run -e native
.pio/build/native/program --selftest
.pio/build/native/program --simtest
```

If the issue is tied to a PR or release, paste the command output and the
artifact manifest into the PR evidence instead of summarizing from memory.

## Host Or USB Problems

### COM8 Is Missing Or Busy

Checks:

- Close PlatformIO monitor, Meshtastic clients, terminal windows, and any other
  serial tool.
- Unplug and replug the device; confirm the Windows COM port did not change.
- Try a known data-capable USB cable and a different USB port.

Useful commands:

```sh
python scripts/serial_harness.py --port COM8 --baud 115200 --open-only --open-timeout 5
```

If the open-only check fails, fix port ownership or cabling before reflashing.

### Upload Connects Then Fails During Stub Handoff

The maintainer COM8 path can be flaky with the ESP ROM stub. Prefer the
no-stub helper:

```sh
python scripts/tdeck_smoke.py --port COM8 --no-stub-upload
```

For slow hosts, use the GitHub Actions artifact instead of a local T-Deck build:

```sh
python scripts/fetch_tdeck_artifact.py --repo ItsLimitlezz/LimitlezzOS --branch <branch> --commit <sha> --out .pio/ci-artifacts/tdeck
python scripts/tdeck_smoke.py --port COM8 --no-stub-upload --skip-build --artifact-dir .pio/ci-artifacts/tdeck
```

## Flash Succeeds But Smoke Times Out

USB CDC may disconnect briefly during boot. If flashing completed and all hashes
verified, do not erase or reflash first. Run a read-only reattach:

```sh
python scripts/tdeck_smoke.py --port COM8 --skip-upload --open-timeout 60 --boot-timeout 90 --timeout 90 --no-expect --commands id sys net rf stats wifi "companion test"
```

If the retry passes, record both facts:

- exact-artifact flash succeeded
- first post-flash smoke timed out during serial reattach
- read-only retry completed `[serial] smoke PASS`

If the retry still fails, capture the partial boot output and check whether the
device is in text console mode or an external companion mode.

## Boot Hardware Diagnostics

The boot log isolates most hardware bring-up failures:

```text
=== LimitlezzOS boot ===
[ok] peripheral power (GPIO10) HIGH
[ok] shared SPI bus up (SCK40/MISO38/MOSI41)
[ok] ST7789 display init + backlight on
[ok] keyboard @0x55
[ok] GT911 touch @0x5D
[ok] trackball + keyboard input
[ok] microSD /sd/limitlezz
[ok] SX1262 radio (RadioLib begin=0)
[ok] node id !...
=== boot complete ===
```

Common interpretations:

- Display works, but SD and radio fail: inspect shared SPI chip-select behavior
  and SD card seating.
- `SX1262 radio (RadioLib begin=...)` is non-zero: treat this as a radio
  bring-up problem, not a UI problem.
- Keyboard or touch fail: inspect I2C bring-up first; rerun touch calibration
  only after GT911 is detected.
- Display colors look inverted: verify panel revision and the inversion setting.
- `appfs not mounted`: expected when the appfs partition is empty or absent; not
  the same as an SD mount failure.

## Serial Console Diagnostics

Run these from the `lz>` prompt:

```text
help
id
sys
net
rf
stats
wifi
nodes
dm status
companion test
```

What to look for:

- `id`: node identity should be stable across reboot when storage is present.
- `sys`: battery, CPU, RAM, and uptime should update.
- `net`: confirms whether Meshtastic and MeshCore are enabled.
- `rf`: confirms active profile and dwell split.
- `stats`: confirms radio TX/RX counters and airtime utilization.
- `wifi`: must report credential backend without printing a password.
- `dm status`: use when delivery state, retries, or ACKs look wrong.
- `companion test`: should end in `PASS` for the USB companion framing path.

## SD, Appfs, And Persistence

Symptoms:

- identity changes after reboot
- message history disappears
- local apps do not appear
- Files only shows one storage root

Checks:

- Boot log includes `microSD /sd/limitlezz` when SD is mounted.
- `sys` reports sane RAM and uptime after boot.
- Files screen exposes SD/local storage and appfs only when those roots mount.
- Local app packages use safe IDs, relative entry paths, and supported SDK
  permission names.

Recovery:

- Reseat or replace the SD card if SD mount is missing.
- Test RAM-only behavior deliberately by booting without SD, but do not count
  persistence tests as passed in that state.
- Use Developer Mode app diagnostics for rejected local packages.

## Wi-Fi Problems

Checks:

```text
wifi
net
sys
```

Interpretation:

- `wifi: off saved=(none)` means no remembered network is currently configured.
- `cred=nvs` on hardware means saved credentials are not stored as plaintext on
  SD.
- Wi-Fi and BLE cannot both stay resident on this RAM-tight ESP32-S3 path; test
  one transport at a time.

Recovery:

- Forget and rejoin the network from Settings if the saved SSID is stale.
- Keep password evidence out of logs and PR bodies.
- If Wi-Fi testing is not part of the release claim, record the saved/off state
  instead of forcing a network join.

## Radio, Mesh, And Delivery Issues

Checks:

```text
rf
stats
nodes
dm status
rxlog on
```

Interpretation:

- No RX count increase: verify region/preset/channel and antenna.
- LongFast public messages missing: confirm the stock peer can talk to another
  stock device on the same channel.
- DMs stuck pending: inspect `dm status`, peer reachability, and ACK timeout
  state.
- Duplicate messages: capture `rxlog` evidence and dedup behavior.
- MeshCore behavior looks wrong: confirm whether MeshCore is enabled and whether
  the branch under test has the relevant MeshCore feature gate opened.
- TDM concerns require a timed hardware run; a single `rf` output proves current
  profile state, not packet-loss behavior under load.

## Companion Problems

USB companion:

- Run `companion test` first; it should report frame counts and `PASS`.
- If the text console disappears after companion use, disable companion mode and
  reattach serial.
- Only one external app transport should own the bridge at a time.

BLE companion:

- `companion ble on` enables BLE diagnostics.
- The official app connect-then-disconnect issue is still tracked as a known
  validation gap; do not mark BLE phone validation complete from the mailbox
  selftest alone.

## Native Simulator And CI Issues

If native builds fail on Windows, install the local SDL2 bundle:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\ensure_sdl2_windows.ps1
```

If GitHub Actions is green but local T-Deck builds are slow or flaky, use the
uploaded firmware artifact and keep local validation to native/simulator checks.

If Actions fails:

- Open the failing `Firmware CI` job.
- Check whether native build, selftest, simtest, screenshot generation, T-Deck
  build, size budget, or artifact upload failed.
- Fix the earliest failing step first.
- Do not flash a PR artifact until the exact head commit has a successful
  Actions run.

## When To Erase Flash

Erase flash only after ordinary reflashing and read-only serial recovery fail.
An erase wipes ESP32 flash partitions, including OTA/appfs/config data, but does
not erase a removable SD card:

```sh
pio run -e tdeck -t erase --upload-port COM8
python scripts/tdeck_smoke.py --port COM8 --no-stub-upload
```

Record the erase in PR or release evidence because it changes the recovery path
being validated.
