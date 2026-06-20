#include "feedback.h"
#include "mesh.h"
#include <stdio.h>
#include <string.h>

extern uint32_t lz_tick_ms(void);

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

static lz_feedback_status_t g_feedback;

static void bounded_copy(char *out, size_t cap, const char *src)
{
    if(!out || cap == 0) return;
    if(!src) src = "";
    size_t j = 0;
    while(*src && j + 1 < cap) {
        char c = *src++;
        if(c == '\r' || c == '\n' || c < 32) c = ' ';
        out[j++] = c;
    }
    while(j > 0 && out[j - 1] == ' ') j--;
    out[j] = 0;
}

bool lz_svc_feedback_notify(const char *source, const char *title, const char *body)
{
    if(!title || !title[0]) title = "Notification";
    if(!body || !body[0]) body = "App requested attention";
    if(!source || !source[0]) source = "system";

    g_feedback.request_count++;
    g_feedback.last_ms = lz_tick_ms();
    bounded_copy(g_feedback.last_source, sizeof g_feedback.last_source, source);
    bounded_copy(g_feedback.last_title, sizeof g_feedback.last_title, title);
    bounded_copy(g_feedback.last_body, sizeof g_feedback.last_body, body);
    return true;
}

void lz_svc_feedback_status(lz_feedback_status_t *out)
{
    if(!out) return;
    *out = g_feedback;
}

int lz_svc_feedback_diag(char *buf, int n)
{
    if(!buf || n <= 0) return 0;
    if(g_feedback.request_count == 0) {
        snprintf(buf, (size_t)n, "feedback: ready requests=0\n");
    } else {
        snprintf(buf, (size_t)n,
                 "feedback: ready requests=%lu last_ms=%lu source=%s title=\"%s\" body=\"%s\"\n",
                 (unsigned long)g_feedback.request_count,
                 (unsigned long)g_feedback.last_ms,
                 g_feedback.last_source[0] ? g_feedback.last_source : "-",
                 g_feedback.last_title,
                 g_feedback.last_body);
    }
    return (int)strlen(buf);
}

int lz_svc_feedback_selftest(char *buf, int n)
{
    uint32_t before = g_feedback.request_count;
    bool ok = lz_svc_feedback_notify("selftest", "Feedback test", "notification route ok");
    bool advanced = g_feedback.request_count == before + 1;
    bool kept = strcmp(g_feedback.last_source, "selftest") == 0 &&
                strcmp(g_feedback.last_title, "Feedback test") == 0;
    if(buf && n > 0) {
        snprintf(buf, (size_t)n, "Feedback selftest: %s requests=%lu",
                 (ok && advanced && kept) ? "PASS" : "FAIL",
                 (unsigned long)g_feedback.request_count);
    }
    return ok && advanced && kept ? 1 : 0;
}
