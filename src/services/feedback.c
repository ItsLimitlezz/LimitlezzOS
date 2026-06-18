#include "mesh.h"
#include <stdio.h>
#include <string.h>

extern uint32_t lz_tick_ms(void);

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
