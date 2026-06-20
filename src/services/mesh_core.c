/**
 * Mesh service core — owns the node table and thread index, mediates
 * between the radio backend and the UI. Backend-agnostic: the sim and the
 * real SX1262 driver both call the lz_core_on_* hooks below.
 */
#include "mesh.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#ifdef LZ_TARGET_TDECK
#include "esp_heap_caps.h"        /* node DB lives in PSRAM to save internal DRAM */
#endif

/* store.c */
void lz_store_init(const char *datadir);
const char *lz_store_file_root(void);
void lz_store_set_appfs_root(const char *root);
const char *lz_store_appfs_root(void);
int  lz_store_file_roots(const char **out, int cap);
int  lz_store_scan_apps(lz_local_app_t *out, int cap);
int  lz_store_scan_app_issues(lz_local_app_issue_t *out, int cap);
bool lz_store_prepare_app_data(const lz_local_app_t *app, char *path_out, int path_cap,
                               char *err, int err_cap);
bool lz_store_app_data_usage(const lz_local_app_t *app, uint32_t *used, uint32_t *quota,
                             char *err, int err_cap);
bool lz_store_clear_app_data(const lz_local_app_t *app, char *err, int err_cap);
bool lz_store_uninstall_local_app(const lz_local_app_t *app, bool keep_data,
                                  char *err, int err_cap);
bool lz_store_save_app_catalog_cache(const char *json, int len, char *err, int err_cap);
bool lz_store_load_app_catalog_cache(char *out, int cap, int *out_len, char *err, int err_cap);
bool lz_store_clear_app_catalog_cache(char *err, int err_cap);
bool lz_store_start_local_app(const lz_local_app_t *app, lz_local_app_session_t *out);
bool lz_store_local_app_action(lz_local_app_session_t *session, int idx);
void lz_store_stop_local_app(lz_local_app_session_t *session);
bool lz_store_validate_app_catalog_json(const char *json, lz_app_catalog_report_t *out);
int  lz_store_app_catalog_diag(char *buf, int n);
int  lz_store_app_catalog_selftest(char *buf, int n);
bool lz_store_ota_manifest_status(lz_ota_manifest_t *out);
int  lz_store_ota_manifest_selftest(char *buf, int n);
bool lz_store_security_status(lz_security_status_t *out);
bool lz_store_security_set_pin(const char *pin, char *err, int err_cap);
bool lz_store_security_check_pin(const char *pin);
bool lz_store_security_clear_pin(const char *pin, char *err, int err_cap);
int  lz_store_security_selftest(char *buf, int n);
void lz_store_append(const char *addr, const lz_msg_rt *m);
int  lz_store_load_tail(const char *addr, lz_msg_rt *ring, int cap);
bool lz_store_find_delivery(const char *addr, uint32_t pkt_id, lz_msg_rt *out);
bool lz_store_update_delivery(const char *addr, uint32_t old_pkt_id,
                              uint32_t new_pkt_id, uint8_t status,
                              uint8_t retries, uint8_t fail_reason);
void lz_store_save_threads(const lz_thread_rt *t, int n);
int  lz_store_load_threads(lz_thread_rt *out, int cap);
void lz_store_save_nodes(const lz_node_rt *nodes, int n);
int  lz_store_load_nodes(lz_node_rt *out, int cap);
void lz_store_save_identity(const char *longn, const char *shortn);
bool lz_store_load_identity(char *longn, int ln, char *shortn, int sn);

/* monotonic clock supplied by the platform layer */
extern uint32_t lz_tick_ms(void);

/* MeshCore TX (real backend on the T-Deck; absent in the sim -> weak/NULL) */
extern bool lz_backend_mc_send_public(const char *text) __attribute__((weak));
extern bool lz_backend_mc_dm_key(const uint8_t peer_pub[32],
                                 const char *name_hint,
                                 const char *text) __attribute__((weak));
extern bool lz_backend_mc_pubkey(uint8_t out32[32]) __attribute__((weak));

static lz_node_rt   *g_nodes;     /* 45 KB node DB — PSRAM-backed on T-Deck (alloc in lz_svc_init) */
static int           g_node_count;
static lz_thread_rt  g_threads[LZ_MAX_THREADS];   /* stable: never reordered */
static int           g_thread_count;
static int           g_order[LZ_MAX_THREADS];     /* display order, newest first */
static lz_msg_rt     g_tail[LZ_TAIL_MAX];
static int           g_tail_count;
static lz_thread_rt *g_open;
static void        (*g_dirty)(void);
static uint32_t      g_pkt_seq;
static bool          g_nodes_dirty;        /* node DB has unsaved high-freq fields */
static uint32_t      g_nodes_dirty_ms;     /* tick when it first became dirty */
/* placeholder until onboarding sets the real name (and MAC sets the num) */
static lz_identity_t g_id = { 0x7c3af1d0, "!7c3af1d0", "Node", "NODE" };
static bool          g_have_identity;            /* false until onboarding done */
static uint32_t      g_epoch_base = 1718200980;  /* UTC at uptime 0 */
static int           g_tz_std_min;               /* standard-time offset, minutes */
static int           g_dst_rule;                 /* LZ_DST_NONE / _US / _EU */
static char          g_tz_std_ab[6] = "UTC";     /* abbrev in standard time */
static char          g_tz_dst_ab[6] = "UTC";     /* abbrev in daylight time */

#define LZ_DELIVERY_PEND 16
#define LZ_DELIVERY_ACK_TIMEOUT_MS 30000u
static struct {
    bool used;
    uint32_t pkt_id;
    uint32_t sent_ms;
    uint8_t retries;
    char addr[16];
} g_delivery[LZ_DELIVERY_PEND];

static lz_thread_rt *find_thread(uint32_t num);

/* ---------- helpers ---------- */

static uint32_t now_epoch(void)                  /* UTC */
{
    return g_epoch_base + lz_tick_ms() / 1000;
}

static bool local_app_has_token(const char *text, const char *token)
{
    return text && token && strstr(text, token) != NULL;
}

static bool local_app_session_has_token(const lz_local_app_session_t *s, const char *token)
{
    if(!s) return false;
    if(local_app_has_token(s->status, token) || local_app_has_token(s->body, token))
        return true;
    for(int i = 0; i < s->action_count; i++) {
        if(local_app_has_token(s->actions[i].status, token) ||
           local_app_has_token(s->actions[i].body, token))
            return true;
    }
    return false;
}

static bool local_app_token_fail(lz_local_app_session_t *s, const char *msg)
{
    if(s) {
        snprintf(s->status, sizeof s->status, "Launch blocked");
        snprintf(s->body, sizeof s->body, "%s", msg);
        snprintf(s->error, sizeof s->error, "%s", msg);
    }
    return false;
}

static void local_app_append_text(char *out, size_t cap, size_t *len, const char *text)
{
    if(!out || !len || !text || cap == 0) return;
    while(*text && *len + 1 < cap) out[(*len)++] = *text++;
}

static void local_app_expand_text(char *text, size_t cap,
                                  const char *time_s, const char *battery_s)
{
    if(!text || cap == 0 ||
       (!local_app_has_token(text, "{time}") && !local_app_has_token(text, "{battery}")))
        return;

    char src[LZ_LOCAL_APP_BODY_MAX];
    snprintf(src, sizeof src, "%s", text);
    char expanded[LZ_LOCAL_APP_BODY_MAX];
    size_t out = 0;
    const char *p = src;
    while(*p && out + 1 < cap) {
        if(strncmp(p, "{time}", 6) == 0) {
            local_app_append_text(expanded, cap, &out, time_s);
            p += 6;
        } else if(strncmp(p, "{battery}", 9) == 0) {
            local_app_append_text(expanded, cap, &out, battery_s);
            p += 9;
        } else {
            expanded[out++] = *p++;
        }
    }
    expanded[out] = 0;
    snprintf(text, cap, "%s", expanded);
}

static void local_app_battery_token(char *out, size_t cap)
{
    lz_sysinfo_t si;
    lz_svc_sysinfo(&si);
    if(si.battery_pct >= 0) snprintf(out, cap, "%d%%", si.battery_pct);
    else if(si.battery_v > 0.0f) snprintf(out, cap, "%.2fV", (double)si.battery_v);
    else if(si.usb) snprintf(out, cap, "USB");
    else snprintf(out, cap, "unknown");
}

static bool local_app_expand_session(lz_local_app_session_t *s)
{
    if(!s) return false;
    if(s->error[0]) {
        lz_local_app_runtime_refresh(s);
        return true;
    }
    bool need_time = local_app_session_has_token(s, "{time}");
    bool need_battery = local_app_session_has_token(s, "{battery}");
    if(need_time && (s->permissions & LZ_APP_PERM_SYSTEM_TIME) == 0)
        return local_app_token_fail(s, "time permission missing");
    if(need_battery && (s->permissions & LZ_APP_PERM_BATTERY) == 0)
        return local_app_token_fail(s, "battery permission missing");

    char time_s[16], battery_s[16];
    lz_fmt_now(time_s, sizeof time_s);
    local_app_battery_token(battery_s, sizeof battery_s);
    local_app_expand_text(s->status, sizeof s->status, time_s, battery_s);
    local_app_expand_text(s->body, sizeof s->body, time_s, battery_s);
    return lz_local_app_runtime_within_budget(s);
}

static uint32_t next_packet_id(void)
{
    uint32_t pid = lz_tick_ms();
    if(!pid) pid = 1;
    if(pid <= g_pkt_seq) pid = g_pkt_seq + 1;
    if(!pid) pid = 1;
    g_pkt_seq = pid;
    return pid;
}

static bool delivery_track(const char *addr, uint32_t pkt_id, uint8_t retries)
{
    if(!pkt_id) return false;
    int oldest = -1;
    for(int i = 0; i < LZ_DELIVERY_PEND; i++) {
        if(g_delivery[i].used && g_delivery[i].pkt_id == pkt_id) {
            snprintf(g_delivery[i].addr, sizeof g_delivery[i].addr, "%s", addr);
            g_delivery[i].sent_ms = lz_tick_ms();
            g_delivery[i].retries = retries;
            return true;
        }
        if(g_delivery[i].used &&
           (oldest < 0 || (int32_t)(g_delivery[i].sent_ms - g_delivery[oldest].sent_ms) < 0))
            oldest = i;
    }
    int slot = -1;
    for(int i = 0; i < LZ_DELIVERY_PEND; i++)
        if(!g_delivery[i].used) { slot = i; break; }
    /* table full: evict the oldest pending so the newest send is never dropped
     * (a silently-untracked DM would sit in SENDING forever). The evicted one
     * is still persisted as SENDING and re-tracked on reopen/reboot. */
    if(slot < 0) slot = oldest;
    if(slot < 0) return false;
    g_delivery[slot].used = true;
    g_delivery[slot].pkt_id = pkt_id;
    g_delivery[slot].sent_ms = lz_tick_ms();
    g_delivery[slot].retries = retries;
    snprintf(g_delivery[slot].addr, sizeof g_delivery[slot].addr, "%s", addr);
    return true;
}

static const char *delivery_addr(uint32_t pkt_id)
{
    for(int i = 0; i < LZ_DELIVERY_PEND; i++)
        if(g_delivery[i].used && g_delivery[i].pkt_id == pkt_id) return g_delivery[i].addr;
    return NULL;
}

static uint8_t delivery_retries(uint32_t pkt_id)
{
    for(int i = 0; i < LZ_DELIVERY_PEND; i++)
        if(g_delivery[i].used && g_delivery[i].pkt_id == pkt_id) return g_delivery[i].retries;
    return 0;
}

static void delivery_forget(uint32_t pkt_id)
{
    for(int i = 0; i < LZ_DELIVERY_PEND; i++)
        if(g_delivery[i].used && g_delivery[i].pkt_id == pkt_id) {
            g_delivery[i].used = false;
            return;
        }
}

const char *lz_svc_delivery_fail_label(uint8_t reason)
{
    switch(reason) {
        case LZ_FAIL_RADIO_SEND:  return "radio send";
        case LZ_FAIL_ACK_TIMEOUT: return "ack timeout";
        case LZ_FAIL_RETRY_LIMIT: return "retry limit";
        default:                  return "failed";
    }
}

static const char *delivery_status_label(uint8_t status)
{
    switch(status) {
        case LZ_MSG_SENDING:   return "sending";
        case LZ_MSG_DELIVERED: return "delivered";
        case LZ_MSG_FAILED:    return "failed";
        default:               return "none";
    }
}

