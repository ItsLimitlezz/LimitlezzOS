#include "emergency_guard.h"
#include <stdio.h>

void lz_emergency_guard_reset(lz_emergency_guard_t *g)
{
    if(!g) return;
    g->state = LZ_EMERGENCY_IDLE;
    g->armed_ms = 0;
}

bool lz_emergency_hold_ready(uint32_t held_ms)
{
    return held_ms >= LZ_EMERGENCY_HOLD_MS;
}

bool lz_emergency_guard_arm(lz_emergency_guard_t *g, uint32_t now_ms, uint32_t held_ms)
{
    if(!g || !lz_emergency_hold_ready(held_ms)) return false;
    g->state = LZ_EMERGENCY_ARMED;
    g->armed_ms = now_ms;
    return true;
}

bool lz_emergency_guard_expired(const lz_emergency_guard_t *g, uint32_t now_ms)
{
    if(!g || g->state != LZ_EMERGENCY_ARMED) return false;
    return (uint32_t)(now_ms - g->armed_ms) > LZ_EMERGENCY_CONFIRM_MS;
}

uint32_t lz_emergency_guard_remaining(const lz_emergency_guard_t *g, uint32_t now_ms)
{
    if(!g || g->state != LZ_EMERGENCY_ARMED) return 0;
    uint32_t elapsed = (uint32_t)(now_ms - g->armed_ms);
    if(elapsed >= LZ_EMERGENCY_CONFIRM_MS) return 0;
    return LZ_EMERGENCY_CONFIRM_MS - elapsed;
}

bool lz_emergency_guard_confirm(lz_emergency_guard_t *g, uint32_t now_ms)
{
    if(!g || g->state != LZ_EMERGENCY_ARMED) return false;
    if(lz_emergency_guard_expired(g, now_ms)) {
        lz_emergency_guard_reset(g);
        return false;
    }
    g->state = LZ_EMERGENCY_TRIGGERED;
    return true;
}

const char *lz_emergency_state_label(lz_emergency_state_t state)
{
    switch(state) {
        case LZ_EMERGENCY_ARMED:     return "armed";
        case LZ_EMERGENCY_TRIGGERED: return "triggered";
        default:                     return "idle";
    }
}

int lz_emergency_guard_diag(const lz_emergency_guard_t *g, uint32_t now_ms,
                            char *buf, int n)
{
    if(!buf || n <= 0) return 0;
    lz_emergency_guard_t idle = { LZ_EMERGENCY_IDLE, 0 };
    if(!g) g = &idle;
    return snprintf(buf, (size_t)n,
                    "emergency guard: state=%s hold_ms=%u confirm_ms=%u remaining_ms=%u\n"
                    "flow: hold -> arm -> confirm; confirm only triggers inside the window\n",
                    lz_emergency_state_label(g->state),
                    (unsigned)LZ_EMERGENCY_HOLD_MS,
                    (unsigned)LZ_EMERGENCY_CONFIRM_MS,
                    (unsigned)lz_emergency_guard_remaining(g, now_ms));
}
