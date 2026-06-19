# T-Deck Power Warning Policy

This Phase 11 foundation slice defines the low-battery decisions that future
Feedback Manager and power-saving code should use. It does not yet drive LEDs,
keyboard pulses, buzzer output, or automatic sleep; it gives those later pieces
one tested policy surface.

## Thresholds

| State | Battery percent | Battery voltage fallback |
| --- | --- | --- |
| Low | `<= 20%` | `<= 3650 mV` |
| Critical | `<= 5%` | `<= 3500 mV` |

If both percent and voltage are unavailable, the state is `unknown` and no
warning action is requested.

## Actions

| State | Notify | Wake screen | Dim screen | Force power-save | Buzz |
| --- | --- | --- | --- | --- | --- |
| Low | yes | no | no | no | no |
| Critical on battery | yes | yes | yes | yes | yes |
| Critical while charging/USB-powered | yes | no | no | no | no |

The current T-Deck serial console exposes `power`, which reports the current
state, action flags, and thresholds using the live `lz_svc_sysinfo()` telemetry.
