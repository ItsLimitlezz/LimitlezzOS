/**
 * Emergency trigger guard.
 *
 * The eventual SOS feature must be hard to trigger by accident. This small
 * state machine requires an intentional hold before arming, then a confirmation
 * inside a short window before any emergency sender is allowed to run.
 */
#ifndef LZ_EMERGENCY_GUARD_H
#define LZ_EMERGENCY_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LZ_EMERGENCY_HOLD_MS       1200u
#define LZ_EMERGENCY_CONFIRM_MS   10000u

typedef enum {
    LZ_EMERGENCY_IDLE = 0,
    LZ_EMERGENCY_ARMED,
    LZ_EMERGENCY_TRIGGERED
} lz_emergency_state_t;

typedef struct {
    lz_emergency_state_t state;
    uint32_t armed_ms;
} lz_emergency_guard_t;

void lz_emergency_guard_reset(lz_emergency_guard_t *g);
bool lz_emergency_hold_ready(uint32_t held_ms);
bool lz_emergency_guard_arm(lz_emergency_guard_t *g, uint32_t now_ms, uint32_t held_ms);
bool lz_emergency_guard_confirm(lz_emergency_guard_t *g, uint32_t now_ms);
bool lz_emergency_guard_expired(const lz_emergency_guard_t *g, uint32_t now_ms);
uint32_t lz_emergency_guard_remaining(const lz_emergency_guard_t *g, uint32_t now_ms);
const char *lz_emergency_state_label(lz_emergency_state_t state);
int lz_emergency_guard_diag(const lz_emergency_guard_t *g, uint32_t now_ms,
                            char *buf, int n);

#ifdef __cplusplus
}
#endif

#endif
