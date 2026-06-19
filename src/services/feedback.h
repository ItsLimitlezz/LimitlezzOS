/**
 * Feedback policy: one place to decide which user-visible signals are allowed
 * for messages, app notifications, critical system events, OTA, and emergency.
 *
 * Hardware drivers remain outside this module. Callers ask for a decision, then
 * route the resulting screen/keyboard/LED/buzzer intent through the platform.
 */
#ifndef LZ_FEEDBACK_H
#define LZ_FEEDBACK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LZ_FEEDBACK_DND_OFF = 0,
    LZ_FEEDBACK_DND_PRIORITY,
    LZ_FEEDBACK_DND_SILENT,
    LZ_FEEDBACK_DND_EMERGENCY,
    LZ_FEEDBACK_DND_COUNT
} lz_feedback_dnd_t;

typedef enum {
    LZ_FEEDBACK_EVENT_MESSAGE = 0,
    LZ_FEEDBACK_EVENT_DIRECT,
    LZ_FEEDBACK_EVENT_APP,
    LZ_FEEDBACK_EVENT_SYSTEM,
    LZ_FEEDBACK_EVENT_OTA_PROGRESS,
    LZ_FEEDBACK_EVENT_OTA_FAILURE,
    LZ_FEEDBACK_EVENT_EMERGENCY,
    LZ_FEEDBACK_EVENT_COUNT
} lz_feedback_event_t;

typedef struct {
    bool queue;          /* keep the event for later review */
    bool present;        /* show it immediately in the UI/lock surface */
    bool wake_screen;    /* wake or keep the display lit */
    bool pulse_keyboard; /* keyboard backlight pulse, if hardware supports it */
    bool pulse_led;      /* status LED pulse, if hardware supports it */
    bool buzz;           /* audible buzzer/chime, if hardware supports it */
    bool bypass_dnd;     /* critical enough to break through the current DND */
} lz_feedback_decision_t;

int lz_feedback_dnd_clamp(int mode);
int lz_feedback_event_clamp(int event);
const char *lz_feedback_dnd_label(int mode);
const char *lz_feedback_event_label(int event);
lz_feedback_decision_t lz_feedback_decide(int mode, int event);
int lz_feedback_policy_diag(char *buf, int n);

#ifdef __cplusplus
}
#endif

#endif
