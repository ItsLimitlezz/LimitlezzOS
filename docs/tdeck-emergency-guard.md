# T-Deck Emergency Guard

This Phase 11 foundation slice defines the guard in front of future emergency
beacon behavior. It does not transmit SOS messages yet; it prevents later
Meshtastic/MeshCore emergency senders from being callable through a single
accidental tap.

## Trigger Flow

1. Hold the emergency action for at least `1200 ms`.
2. The guard enters `armed`.
3. Confirm within `10000 ms`.
4. Only a valid confirmation returns `triggered`.

Confirming without a prior hold, releasing too early, or waiting past the
confirmation window leaves the guard idle and no emergency action is allowed.

The T-Deck serial console exposes `emergency`, `emergency arm`,
`emergency confirm`, and `emergency cancel` as a diagnostic-only guard flow.
`confirm` prints that the emergency beacon is not wired yet instead of sending
anything over radio.
