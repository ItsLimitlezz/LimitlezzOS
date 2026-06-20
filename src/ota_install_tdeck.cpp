#ifdef LZ_TARGET_TDECK

#include "services/ota_install.h"
#include <Arduino.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ota_install_err(char *err, int err_cap, const char *msg)
{
    if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg ? msg : "");
}

static bool install_fail(lz_ota_install_t *out, char *err, int err_cap,
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

static void fill_partitions(lz_ota_install_t *out, const esp_partition_t *running,
                            const esp_partition_t *update)
{
    if(!out) return;
    if(running) snprintf(out->running_label, sizeof out->running_label, "%s", running->label);
    if(update) {
        snprintf(out->partition_label, sizeof out->partition_label, "%s", update->label);
        out->partition_address = update->address;
        out->partition_size = update->size;
    }
}

static bool write_stream_to_partition(FILE *f, uint32_t expected_size,
                                      lz_ota_install_t *out, char *err, int err_cap)
{
    if(!f) return install_fail(out, err, err_cap, "candidate unreadable");
    if(expected_size == 0 || expected_size > LZ_OTA_SLOT_MAX_BYTES)
        return install_fail(out, err, err_cap, "bad candidate size");

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    fill_partitions(out, running, update);
    if(!running || !update)
        return install_fail(out, err, err_cap, "ota partition unavailable");
    if(expected_size > update->size)
        return install_fail(out, err, err_cap, "candidate too large");

    esp_ota_handle_t handle = 0;
    esp_err_t e = esp_ota_begin(update, expected_size, &handle);
    if(e != ESP_OK)
        return install_fail(out, err, err_cap, "ota begin failed");

    uint8_t *buf = (uint8_t *)malloc(1024);
    if(!buf) {
        esp_ota_abort(handle);
        return install_fail(out, err, err_cap, "ota buffer unavailable");
    }

    bool ok = true;
    uint32_t total = 0;
    while(total < expected_size) {
        uint32_t left = expected_size - total;
        size_t want = left < 1024u ? (size_t)left : 1024u;
        size_t got = fread(buf, 1, want, f);
        if(got == 0) {
            ok = false;
            ota_install_err(err, err_cap, ferror(f) ? "candidate read failed" : "size mismatch");
            break;
        }
        e = esp_ota_write(handle, buf, got);
        if(e != ESP_OK) {
            ok = false;
            ota_install_err(err, err_cap, "ota write failed");
            break;
        }
        total += (uint32_t)got;
        delay(0);
    }
    free(buf);

    if(ok) {
        int extra = fgetc(f);
        if(extra != EOF) {
            ok = false;
            ota_install_err(err, err_cap, "candidate too large");
        }
    }

    if(!ok) {
        esp_ota_abort(handle);
        return install_fail(out, err, err_cap, err && err[0] ? err : "ota write failed");
    }

    e = esp_ota_end(handle);
    if(e != ESP_OK)
        return install_fail(out, err, err_cap, "ota image invalid");

    if(out) {
        out->ok = true;
        out->candidate_valid = true;
        out->boot_partition_set = false;
        out->bytes_written = total;
        out->error[0] = 0;
    }
    return true;
}

bool lz_ota_install_file_to_inactive(const char *path, uint32_t expected_size,
                                     lz_ota_install_t *out, char *err, int err_cap)
{
    ota_install_err(err, err_cap, "");
    if(out) memset(out, 0, sizeof *out);
    if(!path || !path[0]) return install_fail(out, err, err_cap, "missing candidate");

    FILE *f = fopen(path, "rb");
    if(!f) return install_fail(out, err, err_cap, "candidate unreadable");
    bool ok = write_stream_to_partition(f, expected_size, out, err, err_cap);
    fclose(f);
    return ok;
}

bool lz_ota_install_running_copy_test(lz_ota_install_t *out, char *err, int err_cap)
{
    ota_install_err(err, err_cap, "");
    if(out) memset(out, 0, sizeof *out);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    fill_partitions(out, running, update);
    if(!running || !update)
        return install_fail(out, err, err_cap, "ota partition unavailable");

    esp_partition_pos_t pos;
    pos.offset = running->address;
    pos.size = running->size;
    esp_image_metadata_t meta;
    if(esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK ||
       meta.image_len == 0 || meta.image_len > running->size ||
       meta.image_len > update->size)
        return install_fail(out, err, err_cap, "running image invalid");

    esp_ota_handle_t handle = 0;
    esp_err_t e = esp_ota_begin(update, meta.image_len, &handle);
    if(e != ESP_OK)
        return install_fail(out, err, err_cap, "ota begin failed");

    uint8_t *buf = (uint8_t *)malloc(1024);
    if(!buf) {
        esp_ota_abort(handle);
        return install_fail(out, err, err_cap, "ota buffer unavailable");
    }

    bool ok = true;
    uint32_t total = 0;
    while(total < meta.image_len) {
        uint32_t left = meta.image_len - total;
        size_t want = left < 1024u ? (size_t)left : 1024u;
        e = esp_partition_read(running, total, buf, want);
        if(e != ESP_OK) {
            ok = false;
            ota_install_err(err, err_cap, "running image read failed");
            break;
        }
        e = esp_ota_write(handle, buf, want);
        if(e != ESP_OK) {
            ok = false;
            ota_install_err(err, err_cap, "ota write failed");
            break;
        }
        total += (uint32_t)want;
        delay(0);
    }
    free(buf);

    if(!ok) {
        esp_ota_abort(handle);
        return install_fail(out, err, err_cap, err && err[0] ? err : "ota write failed");
    }

    e = esp_ota_end(handle);
    if(e != ESP_OK)
        return install_fail(out, err, err_cap, "ota image invalid");

    if(out) {
        out->ok = true;
        out->candidate_valid = true;
        out->copied_running_image = true;
        out->boot_partition_set = false;
        out->bytes_written = total;
        out->error[0] = 0;
    }
    return true;
}

#endif
