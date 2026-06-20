#ifndef LZ_OTA_BOOT_H
#define LZ_OTA_BOOT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LZ_OTA_BOOT_CLEAN = 0,
    LZ_OTA_BOOT_PENDING_VERIFY,
    LZ_OTA_BOOT_MARK_VALID,
    LZ_OTA_BOOT_ROLLBACK
} lz_ota_boot_action_t;

typedef struct {
    bool pending_verify;        /* booted into a newly selected image */
    bool boot_selftest_passed;  /* local startup checks passed */
    bool user_confirmed;        /* user accepted the new image */
    bool critical_fault;        /* device entered a fault before confirmation */
} lz_ota_boot_signals_t;

typedef struct {
    lz_ota_boot_action_t action;
    bool allow_app_launch;
    bool allow_new_update;
    char reason[64];
} lz_ota_boot_decision_t;

const char *lz_ota_boot_action_name(lz_ota_boot_action_t action);
bool lz_ota_boot_decide(const lz_ota_boot_signals_t *signals,
                        lz_ota_boot_decision_t *out);
bool lz_ota_boot_selftest(char *err, int err_cap);

#ifdef __cplusplus
}
#endif

#endif
