#include "feedback.h"
#include <stdio.h>
#include <string.h>

int lz_feedback_dnd_clamp(int mode)
{
    return (mode >= 0 && mode < LZ_FEEDBACK_DND_COUNT) ? mode : LZ_FEEDBACK_DND_OFF;
}

int lz_feedback_event_clamp(int event)
{
    return (event >= 0 && event < LZ_FEEDBACK_EVENT_COUNT) ? event : LZ_FEEDBACK_EVENT_MESSAGE;
}

const char *lz_feedback_dnd_label(int mode)
{
    switch(lz_feedback_dnd_clamp(mode)) {
        case LZ_FEEDBACK_DND_PRIORITY:  return "priority";
        case LZ_FEEDBACK_DND_SILENT:    return "silent";
        case LZ_FEEDBACK_DND_EMERGENCY: return "emergency";
        default:                        return "off";
    }
}

const char *lz_feedback_event_label(int event)
{
    switch(lz_feedback_event_clamp(event)) {
        case LZ_FEEDBACK_EVENT_DIRECT:       return "direct";
        case LZ_FEEDBACK_EVENT_APP:          return "app";
        case LZ_FEEDBACK_EVENT_SYSTEM:       return "system";
        case LZ_FEEDBACK_EVENT_OTA_PROGRESS: return "ota-progress";
        case LZ_FEEDBACK_EVENT_OTA_FAILURE:  return "ota-failure";
        case LZ_FEEDBACK_EVENT_EMERGENCY:    return "emergency";
        default:                             return "message";
    }
}

static bool event_is_priority(int event)
{
    return event == LZ_FEEDBACK_EVENT_DIRECT ||
           event == LZ_FEEDBACK_EVENT_SYSTEM ||
           event == LZ_FEEDBACK_EVENT_OTA_FAILURE ||
           event == LZ_FEEDBACK_EVENT_EMERGENCY;
}

static bool event_is_critical(int event)
{
    return event == LZ_FEEDBACK_EVENT_SYSTEM ||
           event == LZ_FEEDBACK_EVENT_OTA_FAILURE ||
           event == LZ_FEEDBACK_EVENT_EMERGENCY;
}

static bool event_is_audible(int event, int mode)
{
    if(event == LZ_FEEDBACK_EVENT_EMERGENCY) return true;
    if(event == LZ_FEEDBACK_EVENT_SYSTEM || event == LZ_FEEDBACK_EVENT_OTA_FAILURE)
        return mode != LZ_FEEDBACK_DND_SILENT;
    if(event == LZ_FEEDBACK_EVENT_DIRECT)
        return mode == LZ_FEEDBACK_DND_OFF;
    return false;
}

lz_feedback_decision_t lz_feedback_decide(int mode, int event)
{
    mode = lz_feedback_dnd_clamp(mode);
    event = lz_feedback_event_clamp(event);

    bool allow = false;
    switch(mode) {
        case LZ_FEEDBACK_DND_PRIORITY:
            allow = event_is_priority(event);
            break;
        case LZ_FEEDBACK_DND_SILENT:
            allow = event_is_critical(event);
            break;
        case LZ_FEEDBACK_DND_EMERGENCY:
            allow = event == LZ_FEEDBACK_EVENT_EMERGENCY;
            break;
        default:
            allow = true;
            break;
    }

    lz_feedback_decision_t d;
    memset(&d, 0, sizeof d);
    d.queue = true;
    d.present = allow;
    d.wake_screen = allow;
    d.pulse_keyboard = allow;
    d.pulse_led = allow;
    d.buzz = allow && event_is_audible(event, mode);
    d.bypass_dnd = mode != LZ_FEEDBACK_DND_OFF && allow && event_is_critical(event);
    return d;
}

static const char *decision_word(const lz_feedback_decision_t *d, char *buf, int n)
{
    if(!d->present) return "queued";
    snprintf(buf, (size_t)n, "show%s%s",
             d->buzz ? "+buzz" : "",
             d->bypass_dnd ? "+bypass" : "");
    return buf;
}

static void append(char *buf, int n, int *used, const char *text)
{
    if(!buf || n <= 0 || !used || !text || *used >= n) return;
    int wrote = snprintf(buf + *used, (size_t)(n - *used), "%s", text);
    if(wrote < 0) return;
    *used += wrote;
    if(*used >= n) *used = n - 1;
}

int lz_feedback_policy_diag(char *buf, int n)
{
    if(!buf || n <= 0) return 0;
    buf[0] = 0;
    int used = 0;
    append(buf, n, &used, "feedback policy:\n");
    for(int mode = 0; mode < LZ_FEEDBACK_DND_COUNT; mode++) {
        char line[260], word[32], word2[32], word3[32], word4[32], word5[32];
        lz_feedback_decision_t message = lz_feedback_decide(mode, LZ_FEEDBACK_EVENT_MESSAGE);
        lz_feedback_decision_t direct = lz_feedback_decide(mode, LZ_FEEDBACK_EVENT_DIRECT);
        lz_feedback_decision_t system = lz_feedback_decide(mode, LZ_FEEDBACK_EVENT_SYSTEM);
        lz_feedback_decision_t ota = lz_feedback_decide(mode, LZ_FEEDBACK_EVENT_OTA_FAILURE);
        lz_feedback_decision_t emergency = lz_feedback_decide(mode, LZ_FEEDBACK_EVENT_EMERGENCY);
        snprintf(line, sizeof line,
                 "  %s: message=%s direct=%s system=%s ota-failure=%s emergency=%s\n",
                 lz_feedback_dnd_label(mode),
                 decision_word(&message, word, sizeof word),
                 decision_word(&direct, word2, sizeof word2),
                 decision_word(&system, word3, sizeof word3),
                 decision_word(&ota, word4, sizeof word4),
                 decision_word(&emergency, word5, sizeof word5));
        append(buf, n, &used, line);
    }
    return used;
}
