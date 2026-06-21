#ifdef LZ_TARGET_SIM

#include "services/ota_install.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void ota_install_err(char *err, int err_cap, const char *msg)
{
    if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg ? msg : "");
}

static bool g_sim_inactive_written;
static int g_sim_running_slot;
static int g_sim_boot_slot;
static bool g_sim_running_valid = true;

static bool sim_install_fail(lz_ota_install_t *out, char *err, int err_cap,
                             const char *msg)
{
    char msg_copy[48];
    snprintf(msg_copy, sizeof msg_copy, "%s", msg ? msg : "ota install failed");
    ota_install_err(err, err_cap, msg_copy);
    if(out) {
        out->ok = false;
        snprintf(out->error, sizeof out->error, "%s", msg_copy);
    }
    return false;
}

bool lz_ota_install_file_to_inactive(const char *path, uint32_t expected_size,
                                     lz_ota_install_t *out, char *err, int err_cap)
{
    ota_install_err(err, err_cap, "");
    if(out) memset(out, 0, sizeof *out);
    if(!path || !path[0]) return sim_install_fail(out, err, err_cap, "missing candidate");
    if(expected_size == 0 || expected_size > LZ_OTA_SLOT_MAX_BYTES)
        return sim_install_fail(out, err, err_cap, "bad candidate size");

    struct stat st;
    if(stat(path, &st) != 0 || st.st_size < 0)
        return sim_install_fail(out, err, err_cap, "candidate unreadable");
    if((uint32_t)st.st_size != expected_size)
        return sim_install_fail(out, err, err_cap, "size mismatch");

    if(out) {
        out->ok = true;
        out->candidate_valid = true;
        snprintf(out->partition_label, sizeof out->partition_label, "sim_ota_1");
        snprintf(out->running_label, sizeof out->running_label, "sim_ota_0");
        out->bytes_written = expected_size;
        out->partition_size = LZ_OTA_SLOT_MAX_BYTES;
    }
    g_sim_inactive_written = true;
    return true;
}

bool lz_ota_install_running_copy_test(lz_ota_install_t *out, char *err, int err_cap)
{
    ota_install_err(err, err_cap, "");
    if(out) {
        memset(out, 0, sizeof *out);
        out->ok = true;
        out->candidate_valid = true;
        out->copied_running_image = true;
        snprintf(out->partition_label, sizeof out->partition_label, "sim_ota_1");
        snprintf(out->running_label, sizeof out->running_label, "sim_ota_0");
        out->bytes_written = 1536;
        out->partition_size = LZ_OTA_SLOT_MAX_BYTES;
    }
    g_sim_inactive_written = true;
    return true;
}

static void sim_slot_label(char *out, size_t n, int slot)
{
    snprintf(out, n, "sim_ota_%d", slot ? 1 : 0);
}

static bool sim_fill_slot_status(lz_ota_slot_status_t *out)
{
    if(!out) return false;
    memset(out, 0, sizeof *out);
    out->ok = true;
    out->boot_matches_running = g_sim_boot_slot == g_sim_running_slot;
    out->running_pending_verify = !g_sim_running_valid;
    sim_slot_label(out->running_label, sizeof out->running_label, g_sim_running_slot);
    sim_slot_label(out->boot_label, sizeof out->boot_label, g_sim_boot_slot);
    sim_slot_label(out->inactive_label, sizeof out->inactive_label, 1 - g_sim_running_slot);
    snprintf(out->running_state, sizeof out->running_state, "%s",
             g_sim_running_valid ? "valid" : "pending");
    snprintf(out->boot_state, sizeof out->boot_state, "%s",
             g_sim_boot_slot == g_sim_running_slot ? out->running_state : "new");
    out->running_address = g_sim_running_slot ? 0x00510000u : 0x00010000u;
    out->boot_address = g_sim_boot_slot ? 0x00510000u : 0x00010000u;
    out->inactive_address = (1 - g_sim_running_slot) ? 0x00510000u : 0x00010000u;
    return true;
}

bool lz_ota_slot_status(lz_ota_slot_status_t *out, char *err, int err_cap)
{
    ota_install_err(err, err_cap, "");
    if(!out) return false;
    return sim_fill_slot_status(out);
}

bool lz_ota_set_inactive_boot(lz_ota_slot_status_t *out, char *err, int err_cap)
{
    ota_install_err(err, err_cap, "");
    if(!g_sim_inactive_written) {
        if(out) {
            memset(out, 0, sizeof *out);
            snprintf(out->error, sizeof out->error, "inactive image missing");
        }
        ota_install_err(err, err_cap, "inactive image missing");
        return false;
    }
    g_sim_boot_slot = 1 - g_sim_running_slot;
    return sim_fill_slot_status(out);
}

bool lz_ota_mark_running_valid(lz_ota_slot_status_t *out, char *err, int err_cap)
{
    ota_install_err(err, err_cap, "");
    g_sim_running_valid = true;
    return sim_fill_slot_status(out);
}

#endif