static int buf_appendf(char *buf, int n, int pos, const char *fmt, ...)
{
    if(!buf || n <= 0 || pos >= n) return pos;
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(buf + pos, (size_t)(n - pos), fmt, ap);
    va_end(ap);
    if(wrote < 0) return pos;
    pos += wrote;
    if(pos >= n) pos = n - 1;
    return pos;
}

/* days since 1970-01-01 for a civil date (Howard Hinnant's algorithm) */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + doe - 719468;
}

/* 0=Sunday .. 6=Saturday for a day-count since the 1970 epoch (a Thursday) */
static int weekday(long days) { return (int)(((days % 7) + 4 + 7) % 7); }
static int nth_sunday(int y, int m, int nth)     /* day-of-month of the Nth Sunday */
{
    int wd = weekday(days_from_civil(y, m, 1));
    return 1 + ((7 - wd) % 7) + (nth - 1) * 7;
}
static int last_sunday(int y, int m, int last_day)
{
    return last_day - weekday(days_from_civil(y, m, last_day));
}

static void lz_gmtime(const time_t *t, struct tm *out)
{
#if defined(_WIN32)
    gmtime_s(out, t);
#else
    gmtime_r(t, out);
#endif
}

/* is daylight saving in effect at this UTC instant for the active zone? */
static bool dst_at(uint32_t utc)
{
    if(g_dst_rule == LZ_DST_NONE) return false;
    if(g_dst_rule == LZ_DST_US) {
        /* 2nd Sun Mar 02:00 .. 1st Sun Nov 02:00, in local standard time */
        time_t t = (time_t)(utc + g_tz_std_min * 60);
        struct tm tm; lz_gmtime(&t, &tm);
        int m = tm.tm_mon + 1, d = tm.tm_mday, h = tm.tm_hour;
        int y = tm.tm_year + 1900;
        if(m < 3 || m > 11) return false;
        if(m > 3 && m < 11) return true;
        if(m == 3) { int s = nth_sunday(y, 3, 2);  return d > s || (d == s && h >= 2); }
        int e = nth_sunday(y, 11, 1);              return d < e || (d == e && h < 2);
    }
    /* EU: last Sun Mar 01:00 UTC .. last Sun Oct 01:00 UTC */
    time_t t = (time_t)utc;
    struct tm tm; lz_gmtime(&t, &tm);
    int m = tm.tm_mon + 1, d = tm.tm_mday, h = tm.tm_hour, y = tm.tm_year + 1900;
    if(m < 3 || m > 10) return false;
    if(m > 3 && m < 10) return true;
    if(m == 3) { int s = last_sunday(y, 3, 31);  return d > s || (d == s && h >= 1); }
    int e = last_sunday(y, 10, 31);              return d < e || (d == e && h < 1);
}

static int eff_tz_min(void) { return g_tz_std_min + (dst_at(now_epoch()) ? 60 : 0); }
static uint32_t now_local(void) { return now_epoch() + eff_tz_min() * 60; }

const lz_identity_t *lz_svc_identity(void) { return &g_id; }

bool lz_svc_needs_onboarding(void) { return !g_have_identity; }

void lz_svc_set_dirty_cb(void (*cb)(void)) { g_dirty = cb; }
static void mark_dirty(void) { if(g_dirty) g_dirty(); }

/* Position/telemetry/battery arrive far more often than NodeInfo and each used
 * to rewrite the whole nodes.db synchronously from the RX path (flash wear +
 * dropped packets). Coalesce those into one bounded periodic flush. */
#define LZ_NODES_FLUSH_MS 5000u
static void nodes_save_now(void)  { lz_store_save_nodes(g_nodes, g_node_count); g_nodes_dirty = false; }
static void nodes_mark_dirty(void){ if(!g_nodes_dirty) { g_nodes_dirty = true; g_nodes_dirty_ms = lz_tick_ms(); } }
static void nodes_flush(void)
{
    if(g_nodes_dirty && (uint32_t)(lz_tick_ms() - g_nodes_dirty_ms) >= LZ_NODES_FLUSH_MS)
        nodes_save_now();
}

const char *lz_svc_file_root(void)
{
    return lz_store_file_root();
}

void lz_svc_set_appfs_root(const char *root)
{
    lz_store_set_appfs_root(root);
}

int lz_svc_file_roots(const char **out, int cap)
{
    return lz_store_file_roots(out, cap);
}

int lz_svc_scan_apps(lz_local_app_t *out, int cap)
{
    return lz_store_scan_apps(out, cap);
}

int lz_svc_scan_app_issues(lz_local_app_issue_t *out, int cap)
{
    return lz_store_scan_app_issues(out, cap);
}

bool lz_svc_prepare_app_data(const lz_local_app_t *app, char *path_out, int path_cap,
                             char *err, int err_cap)
{
    return lz_store_prepare_app_data(app, path_out, path_cap, err, err_cap);
}

bool lz_svc_app_data_usage(const lz_local_app_t *app, uint32_t *used, uint32_t *quota,
                           char *err, int err_cap)
{
    return lz_store_app_data_usage(app, used, quota, err, err_cap);
}

bool lz_svc_clear_app_data(const lz_local_app_t *app, char *err, int err_cap)
{
    return lz_store_clear_app_data(app, err, err_cap);
}

bool lz_svc_uninstall_local_app(const lz_local_app_t *app, bool keep_data,
                                char *err, int err_cap)
{
    return lz_store_uninstall_local_app(app, keep_data, err, err_cap);
    }
bool lz_svc_save_app_catalog_cache(const char *json, int len, char *err, int err_cap)
{
    return lz_store_save_app_catalog_cache(json, len, err, err_cap);
}

bool lz_svc_load_app_catalog_cache(char *out, int cap, int *out_len, char *err, int err_cap)
{
    return lz_store_load_app_catalog_cache(out, cap, out_len, err, err_cap);
}

bool lz_svc_clear_app_catalog_cache(char *err, int err_cap)
{
    return lz_store_clear_app_catalog_cache(err, err_cap);
}

bool lz_svc_start_local_app(const lz_local_app_t *app, lz_local_app_session_t *out)
{
    if(!lz_store_start_local_app(app, out)) return false;
    return local_app_expand_session(out);
}

static bool local_app_notify_effect(const char *effect, char *body, size_t cap)
{
    if(!effect || !body || cap == 0) return false;
    if(strncmp(effect, "notify:", 7) != 0 || !effect[7]) return false;
    const char *src = effect + 7;
    while(*src == ' ' || *src == '\t') src++;
    size_t j = 0;
    while(*src && j + 1 < cap) {
        char c = *src++;
        if(c == '\r' || c == '\n' || c < 32) c = ' ';
        body[j++] = c;
    }
    while(j > 0 && body[j - 1] == ' ') j--;
    body[j] = 0;
    return body[0] != 0;
}

bool lz_svc_local_app_action(lz_local_app_session_t *session, int idx)
{
    if(!lz_store_local_app_action(session, idx)) return false;
    bool ok = local_app_expand_session(session);
    if(ok && idx >= 0 && idx < session->action_count) {
        char note[LZ_FEEDBACK_BODY_MAX];
        if(local_app_notify_effect(session->actions[idx].effect, note, sizeof note)) {
            lz_svc_feedback_notify(session->title, session->actions[idx].label, note);
        }
    }
    return ok;
}

void lz_svc_stop_local_app(lz_local_app_session_t *session)
{
    lz_store_stop_local_app(session);
}

bool lz_svc_validate_app_catalog_json(const char *json, lz_app_catalog_report_t *out)
{
    return lz_store_validate_app_catalog_json(json, out);
}

int lz_svc_app_catalog_diag(char *buf, int n)
{
    return lz_store_app_catalog_diag(buf, n);
}

int lz_svc_app_catalog_selftest(char *buf, int n)
{
    return lz_store_app_catalog_selftest(buf, n);
}

bool lz_svc_ota_manifest_status(lz_ota_manifest_t *out)
{
    return lz_store_ota_manifest_status(out);
}

int lz_svc_ota_manifest_selftest(char *buf, int n)
{
    return lz_store_ota_manifest_selftest(buf, n);
}

bool lz_svc_security_status(lz_security_status_t *out)
{
    return lz_store_security_status(out);
}

bool lz_svc_security_set_pin(const char *pin, char *err, int err_cap)
{
    return lz_store_security_set_pin(pin, err, err_cap);
}

bool lz_svc_security_check_pin(const char *pin)
{
    return lz_store_security_check_pin(pin);
}

bool lz_svc_security_clear_pin(const char *pin, char *err, int err_cap)
{
    return lz_store_security_clear_pin(pin, err, err_cap);
}

int lz_svc_security_selftest(char *buf, int n)
{
    return lz_store_security_selftest(buf, n);
}

const char *lz_fmt_ago(uint32_t ts, char *buf, size_t n)
{
    if(ts == 0) { snprintf(buf, n, "-"); return buf; }
    uint32_t now = now_epoch();
    long d = (long)now - (long)ts;
    if(d < 0) d = 0;
    if(d < 45)            snprintf(buf, n, "now");
    else if(d < 3600)     snprintf(buf, n, "%ldm", d / 60);
    else if(d < 86400)    snprintf(buf, n, "%ldh", d / 3600);
    else                  snprintf(buf, n, "%ldd", d / 86400);
    return buf;
}

static bool g_clock_24;                              /* false = 12-hour (AM/PM) */
void lz_svc_set_clock24(bool on) { g_clock_24 = on; }
bool lz_svc_clock24(void) { return g_clock_24; }

/* format a local seconds-of-day as 24-hour "14:23" or 12-hour "2:23 PM" */
static const char *fmt_hm_local(uint32_t secs, char *buf, size_t n)
{
    unsigned h = (secs / 3600) % 24, m = (secs % 3600) / 60;
    if(g_clock_24) snprintf(buf, n, "%02u:%02u", h, m);
    else {
        unsigned h12 = h % 12; if(h12 == 0) h12 = 12;
        snprintf(buf, n, "%u:%02u %s", h12, m, h < 12 ? "AM" : "PM");
    }
    return buf;
}

const char *lz_fmt_hm(uint32_t ts, char *buf, size_t n)
{
    return fmt_hm_local((ts + eff_tz_min() * 60) % 86400, buf, n);   /* stored UTC -> local */
}

bool lz_node_messageable(const lz_node_rt *n)
{
    if(!n) return false;
    /* MeshCore is locked until Stage 2: nothing on it is messageable yet */
    if(!LZ_MESHCORE_ENABLED && n->net == LZ_NET_MC) return false;
    /* infrastructure is not a person: Meshtastic Router/Repeater, MeshCore
     * Repeater/Sensor/Room are observable but never DM targets */
    return strcmp(n->role, "Client") == 0 ||
           strcmp(n->role, "ClientMute") == 0 ||
           strcmp(n->role, "Tracker") == 0 ||
           strcmp(n->role, "TAK") == 0 ||
           strcmp(n->role, "Hidden") == 0 ||
           strcmp(n->role, "LostFound") == 0 ||
           strcmp(n->role, "TakTracker") == 0 ||
           strcmp(n->role, "Chat") == 0;
}

const char *lz_svc_mt_role_label(int role)
{
    switch(role) {
        case 0:  return "Client";
        case 1:  return "ClientMute";
        case 2:  return "Router";
        case 3:  return "Router";
        case 4:  return "Repeater";
        case 5:  return "Tracker";
        case 6:  return "Sensor";
        case 7:  return "TAK";
        case 8:  return "Hidden";
        case 9:  return "LostFound";
        case 10: return "TakTracker";
        case 11: return "RouterLate";
        default: return "Client";
    }
}

const char *lz_svc_mt_hw_label(int hw)
{
    switch(hw) {
        case 4:   return "T-Beam";
        case 5:   return "Heltec V2";
        case 7:   return "T-Echo";
        case 9:   return "RAK4631";
        case 10:  return "Heltec V2.1";
        case 12:  return "T-Beam S3";
        case 16:  return "T-LoRa T3S3";
        case 43:  return "Heltec V3";
        case 50:  return "T-Deck";
        case 71:  return "T1000-E";
        case 102: return "T-Deck Pro";
        case 128: return "T1000-E Pro";
        case 255: return "Private HW";
        default:  return "Unknown";
    }
}

/* ---------- node table ---------- */

static lz_node_rt *find_node(uint32_t num)
{
    for(int i = 0; i < g_node_count; i++)
        if(g_nodes[i].num == num) return &g_nodes[i];
    return NULL;
}

