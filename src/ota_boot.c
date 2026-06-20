#include "services/ota_boot.h"
#include <stdio.h>
#include <string.h>

static void boot_reason(lz_ota_boot_decision_t *out, const char *reason)
{
    snprintf(out->reason, sizeof out->reason, "%s", reason);
}

static bool decide(lz_ota_boot_decision_t *out, lz_ota_boot_action_t action,
                   bool apps, bool updates, const char *reason)
{
    out->action = action;
    out->allow_app_launch = apps;
    out->allow_new_update = updates;
    boot_reason(out, reason);
    return true;
}

const char *lz_ota_boot_action_name(lz_ota_boot_action_t action)
{
    switch(action) {
    case LZ_OTA_BOOT_CLEAN:          return "clean";
    case LZ_OTA_BOOT_PENDING_VERIFY: return "pending-verify";
    case LZ_OTA_BOOT_MARK_VALID:     return "mark-valid";
    case LZ_OTA_BOOT_ROLLBACK:       return "rollback";
    default:                         return "unknown";
    }
}

bool lz_ota_boot_decide(const lz_ota_boot_signals_t *signals,
                        lz_ota_boot_decision_t *out)
{
    if(!out) return false;
    memset(out, 0, sizeof *out);
    if(!signals)
        return decide(out, LZ_OTA_BOOT_ROLLBACK, false, false, "missing boot signals");
    if(!signals->pending_verify)
        return decide(out, LZ_OTA_BOOT_CLEAN, true, true, "normal boot");
    if(signals->critical_fault)
        return decide(out, LZ_OTA_BOOT_ROLLBACK, false, false, "critical fault before confirmation");
    if(!signals->boot_selftest_passed)
        return decide(out, LZ_OTA_BOOT_ROLLBACK, false, false, "boot selftest failed");
    if(signals->user_confirmed)
        return decide(out, LZ_OTA_BOOT_MARK_VALID, true, true, "confirmed healthy");
    return decide(out, LZ_OTA_BOOT_PENDING_VERIFY, false, false, "waiting for confirmation");
}

static bool self_check(bool expr, char *err, int err_cap, const char *msg)
{
    if(expr) return true;
    if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg);
    return false;
}

bool lz_ota_boot_selftest(char *err, int err_cap)
{
    if(err && err_cap > 0) err[0] = 0;
    lz_ota_boot_decision_t d;
    lz_ota_boot_signals_t s;

    memset(&s, 0, sizeof s);
    if(!self_check(lz_ota_boot_decide(&s, &d) &&
                   d.action == LZ_OTA_BOOT_CLEAN &&
                   d.allow_app_launch && d.allow_new_update,
                   err, err_cap, "clean boot failed"))
        return false;

    memset(&s, 0, sizeof s);
    s.pending_verify = true;
    s.boot_selftest_passed = true;
    if(!self_check(lz_ota_boot_decide(&s, &d) &&
                   d.action == LZ_OTA_BOOT_PENDING_VERIFY &&
                   !d.allow_app_launch && !d.allow_new_update,
                   err, err_cap, "pending boot failed"))
        return false;

    s.user_confirmed = true;
    if(!self_check(lz_ota_boot_decide(&s, &d) &&
                   d.action == LZ_OTA_BOOT_MARK_VALID &&
                   d.allow_app_launch && d.allow_new_update,
                   err, err_cap, "confirm boot failed"))
        return false;

    memset(&s, 0, sizeof s);
    s.pending_verify = true;
    if(!self_check(lz_ota_boot_decide(&s, &d) &&
                   d.action == LZ_OTA_BOOT_ROLLBACK &&
                   !d.allow_app_launch && !d.allow_new_update &&
                   strcmp(d.reason, "boot selftest failed") == 0,
                   err, err_cap, "failed selftest decision failed"))
        return false;

    s.boot_selftest_passed = true;
    s.user_confirmed = true;
    s.critical_fault = true;
    if(!self_check(lz_ota_boot_decide(&s, &d) &&
                   d.action == LZ_OTA_BOOT_ROLLBACK &&
                   strcmp(d.reason, "critical fault before confirmation") == 0,
                   err, err_cap, "fault decision failed"))
        return false;

    return true;
}
