# T-Deck Release Bug Gate

This guide defines the Phase 13 release gate for "no known P0/P1 bugs." It is
not a promise that every roadmap feature is complete. It is a rule for deciding
whether the firmware can ship with the claims made in README, release notes,
and the release artifact evidence.

## Rule

A release candidate can ship only when:

- no known P0 bugs remain
- no known P1 bugs remain in release-claimed behavior
- unfinished roadmap work is labeled as planned, partial, prototype, or not
  implemented
- hardware validation evidence matches the release claims
- release notes list any intentionally deferred P2/P3 issues

If a feature is incomplete but clearly documented as incomplete, it is not
automatically a P0/P1 bug. If release notes claim that feature as working, it
must satisfy the gate.

## Severity Definitions

| Severity | Definition | Release Action |
| --- | --- | --- |
| P0 | Bricks, boot-loops, prevents basic use, corrupts user data, breaks default Meshtastic send/receive, loses keys/identity unexpectedly, or creates an unsafe flash/update path. | Stop the release. Fix or remove the release claim. |
| P1 | Breaks a primary release-claimed workflow, causes repeatable crashes, prevents rollback/recovery, exposes secrets contrary to release claims, or makes hardware validation inconclusive. | Stop the release unless the claim is removed and the limitation is explicit. |
| P2 | Important defect with a workaround, feature-specific gap outside the release's main claims, or hardware coverage gap that is clearly documented. | May ship only when listed in known issues and not part of the headline claim. |
| P3 | Cosmetic, wording, small polish, or future enhancement. | May ship when tracked or documented. |

## P0 Examples

- device cannot boot to the lock screen after flashing the release artifact
- `COM8` flash verifies but the same artifact cannot ever reach the serial
  prompt after reset/replug
- display remains blank after a repeatable release flash
- default Meshtastic LongFast receive or send is broken
- message history, identity, keys, or settings are corrupted during a normal
  upgrade
- firmware budget check is missing or failing
- release binary does not match `FLASH_MANIFEST.txt`
- OTA or rollback is advertised but cannot recover from a failed candidate

## P1 Examples

- repeated UI crash in Home, Messages, Settings, App Store, or Terminal
- saved Wi-Fi credentials are exposed contrary to the release notes
- direct-message delivery state regresses from the documented behavior
- serial smoke cannot complete on the maintainer hardware after the port is
  confirmed present
- app sandbox behavior lets one local app write outside its scoped data
- MeshCore, BLE, OTA, or app-catalog behavior is claimed as complete without
  matching hardware or simulator evidence
- required release artifacts are missing `firmware.elf`, `firmware.map`, or
  size/budget evidence needed for support

## Known-Issue Classification

Use this table in release notes and PR evidence:

```text
id:
severity:
area:
claim affected:
evidence:
workaround:
release decision:
owner:
next action:
```

`claim affected` must name the exact README/release-note claim. If no public
claim is affected, the issue is usually P2/P3 or planned work.

## Pre-Release Triage Procedure

1. Read the README beta status and the draft release notes.
2. Read `docs/tdeck-firmware-roadmap.md` Phase 13 release gates.
3. Read the release artifact `FLASH_MANIFEST.txt`.
4. Review the latest audit/inventory notes for stale or changed status.
5. Compare every headline claim against current CI, simulator, artifact, and
   hardware evidence.
6. Classify every open defect as P0, P1, P2, or P3.
7. Remove or soften any release claim that lacks evidence.
8. Re-run the targeted validation after every P0/P1 fix.
9. Do not publish until the P0/P1 list is empty or the affected claim is removed.

## Evidence Required To Close P0/P1

Closing a P0/P1 requires stronger evidence than a narrow smoke test:

- for boot/flash bugs: exact artifact flash log, manifest, and serial prompt
- for UI crashes: simulator reproduction or hardware steps plus passing rerun
- for radio bugs: peer identity, network mode, serial `net`/`rf`/`stats`, and
  send/receive evidence
- for storage bugs: before/after files or serial/UI state across reboot
- for credential bugs: backend evidence such as serial `wifi` reporting
  `cred=nvs` without printing the password
- for app sandbox bugs: package layout, rejected/accepted diagnostics, and
  scoped data evidence
- for release process bugs: Actions run URL, artifact name, manifest, and PR or
  release-note update

## Ship/No-Ship Checklist

Before a release is published:

- [ ] `Firmware CI` is green for the release commit.
- [ ] exact release artifacts are downloaded and inspected.
- [ ] required COM8 hardware smoke and matrix rows are complete.
- [ ] README status matches code and evidence.
- [ ] release notes do not overclaim planned work.
- [ ] known P0 list is empty.
- [ ] known P1 list is empty or the affected claim has been removed.
- [ ] P2/P3 known issues are listed with workarounds or next actions.
- [ ] rollback artifact is named.

## Handling Deferred Work

Deferred roadmap items should be explicit, not treated as hidden bugs. Examples:

- OTA download and automatic rollback remain Phase 10 work until implemented.
- richer app runtime APIs and network catalog installs remain later app-platform
  work until implemented.
- BLE companion phone-session behavior must stay marked as in testing until
  official-app connect/reconnect/send/receive/disconnect evidence exists.
- MeshCore split-airtime concerns must stay in testing until hardware
  re-verification proves the scheduler behavior.

If a deferred item is mentioned in marketing or release notes, it must use the
same label as README and the roadmap.