lz_node_rt *lz_svc_node_by_num(uint32_t num) { return find_node(num); }

lz_node_rt *lz_svc_node_by_name(const char *name)
{
    for(int i = 0; i < g_node_count; i++)
        if(strcmp(g_nodes[i].name, name) == 0) return &g_nodes[i];
    return NULL;
}

/* compare shortcodes ignoring trailing spaces (Meshtastic pads short names) */
static bool sc_eq(const char *a, const char *b)
{
    int la = 0, lb = 0;
    while(a[la]) la++; while(la && a[la - 1] == ' ') la--;
    while(b[lb]) lb++; while(lb && b[lb - 1] == ' ') lb--;
    return la == lb && strncmp(a, b, la) == 0;
}
lz_node_rt *lz_svc_node_by_shortcode(const char *sc)
{
    if(!sc || !sc[0]) return NULL;
    for(int i = 0; i < g_node_count; i++)
        if(g_nodes[i].num != g_id.num && sc_eq(g_nodes[i].shortcode, sc))
            return &g_nodes[i];
    return NULL;
}

static lz_node_rt *ensure_node(uint32_t num, const char *id, lz_net_t net)
{
    lz_node_rt *n = find_node(num);
    if(n) return n;
    if(g_node_count < LZ_MAX_NODES) {
        n = &g_nodes[g_node_count++];
    } else {
        /* table full: evict the stalest non-contact node so new nodes keep
         * appearing (a busy mesh has more nodes than slots) */
        int oldest = -1; uint32_t best = 0xFFFFFFFFu;
        for(int i = 0; i < g_node_count; i++) {
            if(g_nodes[i].contact || g_nodes[i].num == g_id.num) continue;
            if(g_nodes[i].last_heard <= best) { best = g_nodes[i].last_heard; oldest = i; }
        }
        if(oldest < 0) return NULL;        /* every slot is a saved contact */
        n = &g_nodes[oldest];
    }
    memset(n, 0, sizeof *n);
    n->num = num;
    n->net = net;
    n->snr = NAN;
    n->batt = -1;
    snprintf(n->id, sizeof n->id, "%s", id && id[0] ? id : "");
    if(!n->id[0]) snprintf(n->id, sizeof n->id, "!%08x", (unsigned)num);
    snprintf(n->name, sizeof n->name, "%.4s", n->id + 1);
    snprintf(n->shortcode, sizeof n->shortcode, "%.3s", n->id + 1);
    snprintf(n->role, sizeof n->role, "Client");
    snprintf(n->dist, sizeof n->dist, "-");
    return n;
}

int lz_svc_nodes(const lz_node_rt **out) { *out = g_nodes; return g_node_count; }

int lz_svc_node_count(lz_net_t net)
{
    int c = 0;
    for(int i = 0; i < g_node_count; i++)
        if(g_nodes[i].net == net && g_nodes[i].num != g_id.num) c++;
    return c;
}

int lz_svc_node_trace(const lz_node_rt *n, char *buf, int nbuf)
{
    if(!buf || nbuf <= 0) return 0;
    buf[0] = 0;
    if(!n) return snprintf(buf, (size_t)nbuf, "trace: no node selected");

    char ago[12];
    lz_fmt_ago(n->last_heard, ago, sizeof ago);

    char route[28];
    lz_thread_rt *t = find_thread(n->num);
    if(t && t->path[0]) {
        snprintf(route, sizeof route, "path %s", t->path);
    } else if(n->dist[0] && strcmp(n->dist, "-") != 0) {
        snprintf(route, sizeof route, "distance %s", n->dist);
    } else {
        snprintf(route, sizeof route, "no route path");
    }

    const char *net = n->net == LZ_NET_MC ? "MC" : "MT";
    const char *role = n->role[0] ? n->role : "node";
    int pos = 0;
    pos = buf_appendf(buf, nbuf, pos, "trace: %s %s", net, role);
    if(n->net == LZ_NET_MC && !LZ_MESHCORE_ENABLED)
        pos = buf_appendf(buf, nbuf, pos, " gated");
    if(!lz_node_messageable(n))
        pos = buf_appendf(buf, nbuf, pos, " observe-only");
    pos = buf_appendf(buf, nbuf, pos, ", %s, ", route);
    if(isnan(n->snr)) pos = buf_appendf(buf, nbuf, pos, "SNR --");
    else              pos = buf_appendf(buf, nbuf, pos, "SNR %+.1f", (double)n->snr);
    pos = buf_appendf(buf, nbuf, pos, ", heard %s", ago);
    return pos;
}

void lz_svc_add_contact(lz_node_rt *n)
{
    if(!n || n->contact) return;
    n->contact = true;
    lz_store_save_nodes(g_nodes, g_node_count);
    mark_dirty();
}

void lz_svc_set_node_num(uint32_t num)
{
    g_id.num = num;
    snprintf(g_id.id, sizeof g_id.id, "!%08x", (unsigned)num);
}

/* ---- time ---- */
static bool g_time_synced;
void lz_svc_set_time(uint32_t epoch)              /* UTC (e.g. from NTP) */
{
    g_epoch_base = epoch - lz_tick_ms() / 1000;
    g_time_synced = true;
}
bool lz_svc_time_synced(void) { return g_time_synced; }
uint32_t lz_svc_epoch(void) { return now_epoch(); }   /* current UTC, for advert timestamps */

void lz_svc_set_tz(int offset_min)               /* fixed offset, no DST */
{
    g_tz_std_min = offset_min; g_dst_rule = LZ_DST_NONE;
}
void lz_svc_set_tz_zone(int std_min, int dst_rule, const char *std_ab, const char *dst_ab)
{
    g_tz_std_min = std_min;
    g_dst_rule = dst_rule;
    snprintf(g_tz_std_ab, sizeof g_tz_std_ab, "%s", std_ab ? std_ab : "UTC");
    snprintf(g_tz_dst_ab, sizeof g_tz_dst_ab, "%s", dst_ab ? dst_ab : g_tz_std_ab);
}
int  lz_svc_tz(void) { return eff_tz_min(); }
bool lz_svc_dst_active(void) { return dst_at(now_epoch()); }
const char *lz_svc_tz_abbrev(void) { return dst_at(now_epoch()) ? g_tz_dst_ab : g_tz_std_ab; }

/* manual set: the user enters LOCAL wall-clock; store as UTC */
void lz_svc_set_clock(int y, int mo, int d, int h, int mi)
{
    uint32_t local = (uint32_t)(days_from_civil(y, mo, d) * 86400L + h * 3600 + mi * 60);
    uint32_t utc = local - g_tz_std_min * 60;
    if(dst_at(utc)) utc -= 3600;                 /* entered wall time was daylight */
    lz_svc_set_time(utc);
}
void lz_svc_get_clock(int *y, int *mo, int *d, int *h, int *mi)
{
    time_t t = (time_t)now_local();
    struct tm tmv; lz_gmtime(&t, &tmv);
    *y = tmv.tm_year + 1900; *mo = tmv.tm_mon + 1; *d = tmv.tm_mday;
    *h = tmv.tm_hour; *mi = tmv.tm_min;
}
const char *lz_fmt_now(char *buf, size_t n)
{
    if(!g_time_synced) { snprintf(buf, n, "--:--"); return buf; }
    return fmt_hm_local(now_local() % 86400, buf, n);
}
const char *lz_fmt_date(char *buf, size_t n)
{
    if(!g_time_synced) { snprintf(buf, n, "Set time in Settings"); return buf; }
    time_t t = (time_t)now_local();
    struct tm tmv;
    lz_gmtime(&t, &tmv);
    strftime(buf, n, "%A, %b %e", &tmv);
    return buf;
}

/* ---- live system info ---- */
static void (*g_sysinfo_cb)(lz_sysinfo_t *);
void lz_set_sysinfo_cb(void (*cb)(lz_sysinfo_t *)) { g_sysinfo_cb = cb; }
void lz_svc_sysinfo(lz_sysinfo_t *out)
{
    memset(out, 0, sizeof *out);
    if(g_sysinfo_cb) { g_sysinfo_cb(out); return; }
    /* sim / no platform provider: demo values */
    out->battery_pct = 87; out->battery_v = 3.94f; out->charging = false; out->usb = true;
    out->cpu_mhz = 240; out->ram_used_kb = 84; out->ram_total_kb = 512;
    out->flash_used_kb = 6 * 1024 + 200; out->flash_total_kb = 16 * 1024;
    out->temp_c = 24; out->uptime_s = lz_tick_ms() / 1000;
}

void lz_svc_set_identity(const char *long_name, const char *short_name)
{
    if(long_name && long_name[0])
        snprintf(g_id.long_name, sizeof g_id.long_name, "%s", long_name);
    if(short_name && short_name[0])
        snprintf(g_id.short_name, sizeof g_id.short_name, "%s", short_name);
    g_have_identity = true;
    lz_store_save_identity(g_id.long_name, g_id.short_name);

    /* reflect the name onto our own node-table entry */
    lz_node_rt *me = find_node(g_id.num);
    if(me) {
        snprintf(me->name, sizeof me->name, "%s", g_id.long_name);
        snprintf(me->shortcode, sizeof me->shortcode, "%s", g_id.short_name);
        lz_store_save_nodes(g_nodes, g_node_count);
    }
    mark_dirty();
}

/* ---------- threads ---------- */

/* rebuild the newest-first display index over the stable thread array */
static void reorder_threads(void)
{
    for(int i = 0; i < g_thread_count; i++) g_order[i] = i;
    for(int i = 1; i < g_thread_count; i++) {
        int key = g_order[i], j = i - 1;
        while(j >= 0 && g_threads[g_order[j]].last_ts < g_threads[key].last_ts) {
            g_order[j + 1] = g_order[j];
            j--;
        }
        g_order[j + 1] = key;
    }
}

int lz_svc_thread_count_all(void) { return g_thread_count; }

/* most-recent thread with unread messages — drives the lock-screen notification */
lz_thread_rt *lz_svc_top_unread(void)
{
    lz_thread_rt *best = NULL;
    for(int i = 0; i < g_thread_count; i++)
        if(g_threads[i].unread > 0 && !g_threads[i].muted &&
           (!best || g_threads[i].last_ts > best->last_ts))
            best = &g_threads[i];
    return best;
}

/* how many conversations currently have unread messages (muted excluded) —
 * the lock screen shows the newest in full and "+N more" for the rest */
int lz_svc_unread_count(void)
{
    int n = 0;
    for(int i = 0; i < g_thread_count; i++)
        if(g_threads[i].unread > 0 && !g_threads[i].muted) n++;
    return n;
}

/* total unread messages across all conversations (muted excluded) — drives the
 * iPhone-style counter badge on the Messages home icon */
int lz_svc_unread_total(void)
{
    int n = 0;
    for(int i = 0; i < g_thread_count; i++)
        if(!g_threads[i].muted) n += g_threads[i].unread;
    return n;
}

/* silence/unsilence a conversation (RAM-only): a muted chat raises no
 * notification and is excluded from the unread badge */
void lz_svc_toggle_mute(lz_thread_rt *t)
{
    if(t) t->muted = !t->muted;
}

lz_thread_rt *lz_svc_thread_at(int display_idx)
{
    if(display_idx < 0 || display_idx >= g_thread_count) return NULL;
    return &g_threads[g_order[display_idx]];
}

static lz_thread_rt *find_thread(uint32_t num)
{
    for(int i = 0; i < g_thread_count; i++)
        if(g_threads[i].node_num == num) return &g_threads[i];
    return NULL;
}

static lz_thread_rt *find_thread_by_addr(const char *addr)
{
    for(int i = 0; i < g_thread_count; i++)
        if(strcmp(g_threads[i].addr, addr) == 0) return &g_threads[i];
    return NULL;
}

static void touch_thread_meta(lz_thread_rt *t, const char *text, uint32_t ts, bool inc_unread)
{
    snprintf(t->last_text, sizeof t->last_text, "%s", text);
    t->last_ts = ts;
    if(inc_unread) t->unread++;
}

static lz_thread_rt *ensure_thread(lz_node_rt *n)
{
    lz_thread_rt *t = find_thread(n->num);
    if(t) return t;
    if(g_thread_count >= LZ_MAX_THREADS) return NULL;   /* full: do not clobber slot 0 */
    t = &g_threads[g_thread_count++];
    memset(t, 0, sizeof *t);
    t->node_num = n->num;
    t->net = n->net;
    snprintf(t->addr, sizeof t->addr, "%s", n->id);
    snprintf(t->name, sizeof t->name, "%s", n->name);
    t->messageable = lz_node_messageable(n);
    t->is_channel = false;
    snprintf(t->path, sizeof t->path, "direct");
    return t;
}

