# T-Deck Feedback Policy

This is the first Phase 11 foundation slice. It does not drive hardware pins
yet; it centralizes the policy that future UI, app SDK, OTA, and emergency code
will use before touching screen wake, keyboard backlight pulses, LEDs, or a
buzzer.

## DND Modes

| Mode | Immediate feedback | Queued only |
| --- | --- | --- |
| `off` | messages, direct messages, app notifications, system critical, OTA failure, emergency | none |
| `priority` | direct messages, system critical, OTA failure, emergency | normal messages, app notifications, OTA progress |
| `silent` | system critical, OTA failure, emergency | messages, direct messages, app notifications, OTA progress |
| `emergency` | emergency only | everything else |

All events remain queued so the user can review them later. Critical system,
OTA-failure, and emergency events are marked as DND bypasses when they break
through an active DND mode; emergency always keeps the buzzer path enabled.

## Validation

- Native selftest covers the policy matrix and diagnostic text.
- T-Deck builds expose the `feedback` serial diagnostic command so hardware
  smoke runs can capture the active policy matrix before LED/buzzer ownership is
  wired in.
