#ifdef LZ_TARGET_SIM

#include "services/ota_install.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void ota_install_err(char *err, int err_cap, const char *msg)
{
    if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg ? msg : "");
}

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
    return true;
}

#endif