lz_thread_rt *lz_svc_thread_for_node(lz_node_rt *n)
{
    lz_thread_rt *t = ensure_thread(n);
    if(t) lz_store_save_threads(g_threads, g_thread_count);
    return t;
}

/* The LongFast primary broadcast channel — where you reach everyone nearby.
 * It's a real channel (created on every device, not demo data): sending here
 * broadcasts (to = LZ_BROADCAST) and inbound broadcasts land here. */
lz_thread_rt *lz_svc_channel_thread(void)
{
    /* keyed by addr (not node_num): both LongFast and MeshCore Public use
     * node_num==LZ_BROADCAST, so find_thread(LZ_BROADCAST) would conflate them. */
    lz_thread_rt *t = find_thread_by_addr("longfast");
    if(t) {                              /* re-assert channel props (survives reload) */
        t->is_channel = true;
        t->messageable = true;
        t->net = LZ_NET_MT;
        if(!t->name[0]) snprintf(t->name, sizeof t->name, "LongFast");
        return t;
    }
    if(g_thread_count >= LZ_MAX_THREADS) return NULL;
    t = &g_threads[g_thread_count++];
    memset(t, 0, sizeof *t);
    t->node_num = LZ_BROADCAST;
    t->net = LZ_NET_MT;
    snprintf(t->addr, sizeof t->addr, "longfast");
    snprintf(t->name, sizeof t->name, "LongFast");
    snprintf(t->path, sizeof t->path, "Primary");
    t->messageable = true;
    t->is_channel = true;
    return t;
}

/* The MeshCore "Public" group channel — the amber-side counterpart to LongFast.
 * Distinct addr so it coexists with the Meshtastic channel in the inbox. */
lz_thread_rt *lz_svc_mc_channel_thread(void)
{
    lz_thread_rt *t = find_thread_by_addr("mcpublic");
    if(t) {
        t->is_channel = true;
        t->messageable = true;
        t->net = LZ_NET_MC;
        if(!t->name[0]) snprintf(t->name, sizeof t->name, "Public");
        return t;
    }
    if(g_thread_count >= LZ_MAX_THREADS) return NULL;
    t = &g_threads[g_thread_count++];
    memset(t, 0, sizeof *t);
    t->node_num = LZ_BROADCAST;
    t->net = LZ_NET_MC;
    snprintf(t->addr, sizeof t->addr, "mcpublic");
    snprintf(t->name, sizeof t->name, "Public");
    snprintf(t->path, sizeof t->path, "MeshCore");
    t->messageable = true;
    t->is_channel = true;
    return t;
}

#define LZ_MC_ADDR_HEX_CHARS 64

