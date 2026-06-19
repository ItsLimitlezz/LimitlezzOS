#include "power_policy.h"
#include <stdio.h>

const char *lz_power_state_label(lz_power_state_t state)
{
    switch(state) {
        case LZ_POWER_OK:       return "ok";
        case LZ_POWER_LOW:      return "low";
        case LZ_POWER_CRITICAL: return "critical";
        default:                return "unknown";
    }
}

static bool known_pct(int pct) { return pct >= 0; }
static bool known_mv(int mv) { return mv > 0; }

static void set_state(lz_power_decision_t *d, lz_power_state_t state, const char *reason)
{
    d->state = state;
    d->reason = reason;
}

lz_power_decision_t lz_power_assess(int battery_pct, int voltage_mv,
                                    bool charging, bool usb_power)
{
    lz_power_decision_t d = { LZ_POWER_UNKNOWN, false, false, false, false, false, "unknown" };
    bool pct_known = known_pct(battery_pct);
    bool mv_known = known_mv(voltage_mv);
    bool external_power = charging || usb_power;

    if(!pct_known && !mv_known) return d;

    set_state(&d, LZ_POWER_OK, "normal");
    if(pct_known && battery_pct <= LZ_POWER_CRITICAL_PCT)
        set_state(&d, LZ_POWER_CRITICAL, "percent");
    else if(mv_known && voltage_mv <= LZ_POWER_CRITICAL_MV)
        set_state(&d, LZ_POWER_CRITICAL, "voltage");
    else if(pct_known && battery_pct <= LZ_POWER_LOW_PCT)
        set_state(&d, LZ_POWER_LOW, "percent");
    else if(mv_known && voltage_mv <= LZ_POWER_LOW_MV)
        set_state(&d, LZ_POWER_LOW, "voltage");

    d.notify = d.state == LZ_POWER_LOW || d.state == LZ_POWER_CRITICAL;
    d.wake_screen = d.state == LZ_POWER_CRITICAL && !external_power;
    d.dim_screen = d.state == LZ_POWER_CRITICAL && !external_power;
    d.force_power_save = d.state == LZ_POWER_CRITICAL && !external_power;
    d.allow_buzz = d.state == LZ_POWER_CRITICAL && !external_power;
    return d;
}

static void fmt_pct(char *buf, int n, int pct)
{
    if(pct < 0) snprintf(buf, (size_t)n, "unknown");
    else snprintf(buf, (size_t)n, "%d%%", pct);
}

static void fmt_mv(char *buf, int n, int mv)
{
    if(mv <= 0) snprintf(buf, (size_t)n, "unknown");
    else snprintf(buf, (size_t)n, "%d.%03dV", mv / 1000, mv % 1000);
}

int lz_power_policy_diag(char *buf, int n, int battery_pct, int voltage_mv,
                         bool charging, bool usb_power)
{
    if(!buf || n <= 0) return 0;
    char pct[16], volts[16];
    fmt_pct(pct, sizeof pct, battery_pct);
    fmt_mv(volts, sizeof volts, voltage_mv);
    lz_power_decision_t d = lz_power_assess(battery_pct, voltage_mv, charging, usb_power);
    return snprintf(buf, (size_t)n,
                    "power: %s  battery=%s  voltage=%s  external=%s  reason=%s\n"
                    "actions: notify=%s wake=%s dim=%s power-save=%s buzz=%s\n"
                    "thresholds: low<=%d%%/%dmV critical<=%d%%/%dmV\n",
                    lz_power_state_label(d.state), pct, volts,
                    (charging || usb_power) ? "yes" : "no", d.reason,
                    d.notify ? "yes" : "no",
                    d.wake_screen ? "yes" : "no",
                    d.dim_screen ? "yes" : "no",
                    d.force_power_save ? "yes" : "no",
                    d.allow_buzz ? "yes" : "no",
                    LZ_POWER_LOW_PCT, LZ_POWER_LOW_MV,
                    LZ_POWER_CRITICAL_PCT, LZ_POWER_CRITICAL_MV);
}
