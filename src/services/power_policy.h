/**
 * Battery warning policy for the T-Deck.
 *
 * This keeps low/critical battery decisions out of UI and hardware drivers so
 * later feedback code can reuse one tested source of truth.
 */
#ifndef LZ_POWER_POLICY_H
#define LZ_POWER_POLICY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LZ_POWER_LOW_PCT        20
#define LZ_POWER_CRITICAL_PCT    5
#define LZ_POWER_LOW_MV       3650
#define LZ_POWER_CRITICAL_MV  3500

typedef enum {
    LZ_POWER_UNKNOWN = 0,
    LZ_POWER_OK,
    LZ_POWER_LOW,
    LZ_POWER_CRITICAL
} lz_power_state_t;

typedef struct {
    lz_power_state_t state;
    bool notify;
    bool wake_screen;
    bool dim_screen;
    bool force_power_save;
    bool allow_buzz;
    const char *reason;
} lz_power_decision_t;

const char *lz_power_state_label(lz_power_state_t state);
lz_power_decision_t lz_power_assess(int battery_pct, int voltage_mv,
                                    bool charging, bool usb_power);
int lz_power_policy_diag(char *buf, int n, int battery_pct, int voltage_mv,
                         bool charging, bool usb_power);

#ifdef __cplusplus
}
#endif

#endif