static int mc_hex_val(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void mc_key_hex(const uint8_t key[32], char *out, int cap)
{
    static const char hex[] = "0123456789abcdef";
    if(!out || cap <= 0) return;
    if(!key || cap < LZ_MC_ADDR_HEX_CHARS + 1) {
        out[0] = 0;
        return;
    }
    for(int i = 0; i < 32; i++) {
        out[i * 2] = hex[(key[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[key[i] & 0x0F];
    }
    out[LZ_MC_ADDR_HEX_CHARS] = 0;
}

static bool mc_key_from_hex(const char *s, uint8_t out[32])
{
    if(!s || !out || strlen(s) != LZ_MC_ADDR_HEX_CHARS) return false;
    for(int i = 0; i < 32; i++) {
        int hi = mc_hex_val(s[i * 2]);
        int lo = mc_hex_val(s[i * 2 + 1]);
        if(hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool mc_node_addr(const lz_node_rt *nd, char *out, int cap)
{
    if(!nd || nd->net != LZ_NET_MC || !nd->has_key) {
        if(out && cap > 0) out[0] = 0;
        return false;
    }
    mc_key_hex(nd->pubkey, out, cap);
    return out && out[0] != 0;
}

static void mc_node_short_id(const lz_node_rt *nd, char *out, int cap)
{
    if(!out || cap <= 0) return;
    if(nd && nd->net == LZ_NET_MC && nd->has_key) {
        snprintf(out, (size_t)cap, "MC-%02x%02x%02x%02x",
                 nd->pubkey[0], nd->pubkey[1], nd->pubkey[2], nd->pubkey[3]);
    } else if(nd && nd->id[0]) {
        snprintf(out, (size_t)cap, "%s", nd->id);
    } else {
        snprintf(out, (size_t)cap, "MC-unknown");
    }
}

static bool mc_local_addr(char *out, int cap)
{
    uint8_t pub[32];
    if(lz_backend_mc_pubkey && lz_backend_mc_pubkey(pub)) {
        mc_key_hex(pub, out, cap);
        return out && out[0] != 0;
    }
    if(out && cap > 0) out[0] = 0;
    return false;
}

static lz_node_rt *mc_node_by_pubkey(const uint8_t key[32])
{
    if(!key) return NULL;
    for(int i = 0; i < g_node_count; i++)
        if(g_nodes[i].net == LZ_NET_MC && g_nodes[i].has_key &&
           memcmp(g_nodes[i].pubkey, key, 32) == 0)
            return &g_nodes[i];
    return NULL;
}

static lz_node_rt *mc_node_by_addr(const char *addr, bool *bad_addr)
{
    uint8_t key[32];
    if(bad_addr) *bad_addr = false;
    if(!addr || !addr[0]) return NULL;
    if(!mc_key_from_hex(addr, key)) {
        if(bad_addr) *bad_addr = true;
        return NULL;
    }
    return mc_node_by_pubkey(key);
}

static char mc_fold_char(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool mc_name_eq(const char *a, const char *b)
{
    if(!a || !b) return false;
    while(*a && *b) {
        if(mc_fold_char(*a++) != mc_fold_char(*b++)) return false;
    }
    return *a == 0 && *b == 0;
}

static lz_node_rt *mc_node_by_name_unique(const char *name, bool *ambiguous)
{
    lz_node_rt *match = NULL;
    if(ambiguous) *ambiguous = false;
    if(!name || !name[0]) return NULL;
    for(int i = 0; i < g_node_count; i++) {
        if(g_nodes[i].net != LZ_NET_MC || !mc_name_eq(g_nodes[i].name, name)) continue;
        if(match) {
            if(ambiguous) *ambiguous = true;
            return match;
        }
        match = &g_nodes[i];
    }
    return match;
}

static const char *mc_companion_dm_state(const lz_node_rt *n)
{
    if(!n || n->net != LZ_NET_MC) return "not_messageable";
    if(strcmp(n->role, "Chat") != 0) return "not_messageable";
    if(!n->has_key) return "no_key";
    return "ready";
}

static bool mc_companion_dm_target(const lz_node_rt *n)
{
    return strcmp(mc_companion_dm_state(n), "ready") == 0;
}

static bool mc_companion_send_dm_node(lz_node_rt *target, const char *text)
{
    if(!target || !text || !text[0] || !mc_companion_dm_target(target))
        return false;
    return lz_backend_mc_dm_key &&
           lz_backend_mc_dm_key(target->pubkey, target->name, text);
}

static void track_loaded_delivery(lz_thread_rt *t);

void lz_svc_open_thread(lz_thread_rt *t)
{
    if(!t) return;
    g_open = t;
    t->unread = 0;
    g_tail_count = lz_store_load_tail(t->addr, g_tail, LZ_TAIL_MAX);
    track_loaded_delivery(t);
    lz_store_save_threads(g_threads, g_thread_count);
    /* opening a Meshtastic DM without the peer's key -> request its NodeInfo so
     * we can PKI-encrypt (the reply carries the key, usually within a second) */
    if(!t->is_channel && t->node_num != LZ_BROADCAST && t->net == LZ_NET_MT) {
        uint8_t k[32];
        if(!lz_svc_node_pubkey(t->node_num, k)) lz_backend_request_nodeinfo(t->node_num);
    }
}

int lz_svc_tail(const lz_msg_rt **out) { *out = g_tail; return g_tail_count; }

static void tail_push(bool self, const char *text, uint32_t ts, uint8_t status,
                      uint32_t pkt_id, uint8_t retries, uint8_t fail_reason)
{
    lz_msg_rt *m;
    if(g_tail_count < LZ_TAIL_MAX) m = &g_tail[g_tail_count++];
    else { memmove(&g_tail[0], &g_tail[1], sizeof(lz_msg_rt) * (LZ_TAIL_MAX - 1));
           m = &g_tail[LZ_TAIL_MAX - 1]; }
    memset(m, 0, sizeof *m);
    m->self = self; m->ts = ts;
    m->status = status; m->pkt_id = pkt_id; m->retries = retries;
    m->fail_reason = status == LZ_MSG_FAILED ? fail_reason : LZ_FAIL_NONE;
    m->sent_ms = status == LZ_MSG_SENDING ? lz_tick_ms() : 0;
    snprintf(m->text, sizeof m->text, "%s", text);
}

static bool tail_mark_delivery(uint32_t old_pkt_id, uint32_t new_pkt_id,
                               uint8_t status, uint8_t retries,
                               uint8_t fail_reason)
{
    for(int i = 0; i < g_tail_count; i++) {
        if(g_tail[i].self && g_tail[i].pkt_id == old_pkt_id) {
            uint8_t next_retries = retries;
            if(status != LZ_MSG_SENDING && next_retries == 0 && g_tail[i].retries)
                next_retries = g_tail[i].retries;
            g_tail[i].status = status;
            g_tail[i].pkt_id = new_pkt_id;
            g_tail[i].retries = next_retries;
            g_tail[i].fail_reason = status == LZ_MSG_FAILED ? fail_reason : LZ_FAIL_NONE;
            g_tail[i].sent_ms = status == LZ_MSG_SENDING ? lz_tick_ms() : 0;
            return true;   /* one in-flight message per pkt_id; stop at the first */
        }
    }
    return false;
}

static uint8_t tail_delivery_retries(uint32_t pkt_id)
{
    for(int i = 0; i < g_tail_count; i++)
        if(g_tail[i].self && g_tail[i].pkt_id == pkt_id) return g_tail[i].retries;
    return 0;
}

static bool tail_find_delivery(uint32_t pkt_id, lz_msg_rt *out)
{
    if(!out) return false;
    for(int i = 0; i < g_tail_count; i++) {
        if(g_tail[i].self && g_tail[i].pkt_id == pkt_id) {
            *out = g_tail[i];
            return true;
        }
    }
    return false;
}

static bool load_queued_delivery(const char *addr, uint32_t pkt_id, lz_msg_rt *out)
{
    if(g_open && strcmp(g_open->addr, addr) == 0 &&
       tail_find_delivery(pkt_id, out))
        return true;
    return lz_store_find_delivery(addr, pkt_id, out);
}

static void track_loaded_delivery(lz_thread_rt *t)
{
    if(!t) return;
    for(int i = 0; i < g_tail_count; i++) {
        if(!g_tail[i].self || !g_tail[i].pkt_id) continue;
        /* keep newly-minted ids ahead of any reloaded one so a fresh DM cannot
         * reuse a still-tracked pkt_id (which would mis-route its ACK/timeout) */
        if(g_tail[i].pkt_id > g_pkt_seq) g_pkt_seq = g_tail[i].pkt_id;
        if(g_tail[i].status == LZ_MSG_SENDING) {
            /* lz_store_load_tail() zeroes sent_ms; give reopened in-flight DMs a
             * fresh ACK window instead of instantly aging them to FAILED */
            g_tail[i].sent_ms = lz_tick_ms();
            delivery_track(t->addr, g_tail[i].pkt_id, g_tail[i].retries);
        }
    }
}

static void track_stored_delivery(void)
{
    static lz_msg_rt ring[LZ_TAIL_MAX];
    for(int t = 0; t < g_thread_count; t++) {
        if(g_threads[t].is_channel) continue;
        int n = lz_store_load_tail(g_threads[t].addr, ring, LZ_TAIL_MAX);
        for(int i = 0; i < n; i++) {
            if(!ring[i].self || !ring[i].pkt_id) continue;
            if(ring[i].pkt_id > g_pkt_seq) g_pkt_seq = ring[i].pkt_id;
            if(ring[i].status == LZ_MSG_SENDING)
                delivery_track(g_threads[t].addr, ring[i].pkt_id,
                               ring[i].retries);
        }
    }
}

static bool send_text_packet(lz_thread_rt *t, const char *text, uint32_t pid)
{
    if(!t || !text) return false;
    lz_mt_packet_t p;
    memset(&p, 0, sizeof p);
    p.to = t->node_num;                  /* LZ_BROADCAST for the channel */
    p.from = g_id.num;
    p.id = pid;
    p.hop_limit = 3;
    p.hop_start = 3;
    p.want_ack = !t->is_channel;          /* broadcasts are not ACKed */
    p.portnum = 1;             /* TEXT_MESSAGE_APP */
    size_t len = strlen(text);
    if(len > sizeof p.payload) len = sizeof p.payload;
    memcpy(p.payload, text, len);
    p.plen = (uint8_t)len;
    return lz_backend_send(&p);
}

bool lz_svc_send_text(lz_thread_rt *t, const char *text)
{
    if(!t || !text[0] || !t->messageable) return false;
    if(t->net == LZ_NET_MC) {       /* MeshCore: backend sends + echoes into the thread */
        if(t->is_channel) return lz_backend_mc_send_public && lz_backend_mc_send_public(text);
        lz_node_rt *n = find_node(t->node_num);
        return mc_companion_send_dm_node(n, text);
    }
    uint32_t ts = now_epoch();
    uint32_t pid = next_packet_id();
    bool track = !t->is_channel;          /* DMs get delivery status; channels don't */

    if(!track) {
        bool sent = send_text_packet(t, text, pid);
        uint8_t status = sent ? LZ_MSG_NONE : LZ_MSG_FAILED;
        uint8_t fail = sent ? LZ_FAIL_NONE : LZ_FAIL_RADIO_SEND;
        lz_msg_rt m = { .self = true, .ts = ts, .status = status,
                        .pkt_id = sent ? 0 : pid, .sent_ms = 0,
                        .fail_reason = fail };
        snprintf(m.text, sizeof m.text, "%s", text);
        lz_store_append(t->addr, &m);
        if(g_open == t) tail_push(true, text, ts, status, sent ? 0 : pid,
                                  0, fail);
        touch_thread_meta(t, text, ts, false);
        reorder_threads();
        lz_store_save_threads(g_threads, g_thread_count);
        mark_dirty();
        return true;
    }

    uint8_t status = track ? LZ_MSG_SENDING : LZ_MSG_NONE;
    lz_msg_rt m = { .self = true, .ts = ts, .status = status,
                    .pkt_id = track ? pid : 0, .sent_ms = lz_tick_ms() };
    snprintf(m.text, sizeof m.text, "%s", text);
    lz_store_append(t->addr, &m);
    if(g_open == t) tail_push(true, text, ts, status, track ? pid : 0,
                              0, LZ_FAIL_NONE);
    touch_thread_meta(t, text, ts, false);

    if(track) delivery_track(t->addr, pid, 0);
    bool sent = send_text_packet(t, text, pid);
    if(track) {
        if(!sent) {
            delivery_forget(pid);
            lz_store_update_delivery(t->addr, pid, pid, LZ_MSG_FAILED,
                                     0, LZ_FAIL_RADIO_SEND);
            if(g_open == t) tail_mark_delivery(pid, pid, LZ_MSG_FAILED,
                                               0, LZ_FAIL_RADIO_SEND);
        }
    }

    reorder_threads();
    lz_store_save_threads(g_threads, g_thread_count);
    mark_dirty();
    return true;
}

/* resend a failed sent DM (long-press): reset to SENDING with a new id + retx */
bool lz_svc_resend(int tail_idx)
{
    if(!g_open || tail_idx < 0 || tail_idx >= g_tail_count) return false;
    lz_msg_rt *m = &g_tail[tail_idx];
    if(!m->self || m->status != LZ_MSG_FAILED) return false;
    uint32_t old_pid = m->pkt_id;
    if(m->retries >= LZ_MSG_RETRY_MAX) {
        m->fail_reason = LZ_FAIL_RETRY_LIMIT;
        if(old_pid)
            lz_store_update_delivery(g_open->addr, old_pid, old_pid,
                                     LZ_MSG_FAILED, m->retries,
                                     LZ_FAIL_RETRY_LIMIT);
        mark_dirty();
        return false;
    }
    uint32_t pid = next_packet_id();
    uint8_t retries = (uint8_t)(m->retries + 1);
    m->pkt_id = pid; m->sent_ms = lz_tick_ms(); m->status = LZ_MSG_SENDING;
    m->retries = retries; m->fail_reason = LZ_FAIL_NONE;

    if(old_pid) {
        lz_store_update_delivery(g_open->addr, old_pid, pid,
                                 LZ_MSG_SENDING, retries, LZ_FAIL_NONE);
        delivery_forget(old_pid);
    }
    delivery_track(g_open->addr, pid, retries);
    bool sent = send_text_packet(g_open, m->text, pid);
    if(!sent) {
        delivery_forget(pid);
        m->status = LZ_MSG_FAILED;
        m->sent_ms = 0;
        m->fail_reason = LZ_FAIL_RADIO_SEND;
        if(old_pid)
            lz_store_update_delivery(g_open->addr, pid, pid,
                                     LZ_MSG_FAILED, retries,
                                     LZ_FAIL_RADIO_SEND);
    }
    mark_dirty();
    return true;
}

int lz_svc_delivery_diag(char *buf, int n)
{
    if(!buf || n <= 0) return 0;
    buf[0] = 0;
    int pos = 0;
    uint32_t now = lz_tick_ms();
    int pending = 0;

    for(int i = 0; i < LZ_DELIVERY_PEND; i++)
        if(g_delivery[i].used) pending++;

    if(pending) {
        pos = buf_appendf(buf, n, pos, "delivery: %d pending sent DM%s\n",
                          pending, pending == 1 ? "" : "s");
        for(int i = 0; i < LZ_DELIVERY_PEND; i++) {
            if(!g_delivery[i].used) continue;
            lz_thread_rt *t = find_thread_by_addr(g_delivery[i].addr);
            uint32_t age_s = (now - g_delivery[i].sent_ms) / 1000;
            pos = buf_appendf(buf, n, pos,
                              "  %08lx  %s  %s  age=%lus retry=%u/%u\n",
                              (unsigned long)g_delivery[i].pkt_id,
                              t ? t->name : g_delivery[i].addr,
                              delivery_status_label(LZ_MSG_SENDING),
                              (unsigned long)age_s,
                              (unsigned)g_delivery[i].retries,
                              (unsigned)LZ_MSG_RETRY_MAX);
        }
    } else {
        pos = buf_appendf(buf, n, pos, "delivery: no pending sent DMs\n");
    }

    if(g_open) {
        int failed = 0;
        for(int i = 0; i < g_tail_count; i++) {
            if(!g_tail[i].self || g_tail[i].status != LZ_MSG_FAILED) continue;
            if(!failed++)
                pos = buf_appendf(buf, n, pos, "open thread failures:\n");
            const char *reason = g_tail[i].retries >= LZ_MSG_RETRY_MAX
                               ? lz_svc_delivery_fail_label(LZ_FAIL_RETRY_LIMIT)
                               : lz_svc_delivery_fail_label(g_tail[i].fail_reason);
            pos = buf_appendf(buf, n, pos,
                              "  %08lx  %s  retry=%u/%u\n",
                              (unsigned long)g_tail[i].pkt_id, reason,
                              (unsigned)g_tail[i].retries,
                              (unsigned)LZ_MSG_RETRY_MAX);
        }
    }
    return pos;
}

int lz_svc_mc_companion_hello(char *buf, int n)
{
    if(!buf || n <= 0) return 0;
    char addr[LZ_MC_ADDR_HEX_CHARS + 1], short_id[24];
    if(!mc_local_addr(addr, sizeof addr)) lz_backend_mc_addr(addr, sizeof addr);
    lz_backend_mc_addr(short_id, sizeof short_id);
    const char *build =
#if LZ_MESHCORE_ENABLED
        "enabled";
#else
        "gated";
#endif
    return snprintf(buf, (size_t)n,
                    "mccomp: hello v0 build=%s meshcore=%s addr=%s short_id=%s protocol=line\n",
                    build, build, addr, short_id);
}

int lz_svc_mc_companion_status(char *buf, int n)
{
    if(!buf || n <= 0) return 0;
    int mc_nodes = 0, mc_threads = 0, unread = 0;
    for(int i = 0; i < g_node_count; i++)
        if(g_nodes[i].net == LZ_NET_MC) mc_nodes++;
    for(int i = 0; i < g_thread_count; i++) {
        if(g_threads[i].net != LZ_NET_MC) continue;
        mc_threads++;
        unread += g_threads[i].unread;
    }
    char addr[LZ_MC_ADDR_HEX_CHARS + 1], short_id[24];
    if(!mc_local_addr(addr, sizeof addr)) lz_backend_mc_addr(addr, sizeof addr);
    lz_backend_mc_addr(short_id, sizeof short_id);
    return snprintf(buf, (size_t)n,
                    "mccomp: status v0 addr=%s short_id=%s nodes=%d threads=%d unread=%d public=%s dm=%s\n",
                    addr, short_id, mc_nodes, mc_threads, unread,
                    lz_backend_mc_send_public ? "sendable" : "unavailable",
                    lz_backend_mc_dm_key ? "sendable" : "unavailable");
}

int lz_svc_mc_companion_nodes(char *buf, int n)
{
    int pos = 0, listed = 0;
    if(!buf || n <= 0) return 0;
    for(int i = 0; i < g_node_count; i++) {
        const lz_node_rt *nd = &g_nodes[i];
        if(nd->net != LZ_NET_MC) continue;
        char ago[12];
        lz_fmt_ago(nd->last_heard, ago, sizeof ago);
        char addr[LZ_MC_ADDR_HEX_CHARS + 1], short_id[24];
        bool has_addr = mc_node_addr(nd, addr, sizeof addr);
        mc_node_short_id(nd, short_id, sizeof short_id);
        pos = buf_appendf(buf, n, pos,
                          "mccomp-node: name=\"%s\" addr=%s short_id=%s role=%s snr=%.1f last=%s public_key=%s dm=%s\n",
                          nd->name, has_addr ? addr : "-", short_id, nd->role,
                          (double)nd->snr, ago,
                          nd->has_key ? "present" : "missing",
                          mc_companion_dm_target(nd) ? "yes" : "no");
        listed++;
    }
    if(!listed)
        pos = buf_appendf(buf, n, pos, "mccomp-node: none\n");
    return pos;
}

int lz_svc_mc_companion_threads(char *buf, int n)
{
    int pos = 0, listed = 0;
    if(!buf || n <= 0) return 0;
    for(int oi = 0; oi < g_thread_count; oi++) {
        int idx = (oi < LZ_MAX_THREADS) ? g_order[oi] : -1;
        if(idx < 0 || idx >= g_thread_count) continue;
        const lz_thread_rt *t = &g_threads[idx];
        if(t->net != LZ_NET_MC) continue;
        char ago[12];
        lz_fmt_ago(t->last_ts, ago, sizeof ago);
        pos = buf_appendf(buf, n, pos,
                          "mccomp-thread: name=\"%s\" addr=%s kind=%s unread=%d last=%s text=\"%s\"\n",
                          t->name, t->addr, t->is_channel ? "public" : "dm",
                          t->unread, ago, t->last_text);
        listed++;
    }
    if(!listed)
        pos = buf_appendf(buf, n, pos, "mccomp-thread: none\n");
    return pos;
}

bool lz_svc_mc_companion_send_public(const char *text)
{
    if(!text || !text[0]) return false;
    lz_thread_rt *t = lz_svc_mc_channel_thread();
    return t && lz_svc_send_text(t, text);
}

bool lz_svc_mc_companion_send_dm(const char *name, const char *text)
{
    if(!name || !name[0] || !text || !text[0]) return false;
    bool ambiguous = false;
    lz_node_rt *n = mc_node_by_name_unique(name, &ambiguous);
    if(ambiguous) return false;
    return mc_companion_send_dm_node(n, text);
}

static const char *mc0_skip_ws(const char *p)
{
    while(p && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static bool mc0_next_token(const char **p, char *out, int cap)
{
    const char *s = mc0_skip_ws(*p);
    int i = 0;
    if(!s || !*s || cap <= 0) return false;
    while(*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
        if(i + 1 < cap) out[i++] = *s;
        s++;
    }
    out[i] = 0;
    *p = s;
    return i > 0;
}

static bool mc0_hexval(char c, int *v)
{
    if(c >= '0' && c <= '9') { *v = c - '0'; return true; }
    if(c >= 'a' && c <= 'f') { *v = c - 'a' + 10; return true; }
    if(c >= 'A' && c <= 'F') { *v = c - 'A' + 10; return true; }
    return false;
}

static void mc0_decode_value(const char *src, char *dst, int cap)
{
    int out = 0;
    if(cap <= 0) return;
    while(src && *src && out + 1 < cap) {
        if(*src == '%' && src[1] && src[2]) {
            int hi, lo;
            if(mc0_hexval(src[1], &hi) && mc0_hexval(src[2], &lo)) {
                dst[out++] = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }
        dst[out++] = *src++;
    }
    dst[out] = 0;
}

static int mc0_append_pct(char *buf, int n, int pos, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    if(!s) s = "";
    while(*s) {
        unsigned char c = (unsigned char)*s++;
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                     c == '.' || c == '~' || c == '!';
        if(plain) {
            if(pos + 1 < n) buf[pos] = (char)c;
            pos++;
        } else {
            if(pos + 3 < n) {
                buf[pos] = '%';
                buf[pos + 1] = hex[(c >> 4) & 0x0F];
                buf[pos + 2] = hex[c & 0x0F];
            }
            pos += 3;
        }
    }
    if(n > 0) buf[pos < n ? pos : n - 1] = 0;
    return pos;
}

static bool mc0_get_arg(const char *p, const char *key, char *out, int cap)
{
    char tok[220];
    size_t klen = strlen(key);
    while(mc0_next_token(&p, tok, sizeof tok)) {
        if(strncmp(tok, key, klen) == 0 && tok[klen] == '=') {
            mc0_decode_value(tok + klen + 1, out, cap);
            return out && cap > 0 && out[0] != 0;
        }
    }
    if(out && cap > 0) out[0] = 0;
    return false;
}

static int mc0_ok_prefix(char *buf, int n, const char *id)
{
    return snprintf(buf, (size_t)n, "MC0 %s OK ", id && id[0] ? id : "0");
}

static int mc0_err(char *buf, int n, const char *id,
                   const char *code, bool retry, const char *msg)
{
    int pos = snprintf(buf, (size_t)n, "MC0 %s ERR code=%s retry=%d message=",
                       id && id[0] ? id : "0", code ? code : "internal", retry ? 1 : 0);
    pos = mc0_append_pct(buf, n, pos, msg ? msg : "");
    pos = buf_appendf(buf, n, pos, "\n");
    return pos;
}

static int mc0_count_nodes(void)
{
    int c = 0;
    for(int i = 0; i < g_node_count; i++)
        if(g_nodes[i].net == LZ_NET_MC) c++;
    return c;
}

static int mc0_count_threads(int *unread_out)
{
    int c = 0, unread = 0;
    for(int i = 0; i < g_thread_count; i++) {
        if(g_threads[i].net != LZ_NET_MC) continue;
        c++;
        unread += g_threads[i].unread;
    }
    if(unread_out) *unread_out = unread;
    return c;
}

static int mc0_hello(char *buf, int n, const char *id)
{
    int pos = mc0_ok_prefix(buf, n, id);
    return buf_appendf(buf, n, pos,
                       "proto=0 fw=0.8-draft device=tdeck caps=identity,nodes,status,threads,send_public,send_dm,events,exit max_line=512 max_text=%d event_seq=0 nodes_rev=0 messages_rev=0\n",
                       LZ_TEXT_MAX);
}

static int mc0_identity(char *buf, int n, const char *id)
{
    char addr[LZ_MC_ADDR_HEX_CHARS + 1], short_id[24];
    bool have_addr = mc_local_addr(addr, sizeof addr);
    if(!have_addr) lz_backend_mc_addr(addr, sizeof addr);
    lz_backend_mc_addr(short_id, sizeof short_id);
    int pos = mc0_ok_prefix(buf, n, id);
    pos = buf_appendf(buf, n, pos, "enabled=%d name=", LZ_MESHCORE_ENABLED ? 1 : 0);
    pos = mc0_append_pct(buf, n, pos, g_id.long_name);
    return buf_appendf(buf, n, pos,
                       " addr=%s short_id=%s role=chat pubkey=%s addr_format=meshcore-pubkey-hex advert_ready=%d\n",
                       addr, short_id, have_addr ? addr : "-",
                       have_addr ? 1 : 0);
}

static int mc0_status(char *buf, int n, const char *id)
{
    char addr[LZ_MC_ADDR_HEX_CHARS + 1], short_id[24];
    int unread = 0;
    int nodes = mc0_count_nodes();
    int threads = mc0_count_threads(&unread);
    if(!mc_local_addr(addr, sizeof addr)) lz_backend_mc_addr(addr, sizeof addr);
    lz_backend_mc_addr(short_id, sizeof short_id);
    int pos = mc0_ok_prefix(buf, n, id);
    return buf_appendf(buf, n, pos,
                       "proto=0 mc=%s bridge=usb mc_companion=attached mt_companion=%s addr=%s short_id=%s nodes=%d threads=%d unread=%d public=%d dm=%d event_seq=0 nodes_rev=0 messages_rev=0\n",
                       LZ_MESHCORE_ENABLED ? "on" : "disabled",
                       lz_mtc_active() ? "on" : "off",
                       addr, short_id, nodes, threads, unread,
                       lz_backend_mc_send_public ? 1 : 0,
                       lz_backend_mc_dm_key ? 1 : 0);
}

static int mc0_limit_arg(const char *args, int def, int max)
{
    char raw[12];
    int limit = def;
    if(mc0_get_arg(args, "limit", raw, sizeof raw)) {
        limit = atoi(raw);
        if(limit <= 0) limit = def;
    }
    if(limit > max) limit = max;
    return limit;
}

static int mc0_nodes(char *buf, int n, const char *id, const char *args)
{
    int count = mc0_count_nodes();
    int limit = mc0_limit_arg(args, 5, 5);
    int listed = count < limit ? count : limit;
    int more = count > listed ? 1 : 0;
    int pos = buf_appendf(buf, n, 0,
                          "MC0 %s BEGIN type=nodes rev=0 count=%d more=%d cursor=%s\n",
                          id, listed, more, more ? "next" : "end");
    int emitted = 0;
    for(int i = 0; i < g_node_count; i++) {
        const lz_node_rt *nd = &g_nodes[i];
        if(nd->net != LZ_NET_MC) continue;
        if(emitted >= listed) break;
        uint32_t seen_ms = 0;
        uint32_t now_s = now_epoch();
        if(nd->last_heard && now_s >= nd->last_heard)
            seen_ms = (now_s - nd->last_heard) * 1000u;
        char addr[LZ_MC_ADDR_HEX_CHARS + 1], short_id[24];
        bool has_addr = mc_node_addr(nd, addr, sizeof addr);
        mc_node_short_id(nd, short_id, sizeof short_id);
        pos = buf_appendf(buf, n, pos, "MC0 %s NODE addr=%s short_id=%s name=",
                          id, has_addr ? addr : "-", short_id);
        pos = mc0_append_pct(buf, n, pos, nd->name);
        pos = buf_appendf(buf, n, pos, " role=");
        pos = mc0_append_pct(buf, n, pos, nd->role);
        pos = buf_appendf(buf, n, pos, " seen_ms=%lu snr=%.1f public_key=%s dm=%s\n",
                          (unsigned long)seen_ms, (double)nd->snr,
                          nd->has_key ? "present" : "missing",
                          mc_companion_dm_state(nd));
        emitted++;
    }
    return buf_appendf(buf, n, pos,
                       "MC0 %s END type=nodes rev=0 count=%d more=%d cursor=%s\n",
                       id, emitted, more, more ? "next" : "end");
}

static int mc0_threads(char *buf, int n, const char *id)
{
    int count = mc0_count_threads(NULL);
    int pos = buf_appendf(buf, n, 0,
                          "MC0 %s BEGIN type=threads rev=0 count=%d more=0 cursor=end\n",
                          id, count);
    for(int oi = 0; oi < g_thread_count; oi++) {
        int idx = (oi < LZ_MAX_THREADS) ? g_order[oi] : -1;
        if(idx < 0 || idx >= g_thread_count) continue;
        const lz_thread_rt *t = &g_threads[idx];
        if(t->net != LZ_NET_MC) continue;
        pos = buf_appendf(buf, n, pos, "MC0 %s THREAD addr=%s name=", id, t->addr);
        pos = mc0_append_pct(buf, n, pos, t->name);
        pos = buf_appendf(buf, n, pos, " kind=%s unread=%d last=%lu text=",
                          t->is_channel ? "public" : "dm", t->unread,
                          (unsigned long)t->last_ts);
        pos = mc0_append_pct(buf, n, pos, t->last_text);
        pos = buf_appendf(buf, n, pos, "\n");
    }
    return buf_appendf(buf, n, pos,
                       "MC0 %s END type=threads rev=0 count=%d more=0 cursor=end\n",
                       id, count);
}

static int mc0_send_public(char *buf, int n, const char *id, const char *args)
{
    char text[LZ_TEXT_MAX + 1];
    if(!mc0_get_arg(args, "text", text, sizeof text))
        return mc0_err(buf, n, id, "bad_request", false, "missing text");
    if((int)strlen(text) > LZ_TEXT_MAX)
        return mc0_err(buf, n, id, "text_too_long", false, "text too long");
    if(!lz_svc_mc_companion_send_public(text))
        return mc0_err(buf, n, id, "send_failed", true, "public send failed");
    int pos = mc0_ok_prefix(buf, n, id);
    return buf_appendf(buf, n, pos, "accepted=1 kind=public status=queued\n");
}

static int mc0_send_dm(char *buf, int n, const char *id, const char *args)
{
    char text[LZ_TEXT_MAX + 1], name[64], addr[LZ_MC_ADDR_HEX_CHARS + 1];
    if(!mc0_get_arg(args, "text", text, sizeof text))
        return mc0_err(buf, n, id, "bad_request", false, "missing text");
    if((int)strlen(text) > LZ_TEXT_MAX)
        return mc0_err(buf, n, id, "text_too_long", false, "text too long");

    bool have_name = mc0_get_arg(args, "to_name", name, sizeof name);
    bool have_addr = mc0_get_arg(args, "to_addr", addr, sizeof addr);
    lz_node_rt *target = NULL;
    bool ambiguous = false;
    if(have_addr) {
        bool bad_addr = false;
        target = mc_node_by_addr(addr, &bad_addr);
        if(bad_addr) return mc0_err(buf, n, id, "bad_addr", false, "to_addr must be 64 hex chars");
        if(!target) return mc0_err(buf, n, id, "not_found", false, "node not found");
    }
    if(have_name) {
        lz_node_rt *by_name = mc_node_by_name_unique(name, &ambiguous);
        if(ambiguous) return mc0_err(buf, n, id, "ambiguous_name", false, "name is ambiguous");
        if(!by_name) return mc0_err(buf, n, id, "not_found", false, "node not found");
        if(target && by_name != target)
            return mc0_err(buf, n, id, "target_mismatch", false, "to_addr and to_name identify different nodes");
        target = by_name;
    }
    if(!have_addr && !have_name) {
        return mc0_err(buf, n, id, "bad_request", false, "missing to_name or to_addr");
    }
    if(!target) return mc0_err(buf, n, id, "not_found", false, "node not found");
    const char *dm_state = mc_companion_dm_state(target);
    if(strcmp(dm_state, "no_key") == 0)
        return mc0_err(buf, n, id, "no_key", false, "node key is missing");
    if(strcmp(dm_state, "ready") != 0)
        return mc0_err(buf, n, id, "not_messageable", false, "node cannot receive DMs");
    if(!mc_companion_send_dm_node(target, text))
        return mc0_err(buf, n, id, "send_failed", true, "DM send failed");

    char target_addr[LZ_MC_ADDR_HEX_CHARS + 1];
    mc_node_addr(target, target_addr, sizeof target_addr);
    int pos = mc0_ok_prefix(buf, n, id);
    pos = buf_appendf(buf, n, pos, "accepted=1 kind=dm to_addr=%s to_name=",
                      target_addr);
    pos = mc0_append_pct(buf, n, pos, target->name);
    return buf_appendf(buf, n, pos, " status=queued\n");
}

int lz_svc_mc_companion_handle_line(const char *line, char *buf, int n, bool *exit_mode)
{
    char prefix[8], id[16], verb[24];
    const char *p = line;
    if(exit_mode) *exit_mode = false;
    if(!buf || n <= 0) return 0;
    buf[0] = 0;
    if(!mc0_next_token(&p, prefix, sizeof prefix) || strcmp(prefix, "MC0") != 0)
        return mc0_err(buf, n, "0", "bad_request", false, "expected MC0 prefix");
    if(!mc0_next_token(&p, id, sizeof id))
        return mc0_err(buf, n, "0", "bad_request", false, "missing request id");
    if(!mc0_next_token(&p, verb, sizeof verb))
        return mc0_err(buf, n, id, "bad_request", false, "missing command");

    if(strcmp(verb, "HELLO") == 0) return mc0_hello(buf, n, id);
    if(strcmp(verb, "IDENTITY") == 0) return mc0_identity(buf, n, id);
    if(strcmp(verb, "STATUS") == 0) return mc0_status(buf, n, id);
    if(strcmp(verb, "NODES") == 0) return mc0_nodes(buf, n, id, p);
    if(strcmp(verb, "THREADS") == 0) return mc0_threads(buf, n, id);
    if(strcmp(verb, "SEND_PUBLIC") == 0) return mc0_send_public(buf, n, id, p);
    if(strcmp(verb, "SEND_DM") == 0) return mc0_send_dm(buf, n, id, p);
    if(strcmp(verb, "EVENTS") == 0) {
        int pos = mc0_ok_prefix(buf, n, id);
        return buf_appendf(buf, n, pos, "events=off types=none event_seq=0\n");
    }
    if(strcmp(verb, "EXIT") == 0) {
        if(exit_mode) *exit_mode = true;
        int pos = mc0_ok_prefix(buf, n, id);
        return buf_appendf(buf, n, pos, "mode=usb state=detached\n");
    }
    return mc0_err(buf, n, id, "unknown_command", false, "unknown MC0 command");
}

int lz_svc_mc_companion_selftest(char *buf, int n)
{
    char out[900];
    bool exit_mode = false;
    bool ok = true;
    lz_svc_mc_companion_handle_line("MC0 1 HELLO proto=0 app=selftest", out, sizeof out, &exit_mode);
    ok = ok && strstr(out, "MC0 1 OK proto=0") != NULL;
    lz_svc_mc_companion_handle_line("MC0 2 STATUS", out, sizeof out, &exit_mode);
    ok = ok && strstr(out, "MC0 2 OK") != NULL && strstr(out, "bridge=usb") != NULL;
    lz_svc_mc_companion_handle_line("MC0 3 NODES", out, sizeof out, &exit_mode);
    ok = ok && strstr(out, "MC0 3 BEGIN type=nodes") != NULL &&
              strstr(out, "MC0 3 END type=nodes") != NULL;
    lz_svc_mc_companion_handle_line("MC0 4 EXIT", out, sizeof out, &exit_mode);
    ok = ok && exit_mode && strstr(out, "state=detached") != NULL;
    return snprintf(buf, (size_t)n, "MeshCore MC0 protocol selftest: %s", ok ? "PASS" : "FAIL");
}

/* ---------- inbound events from backends ---------- */

void lz_core_on_heard(uint32_t from, float snr)
{
    lz_node_rt *n = find_node(from);
    if(!n) return;
    if(!isnan(snr)) n->snr = snr;
    n->last_heard = now_epoch();
}

void lz_core_on_text(uint32_t from, uint32_t to, const char *text, int hops_used, float snr)
{
    bool broadcast = (to == LZ_BROADCAST);
    lz_node_rt *n = ensure_node(from, NULL, LZ_NET_MT);   /* may be NULL only if every slot is a saved contact */
    if(n) { if(!isnan(snr)) n->snr = snr; n->last_heard = now_epoch(); }

    /* broadcasts go to the LongFast channel (a group chat); directed messages
     * go to that sender's DM thread (which needs the node) */
    lz_thread_rt *t = broadcast ? lz_svc_channel_thread() : (n ? ensure_thread(n) : NULL);
    if(!t) return;                       /* no thread (DM w/o node, or table full): drop, never corrupt */
    if(!broadcast) {
        if(hops_used <= 0) snprintf(t->path, sizeof t->path, "direct");
        else snprintf(t->path, sizeof t->path, "%d hop%s", hops_used, hops_used > 1 ? "s" : "");
    }

    /* in a channel, prefix the sender so you can tell who said what */
    char stored[LZ_TEXT_MAX];
    if(broadcast) {
        char who[12];
        if(n && n->shortcode[0])  snprintf(who, sizeof who, "%s", n->shortcode);
        else if(n && n->name[0])  snprintf(who, sizeof who, "%s", n->name);
        else                      snprintf(who, sizeof who, "%04x", (unsigned)(from & 0xFFFF));
        snprintf(stored, sizeof stored, "%s: %s", who, text);
    } else          snprintf(stored, sizeof stored, "%s", text);

    uint32_t ts = now_epoch();
    lz_msg_rt m = { .self = false, .ts = ts };
    snprintf(m.text, sizeof m.text, "%s", stored);
    lz_store_append(t->addr, &m);
    if(g_open == t) tail_push(false, stored, ts, LZ_MSG_NONE, 0,
                              0, LZ_FAIL_NONE);
    touch_thread_meta(t, stored, ts, g_open != t);

    reorder_threads();
    lz_store_save_threads(g_threads, g_thread_count);
    lz_store_save_nodes(g_nodes, g_node_count);
    mark_dirty();
}

void lz_core_on_nodeinfo(uint32_t from, const char *id, const char *long_name,
                         const char *short_name, int role, const char *hw, float snr)
{
    lz_node_rt *n = ensure_node(from, id, LZ_NET_MT);
    if(long_name && long_name[0])  snprintf(n->name, sizeof n->name, "%s", long_name);
    if(short_name && short_name[0]) snprintf(n->shortcode, sizeof n->shortcode, "%s", short_name);
    if(hw && hw[0]) snprintf(n->hw, sizeof n->hw, "%s", hw);
    if(role >= 0) snprintf(n->role, sizeof n->role, "%s", lz_svc_mt_role_label(role));
    if(!isnan(snr)) n->snr = snr;
    n->last_heard = now_epoch();
    lz_store_save_nodes(g_nodes, g_node_count);
    mark_dirty();
}

/* MeshCore advert ingress: learn a node's name + role from a signed (and
 * unencrypted) ADVERT. Identity is keyed off the public key's leading bytes. */
void lz_core_on_mc_node(const uint8_t *pubkey, const char *name, int adv_type, float snr)
{
    uint32_t num = ((uint32_t)pubkey[0] << 24) | ((uint32_t)pubkey[1] << 16) |
                   ((uint32_t)pubkey[2] << 8) | (uint32_t)pubkey[3];
    char id[16];
    snprintf(id, sizeof id, "MC-%02x%02x%02x%02x",
             pubkey[0], pubkey[1], pubkey[2], pubkey[3]);
    lz_node_rt *n = ensure_node(num, id, LZ_NET_MC);
    if(!n) return;
    n->net = LZ_NET_MC;
    memcpy(n->pubkey, pubkey, 32);       /* persist the Ed25519 key so DM ECDH survives reboot */
    n->has_key = true;
    if(name && name[0]) snprintf(n->name, sizeof n->name, "%s", name);
    snprintf(n->shortcode, sizeof n->shortcode, "%02x%02x", pubkey[0], pubkey[1]);
    const char *role = adv_type == 2 ? "Repeater" : adv_type == 3 ? "Room"
                     : adv_type == 4 ? "Sensor"   : "Chat";
    snprintf(n->role, sizeof n->role, "%s", role);
    if(!isnan(snr)) n->snr = snr;
    n->last_heard = now_epoch();
    lz_store_save_nodes(g_nodes, g_node_count);
    mark_dirty();
}

/* append a MeshCore message (inbound or our own send) to a thread + persist */
static void mc_thread_append(lz_thread_rt *t, bool self, const char *text)
{
    if(!t) return;
    uint32_t ts = now_epoch();
    lz_msg_rt m = { .self = self, .ts = ts };
    snprintf(m.text, sizeof m.text, "%s", text);
    lz_store_append(t->addr, &m);
    if(g_open == t) tail_push(self, text, ts, LZ_MSG_NONE, 0, 0, LZ_FAIL_NONE);
    touch_thread_meta(t, text, ts, !self && g_open != t);   /* unread only for inbound */
    reorder_threads();
    lz_store_save_threads(g_threads, g_thread_count);
    mark_dirty();
}

static lz_thread_rt *mc_dm_thread(const uint8_t *pubkey, const char *name, float snr)
{
    uint32_t num = ((uint32_t)pubkey[0] << 24) | ((uint32_t)pubkey[1] << 16) |
                   ((uint32_t)pubkey[2] << 8) | (uint32_t)pubkey[3];
    lz_node_rt *n = find_node(num);
    if(!n) { lz_core_on_mc_node(pubkey, name, 1 /*Chat*/, snr); n = find_node(num); }
    if(!n) return NULL;
    n->net = LZ_NET_MC;
    if(name && name[0]) snprintf(n->name, sizeof n->name, "%s", name);
    lz_thread_rt *t = ensure_thread(n);
    if(t) { t->net = LZ_NET_MC; t->messageable = true; }   /* a chat peer we can DM back */
    return t;
}

/* MeshCore Public-channel message arrived -> MeshCore Public thread ("sender: text") */
void lz_core_on_mc_channel_text(const char *sender, const char *text, float snr)
{
    (void)snr;
    char stored[LZ_TEXT_MAX];
    if(sender && sender[0]) snprintf(stored, sizeof stored, "%s: %s", sender, text);
    else                    snprintf(stored, sizeof stored, "%s", text);
    mc_thread_append(lz_svc_mc_channel_thread(), false, stored);
}

/* our own Public-channel send -> show it in the Public thread */
void lz_core_on_mc_channel_self(const char *text)
{
    mc_thread_append(lz_svc_mc_channel_thread(), true, text);
}

/* a MeshCore direct message arrived from `pubkey` -> that peer's DM thread */
void lz_core_on_mc_dm(const uint8_t *pubkey, const char *name, const char *text, float snr)
{
    mc_thread_append(mc_dm_thread(pubkey, name, snr), false, text);
}

/* our own DM send -> show it in that peer's DM thread */
void lz_core_on_mc_dm_self(const uint8_t *pubkey, const char *name, const char *text)
{
    mc_thread_append(mc_dm_thread(pubkey, name, 0.0f), true, text);
}

void lz_core_on_battery(uint32_t from, int batt)
{
    lz_node_rt *n = find_node(from);
    if(n) {
        n->batt = batt;
        n->last_heard = now_epoch();
        nodes_mark_dirty();
        mark_dirty();
    }
}

void lz_core_on_position(uint32_t from, int32_t lat_i, int32_t lon_i,
                         bool has_alt, int32_t alt_m, uint32_t pos_time,
                         uint8_t precision_bits, float snr)
{
    lz_node_rt *n = ensure_node(from, NULL, LZ_NET_MT);
    if(!n) return;
    if(!isnan(snr)) n->snr = snr;
    n->last_heard = now_epoch();
    n->pos_flags |= LZ_NODE_POS_VALID;
    n->lat_i = lat_i;
    n->lon_i = lon_i;
    if(has_alt) {
        n->pos_flags |= LZ_NODE_POS_ALT;
        n->alt_m = alt_m;
    }
    if(precision_bits) {
        n->pos_flags |= LZ_NODE_POS_PREC;
        n->precision_bits = precision_bits;
    }
    n->pos_time = pos_time ? pos_time : n->last_heard;
    nodes_mark_dirty();
    mark_dirty();
}

static uint16_t clamp_u16(float v, float scale)
{
    if(!isfinite(v) || v < 0.0f) return 0;   /* NaN/Inf from a hostile packet */
    float s = v * scale + 0.5f;
    if(s > 65535.0f) return 65535;
    return (uint16_t)s;
}

static int16_t clamp_i16(float v, float scale)
{
    if(!isfinite(v)) return 0;               /* NaN->int cast is UB; reject it */
    float s = v * scale + (v >= 0.0f ? 0.5f : -0.5f);
    if(s > 32767.0f) return 32767;
    if(s < -32768.0f) return -32768;
    return (int16_t)s;
}

void lz_core_on_telemetry(uint32_t from, const lz_node_telemetry_t *telem, float snr)
{
    if(!telem) return;
    lz_node_rt *n = ensure_node(from, NULL, LZ_NET_MT);
    if(!n) return;
    if(!isnan(snr)) n->snr = snr;
    n->last_heard = now_epoch();
    if(telem->has_battery) n->batt = telem->battery_pct;
    if(telem->has_voltage) {
        n->telem_flags |= LZ_NODE_TEL_VOLT;
        n->voltage_mv = clamp_u16(telem->voltage, 1000.0f);
    }
    if(telem->has_uptime) {
        n->telem_flags |= LZ_NODE_TEL_UPTIME;
        n->uptime_s = telem->uptime_s;
    }
    if(telem->has_temperature) {
        n->telem_flags |= LZ_NODE_TEL_TEMP;
        n->temp_c10 = clamp_i16(telem->temperature_c, 10.0f);
    }
    if(telem->has_humidity) {
        n->telem_flags |= LZ_NODE_TEL_HUM;
        n->humidity10 = clamp_u16(telem->humidity_pct, 10.0f);
    }
    if(telem->has_pressure) {
        n->telem_flags |= LZ_NODE_TEL_PRESS;
        n->pressure10 = clamp_u16(telem->pressure_hpa, 10.0f);
    }
    nodes_mark_dirty();
    mark_dirty();
}

/* Meshtastic PKI: store a node's X25519 public key learned from its NodeInfo.
 * Kept in RAM only (re-learned each session); not persisted to nodes.db. */
void lz_core_on_pubkey(uint32_t from, const uint8_t *pub32)
{
    lz_node_rt *n = ensure_node(from, NULL, LZ_NET_MT);
    if(!n) return;
    if(n->has_key && memcmp(n->pubkey, pub32, 32) == 0) return;   /* unchanged */
    memcpy(n->pubkey, pub32, 32);
    n->has_key = true;
    lz_store_save_nodes(g_nodes, g_node_count);   /* persist so DMs survive reboot */
}

bool lz_svc_node_pubkey(uint32_t num, uint8_t out32[32])
{
    lz_node_rt *n = find_node(num);
    if(!n || !n->has_key) return false;
    memcpy(out32, n->pubkey, 32);
    return true;
}

/* a ROUTING ack came back for one of our sent DMs -> mark it delivered */
void lz_core_on_ack(uint32_t request_id)
{
    if(!request_id) return;
    const char *addr = delivery_addr(request_id);
    /* fall back to the open conversation only if it actually owns this pkt_id,
     * so a late/duplicate ACK can't flip an unrelated message in whatever
     * thread happens to be open */
    if(!addr && g_open) {
        lz_msg_rt probe;
        if(tail_find_delivery(request_id, &probe)) addr = g_open->addr;
    }
    if(!addr) return;            /* not a message we are tracking -> ignore */

    uint8_t retries = delivery_retries(request_id);
    if(!retries) retries = tail_delivery_retries(request_id);
    bool changed = false;
    if(g_open && strcmp(g_open->addr, addr) == 0)
        changed = tail_mark_delivery(request_id, request_id, LZ_MSG_DELIVERED,
                                     retries, LZ_FAIL_NONE);
    if(lz_store_update_delivery(addr, request_id, request_id,
                                LZ_MSG_DELIVERED, retries, LZ_FAIL_NONE))
        changed = true;
    delivery_forget(request_id);
    if(changed) mark_dirty();
}

/* expire un-acked DMs (call from the service loop): SENDING -> FAILED after ~30s */
static void age_sent_status(void)
{
    uint32_t now = lz_tick_ms();
    for(int i = 0; i < LZ_DELIVERY_PEND; i++) {
        if(!g_delivery[i].used) continue;
        if(now - g_delivery[i].sent_ms <= LZ_DELIVERY_ACK_TIMEOUT_MS) continue;
        uint32_t old_pid = g_delivery[i].pkt_id;
        uint8_t retries = g_delivery[i].retries;
        char addr[16];
        snprintf(addr, sizeof addr, "%s", g_delivery[i].addr);

        if(retries >= LZ_MSG_RETRY_MAX) {
            lz_store_update_delivery(addr, old_pid, old_pid, LZ_MSG_FAILED,
                                     retries, LZ_FAIL_RETRY_LIMIT);
            if(g_open && strcmp(g_open->addr, addr) == 0)
                tail_mark_delivery(old_pid, old_pid, LZ_MSG_FAILED,
                                   retries, LZ_FAIL_RETRY_LIMIT);
            g_delivery[i].used = false;
            mark_dirty();
            continue;
        }

        lz_thread_rt *t = find_thread_by_addr(addr);
        lz_msg_rt queued;
        if(!t || t->is_channel || !t->messageable ||
           !load_queued_delivery(addr, old_pid, &queued)) {
            lz_store_update_delivery(addr, old_pid, old_pid, LZ_MSG_FAILED,
                                     retries, LZ_FAIL_ACK_TIMEOUT);
            if(g_open && strcmp(g_open->addr, addr) == 0)
                tail_mark_delivery(old_pid, old_pid, LZ_MSG_FAILED,
                                   retries, LZ_FAIL_ACK_TIMEOUT);
            g_delivery[i].used = false;
            mark_dirty();
            continue;
        }

        uint8_t next_retries = (uint8_t)(retries + 1);
        uint32_t new_pid = next_packet_id();
        bool sent = send_text_packet(t, queued.text, new_pid);
        lz_store_update_delivery(addr, old_pid, sent ? new_pid : old_pid,
                                 sent ? LZ_MSG_SENDING : LZ_MSG_FAILED,
                                 next_retries,
                                 sent ? LZ_FAIL_NONE : LZ_FAIL_RADIO_SEND);
        if(g_open && strcmp(g_open->addr, addr) == 0)
            tail_mark_delivery(old_pid, sent ? new_pid : old_pid,
                               sent ? LZ_MSG_SENDING : LZ_MSG_FAILED,
                               next_retries,
                               sent ? LZ_FAIL_NONE : LZ_FAIL_RADIO_SEND);
        if(sent) {
            g_delivery[i].pkt_id = new_pid;
            g_delivery[i].sent_ms = now;
            g_delivery[i].retries = next_retries;
        } else {
            g_delivery[i].used = false;
        }
        mark_dirty();
    }
    now = lz_tick_ms();
    for(int i = 0; i < g_tail_count; i++)
        if(g_tail[i].self && g_tail[i].status == LZ_MSG_SENDING &&
           now - g_tail[i].sent_ms > LZ_DELIVERY_ACK_TIMEOUT_MS) {
            uint32_t old_pid = g_tail[i].pkt_id;
            uint8_t retries = g_tail[i].retries;
            if(retries >= LZ_MSG_RETRY_MAX) {
                g_tail[i].status = LZ_MSG_FAILED;
                g_tail[i].sent_ms = 0;
                g_tail[i].fail_reason = LZ_FAIL_RETRY_LIMIT;
                if(g_open && old_pid)
                    lz_store_update_delivery(g_open->addr, old_pid, old_pid,
                                             LZ_MSG_FAILED, retries,
                                             LZ_FAIL_RETRY_LIMIT);
                if(old_pid) delivery_forget(old_pid);
                mark_dirty();
                continue;
            }

            uint8_t next_retries = (uint8_t)(retries + 1);
            uint32_t new_pid = next_packet_id();
            bool sent = g_open && send_text_packet(g_open, g_tail[i].text, new_pid);
            if(sent) {
                if(g_open && old_pid)
                    lz_store_update_delivery(g_open->addr, old_pid, new_pid,
                                             LZ_MSG_SENDING, next_retries,
                                             LZ_FAIL_NONE);
                if(old_pid) delivery_forget(old_pid);
                g_tail[i].pkt_id = new_pid;
                g_tail[i].sent_ms = now;
                g_tail[i].retries = next_retries;
                g_tail[i].fail_reason = LZ_FAIL_NONE;
                if(g_open) delivery_track(g_open->addr, new_pid, next_retries);
            } else {
                g_tail[i].status = LZ_MSG_FAILED;
                g_tail[i].sent_ms = 0;
                g_tail[i].retries = next_retries;
                g_tail[i].fail_reason = LZ_FAIL_RADIO_SEND;
                if(g_open && old_pid)
                    lz_store_update_delivery(g_open->addr, old_pid, old_pid,
                                             LZ_MSG_FAILED, next_retries,
                                             LZ_FAIL_RADIO_SEND);
                if(old_pid) delivery_forget(old_pid);
            }
            mark_dirty();
        }
}

/* ---------- demo seed (matches the design's sample data) ---------- */

void lz_seed_demo(void);   /* in mesh_seed.c */

/* ---------- lifecycle ---------- */

void lz_svc_init(const char *datadir, bool seed_demo)
{
    /* Node DB (45 KB) lives in PSRAM on the T-Deck to keep internal DRAM free for
     * WiFi/BLE; the sim falls back to the heap. Allocated once, before any use. */
    if(!g_nodes) {
#ifdef LZ_TARGET_TDECK
        g_nodes = (lz_node_rt *)heap_caps_calloc(LZ_MAX_NODES, sizeof(lz_node_rt),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if(!g_nodes) g_nodes = (lz_node_rt *)calloc(LZ_MAX_NODES, sizeof(lz_node_rt));
    }

    lz_store_init(datadir);

    /* identity from a prior onboarding; absent -> UI shows onboarding first */
    char ln[24], sn[6];
    if(lz_store_load_identity(ln, sizeof ln, sn, sizeof sn)) {
        snprintf(g_id.long_name, sizeof g_id.long_name, "%s", ln);
        snprintf(g_id.short_name, sizeof g_id.short_name, "%s", sn);
        g_have_identity = true;
    }

    g_node_count = lz_store_load_nodes(g_nodes, LZ_MAX_NODES);
    g_thread_count = lz_store_load_threads(g_threads, LZ_MAX_THREADS);

    /* always have self in the table */
    if(!find_node(g_id.num)) {
        lz_node_rt *me = ensure_node(g_id.num, g_id.id, LZ_NET_MT);
        snprintf(me->name, sizeof me->name, "%s", g_id.long_name);
        snprintf(me->shortcode, sizeof me->shortcode, "%s", g_id.short_name);
    }

    if(seed_demo && g_thread_count == 0) lz_seed_demo();
    lz_svc_channel_thread();              /* LongFast always present, even on a clean device */
    if(LZ_MESHCORE_ENABLED) lz_svc_mc_channel_thread();   /* MeshCore Public always in Channels too */
    track_stored_delivery();
    reorder_threads();
    lz_backend_init();
}

void lz_svc_loop(void)
{
    lz_backend_loop();
    age_sent_status();        /* SENDING -> FAILED after the ack timeout */
    nodes_flush();            /* commit coalesced position/telemetry/battery */
}

void lz_svc_radio_stats(lz_radio_stats_t *out) { lz_backend_stats(out); }

/* seed helpers used by mesh_seed.c */
lz_node_rt *lz_seed_node(uint32_t num, const char *id, lz_net_t net, const char *name,
                         const char *sc, const char *role, float snr, int batt,
                         const char *hw, const char *dist, uint32_t ago_s, bool contact)
{
    lz_node_rt *n = ensure_node(num, id, net);
    snprintf(n->name, sizeof n->name, "%s", name);
    snprintf(n->shortcode, sizeof n->shortcode, "%s", sc);
    snprintf(n->role, sizeof n->role, "%s", role);
    n->snr = snr; n->batt = batt;
    snprintf(n->hw, sizeof n->hw, "%s", hw);
    snprintf(n->dist, sizeof n->dist, "%s", dist);
    n->last_heard = now_epoch() - ago_s;
    n->contact = contact;
    return n;
}

void lz_seed_thread(lz_node_rt *n, const char *path, const char *last, uint32_t ago_s,
                    int unread, const lz_msg_rt *history, int hist_n)
{
    lz_thread_rt *t = ensure_thread(n);
    if(!t) return;
    snprintf(t->path, sizeof t->path, "%s", path);
    uint32_t base = now_epoch() - ago_s;
    for(int i = 0; i < hist_n; i++) {
        lz_msg_rt m = history[i];
        m.ts = base - (hist_n - 1 - i) * 60;
        lz_store_append(t->addr, &m);
    }
    touch_thread_meta(t, last, base, false);
    t->unread = unread;
    lz_store_save_threads(g_threads, g_thread_count);
    lz_store_save_nodes(g_nodes, g_node_count);
}
