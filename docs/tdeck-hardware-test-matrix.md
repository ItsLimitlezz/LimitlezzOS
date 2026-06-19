# T-Deck Release Hardware Test Matrix

This matrix turns the Phase 13 hardware-test roadmap item into a repeatable
release gate. It complements
[`docs/tdeck-hardware-dogfood-checklist.md`](tdeck-hardware-dogfood-checklist.md),
which is scenario-focused and Meshtastic-first.

Use this file for release-candidate evidence. For feature-branch evidence, run
the smallest relevant subset and record what was skipped.

## Artifact Rule

Every hardware run must identify the exact firmware under test.

Required evidence:

- repository, branch, and commit SHA
- clean or dirty git state before the artifact was built
- GitHub Actions run URL for `Firmware CI`
- `FLASH_MANIFEST.txt` from the downloaded artifact
- flash command and serial port
- serial smoke output
- pass/fail notes for each matrix row attempted

On slow local hosts, prefer the GitHub Actions artifact for the pushed commit:

```sh
python scripts/fetch_tdeck_artifact.py --repo ItsLimitlezz/LimitlezzOS --branch <branch> --commit <sha> --out .pio/ci-artifacts/<name>
python scripts/tdeck_smoke.py --port COM8 --no-stub-upload --skip-build --artifact-dir .pio/ci-artifacts/<name>
```

If the first post-flash console attach times out after a successful flash,
retry without reflashing:

```sh
python scripts/tdeck_smoke.py --port COM8 --skip-upload --open-timeout 60 --boot-timeout 90 --timeout 90 --no-expect --commands id sys net rf stats wifi "companion test"
```

## Test Dimensions

Cover these dimensions before declaring a firmware release complete:

- device: T-Deck and T-Deck Plus
- storage: SD present, SD absent, SD present but app/data failures injected
- Wi-Fi: disabled/no saved network, saved network present, network unavailable
- peers: one Meshtastic peer, multiple Meshtastic peers, one MeshCore peer
- radio mode: Meshtastic only, MeshCore only, both networks enabled
- power: USB powered, battery powered, sleep/wake while receiving
- update path: fresh flash, same-version reflash, OTA upgrade, OTA rollback
- app platform: accepted local app, rejected local app, scoped data clear

## Minimum Release Matrix

| ID | Configuration | Procedure | Required Evidence | Pass Gate |
| --- | --- | --- | --- | --- |
| H01 | T-Deck, SD present, USB power, no Wi-Fi | Flash the exact Actions artifact on `COM8`, then run serial smoke. | `FLASH_MANIFEST.txt`, esptool hash verification, `id`, `sys`, `net`, `rf`, `stats`, `wifi`, `companion test`. | Prompt returns after every command and smoke reports `PASS`. |
| H02 | T-Deck, SD absent | Boot after removing or disabling SD. Open Home, Messages, Files, App Store, and Settings. | Boot log, `sys`, screenshot/photo of Files/App Store degraded state. | No crash loop; storage-dependent screens explain unavailable storage. |
| H03 | T-Deck, SD present, saved Wi-Fi network | Connect to a known network, reboot, and confirm saved credential behavior. | `wifi`, UI settings state, reboot confirmation. | Network state persists correctly and failure state is readable if AP is unavailable. |
| H04 | T-Deck plus one Meshtastic peer | Receive and send LongFast text on the shared channel. | Peer firmware/version, packet receive evidence, sent message evidence. | Both directions succeed without duplicate spam. |
| H05 | T-Deck plus multiple Meshtastic peers | Receive repeated traffic from two peers and inspect nodes/threads. | `nodes`, thread list, RX/dedup notes. | Nodes update and duplicate packets do not create duplicate user messages. |
| H06 | T-Deck plus one MeshCore peer | Enable MeshCore path and validate public/default room receive plus one send path where available. | MeshCore peer identity, `mc peers` or equivalent serial evidence, thread evidence. | MeshCore messages route through the service boundary and appear in the unified UI. |
| H07 | Both networks enabled | Exercise split-airtime mode with Meshtastic and MeshCore traffic present. | `net`, `rf`, `stats`, switch count, message evidence from both networks. | Both networks make progress; any split-airtime regression is recorded as a blocking bug. |
| H08 | Battery powered sleep/wake | Let the device sleep, receive or queue traffic, wake it, then inspect notifications and unread state. | Before/after `sys`, lock-screen photo, unread thread evidence. | Wake returns to a usable UI and unread state is correct. |
| H09 | Local app platform with SD present | Install one valid SDK 0.1 local app and one intentionally rejected package. Open the valid app and run one action. | Package IDs, App Store accepted/rejected diagnostics, foreground app action result. | Accepted app launches; rejected app is explainable; scoped app data can be cleared. |
| H10 | OTA upgrade and rollback | Install a signed OTA candidate to the inactive slot, reboot, then force or simulate rollback. | OTA manifest, partition before/after, boot reason, rollback result. | Upgrade is atomic and rollback returns to the previous working firmware. |
| H11 | T-Deck Plus parity | Repeat H01, H03, H04, and H08 on T-Deck Plus. | Same evidence as repeated rows, plus device revision/photo. | T-Deck Plus matches T-Deck behavior or differences are documented. |
| H12 | Companion path | Run USB companion smoke and, when BLE is enabled for the build, official-app connect/reconnect/send/receive/disconnect. | `companion test`, BLE service UUID, app pairing notes. | USB companion passes; BLE behavior is either passing or listed as a known release blocker. |

## Feature-Branch Subsets

Use a targeted subset for PR validation, then record skipped rows in the PR body.

| Change Area | Required Rows |
| --- | --- |
| Docs only | H01 serial smoke using the exact Actions artifact. |
| UI-only simulator changes | H01 plus the native simulator checks and screenshot artifact. |
| Storage, Files, App Store, or app runtime | H01, H02, H09. |
| Wi-Fi or credential storage | H01, H03. |
| Meshtastic receive/send/routing | H01, H04, H05. |
| MeshCore or split-airtime | H01, H06, H07. |
| Sleep, battery, backlight, or power settings | H01, H08. |
| Companion USB or BLE | H01, H12. |
| OTA or partition work | H01, H10. |

## Evidence Log Template

```text
release:
branch:
commit:
workflow_run:
artifact:
flash_manifest_sha:
port:
device:
sd_state:
wifi_state:
peer_devices:
rows_run:
rows_skipped:
blocking_failures:
notes:
```

## Stop-The-Line Rules

Do not ship a release candidate when any of these are true:

- H01 fails on the release artifact.
- Boot loops, serial prompt loss, or display blanking is repeatable.
- Meshtastic send/receive is broken in the default release mode.
- Stored user data is corrupted across reboot.
- Settings cannot be saved or reverted.
- The release notes claim MeshCore, BLE, OTA, or app behavior that was not
  hardware-tested for that candidate.
- OTA rollback is required for the release but H10 is not passing.

Rows that are not yet implemented, such as OTA rollback before OTA support
lands, must be marked `not implemented` instead of silently skipped.
