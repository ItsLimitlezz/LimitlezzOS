/**
 * Mesh service core — owns the node table and thread index, mediates
 * between the radio backend and the UI. Backend-agnostic: the sim and the
 * real SX1262 driver both call the lz_core_on_* hooks below.
 */
#include "mesh.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/* store.c */
void lz_store_init(const char *datadir);
void lz_store_append(const char *addr, const lz_msg_rt *m);
int  lz_store_load_tail(const char *addr, lz_msg_rt *ring, int cap);
void lz_store_save_threads(const lz_thread_rt *t, int n);
int  lz_store_load_threads(lz_thread_rt *out, int cap);
void lz_store_save_nodes(const lz_node_rt *nodes, int n);
int  lz_store_load_nodes(lz_node_rt *out, int cap);
void lz_store_save_identity(const char *longn, const char *shortn);
bool lz_store_load_identity(char *longn, int ln, char *shortn, int sn);

/* monotonic clock supplied by the platform layer */
extern uint32_t lz_tick_ms(void);

static lz_node_rt    g_nodes[LZ_MAX_NODES];
static int           g_node_count;
static lz_thread_rt  g_threads[LZ_MAX_THREADS];   /* stable: never reordered */
static int           g_thread_count;
static int           g_order[LZ_MAX_THREADS];     /* display order, newest first */
static lz_msg_rt     g_tail[LZ_TAIL_MAX];
static int           g_tail_count;
static lz_thread_rt *g_open;
static void        (*g_dirty)(void);
/* placeholder until onboarding sets the real name (and MAC sets the num) */
static lz_identity_t g_id = { 0x7c3af1d0, "!7c3af1d0", "Node", "NODE" };
static bool          g_have_identity;            /* false until onboarding done */
static uint32_t      g_epoch_base = 1718200980;  /* UTC at uptime 0 */
static int           g_tz_std_min;               /* standard-time offset, minutes */
static int           g_dst_rule;                 /* LZ_DST_NONE / _US / _EU */
static char          g_tz_std_ab[6] = "UTC";     /* abbrev in standard time */
static char          g_tz_dst_ab[6] = "UTC";     /* abbrev in daylight time */

/* ---------- helpers ---------- */

static uint32_t now_epoch(void)                  /* UTC */
{
    return g_epoch_base + lz_tick_ms() / 1000;
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

/* is daylight saving in effect at this UTC instant for the active zone? */
static bool dst_at(uint32_t utc)
{
    if(g_dst_rule == LZ_DST_NONE) return false;
    if(g_dst_rule == LZ_DST_US) {
        /* 2nd Sun Mar 02:00 .. 1st Sun Nov 02:00, in local standard time */
        time_t t = (time_t)(utc + g_tz_std_min * 60);
        struct tm tm; gmtime_r(&t, &tm);
        int m = tm.tm_mon + 1, d = tm.tm_mday, h = tm.tm_hour;
        int y = tm.tm_year + 1900;
        if(m < 3 || m > 11) return false;
        if(m > 3 && m < 11) return true;
        if(m == 3) { int s = nth_sunday(y, 3, 2);  return d > s || (d == s && h >= 2); }
        int e = nth_sunday(y, 11, 1);              return d < e || (d == e && h < 2);
    }
    /* EU: last Sun Mar 01:00 UTC .. last Sun Oct 01:00 UTC */
    time_t t = (time_t)utc;
    struct tm tm; gmtime_r(&t, &tm);
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
    return strcmp(n->role, "Client") == 0 || strcmp(n->role, "Chat") == 0;
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
    struct tm tmv; gmtime_r(&t, &tmv);
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
    gmtime_r(&t, &tmv);
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
    lz_thread_rt *t = find_thread(LZ_BROADCAST);
    if(t) {                              /* re-assert channel props (survives reload) */
        t->is_channel = true;
        t->messageable = true;
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

void lz_svc_open_thread(lz_thread_rt *t)
{
    if(!t) return;
    g_open = t;
    t->unread = 0;
    g_tail_count = lz_store_load_tail(t->addr, g_tail, LZ_TAIL_MAX);
    lz_store_save_threads(g_threads, g_thread_count);
    /* opening a Meshtastic DM without the peer's key -> request its NodeInfo so
     * we can PKI-encrypt (the reply carries the key, usually within a second) */
    if(!t->is_channel && t->node_num != LZ_BROADCAST && t->net == LZ_NET_MT) {
        uint8_t k[32];
        if(!lz_svc_node_pubkey(t->node_num, k)) lz_backend_request_nodeinfo(t->node_num);
    }
}

int lz_svc_tail(const lz_msg_rt **out) { *out = g_tail; return g_tail_count; }

static void tail_push(bool self, const char *text, uint32_t ts, uint8_t status, uint32_t pkt_id)
{
    lz_msg_rt *m;
    if(g_tail_count < LZ_TAIL_MAX) m = &g_tail[g_tail_count++];
    else { memmove(&g_tail[0], &g_tail[1], sizeof(lz_msg_rt) * (LZ_TAIL_MAX - 1));
           m = &g_tail[LZ_TAIL_MAX - 1]; }
    memset(m, 0, sizeof *m);
    m->self = self; m->ts = ts;
    m->status = status; m->pkt_id = pkt_id; m->sent_ms = lz_tick_ms();
    snprintf(m->text, sizeof m->text, "%s", text);
}

bool lz_svc_send_text(lz_thread_rt *t, const char *text)
{
    if(!t || !text[0] || !t->messageable) return false;
    uint32_t ts = now_epoch();
    uint32_t pid = lz_tick_ms(); if(!pid) pid = 1;
    bool track = !t->is_channel;          /* DMs get delivery status; channels don't */
    uint8_t status = track ? LZ_MSG_SENDING : LZ_MSG_NONE;
    lz_msg_rt m = { .self = true, .ts = ts };
    snprintf(m.text, sizeof m.text, "%s", text);
    lz_store_append(t->addr, &m);
    if(g_open == t) tail_push(true, text, ts, status, pid);
    touch_thread_meta(t, text, ts, false);

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
    lz_backend_send(&p);

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
    uint32_t pid = lz_tick_ms(); if(!pid) pid = 1;
    m->pkt_id = pid; m->sent_ms = lz_tick_ms(); m->status = LZ_MSG_SENDING;

    lz_mt_packet_t p;
    memset(&p, 0, sizeof p);
    p.to = g_open->node_num; p.from = g_id.num; p.id = pid;
    p.hop_limit = 3; p.hop_start = 3; p.want_ack = !g_open->is_channel; p.portnum = 1;
    size_t len = strlen(m->text);
    if(len > sizeof p.payload) len = sizeof p.payload;
    memcpy(p.payload, m->text, len); p.plen = (uint8_t)len;
    lz_backend_send(&p);
    mark_dirty();
    return true;
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
    if(g_open == t) tail_push(false, stored, ts, LZ_MSG_NONE, 0);
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
    if(role == 1 || role == 2) snprintf(n->role, sizeof n->role, "Router");
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
    snprintf(id, sizeof id, "MC-%02x%02x", pubkey[0], pubkey[1]);
    lz_node_rt *n = ensure_node(num, id, LZ_NET_MC);
    if(!n) return;
    n->net = LZ_NET_MC;
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

void lz_core_on_battery(uint32_t from, int batt)
{
    lz_node_rt *n = find_node(from);
    if(n) { n->batt = batt; mark_dirty(); }
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
    for(int i = 0; i < g_tail_count; i++)
        if(g_tail[i].self && g_tail[i].pkt_id == request_id &&
           (g_tail[i].status == LZ_MSG_SENDING || g_tail[i].status == LZ_MSG_FAILED)) {
            g_tail[i].status = LZ_MSG_DELIVERED;
            mark_dirty();
            return;
        }
}

/* expire un-acked DMs (call from the service loop): SENDING -> FAILED after ~30s */
static void age_sent_status(void)
{
    uint32_t now = lz_tick_ms();
    for(int i = 0; i < g_tail_count; i++)
        if(g_tail[i].self && g_tail[i].status == LZ_MSG_SENDING &&
           now - g_tail[i].sent_ms > 30000) {
            g_tail[i].status = LZ_MSG_FAILED;
            mark_dirty();
        }
}

/* ---------- demo seed (matches the design's sample data) ---------- */

void lz_seed_demo(void);   /* in mesh_seed.c */

/* ---------- lifecycle ---------- */

void lz_svc_init(const char *datadir, bool seed_demo)
{
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
    reorder_threads();
    lz_backend_init();
}

void lz_svc_loop(void)
{
    lz_backend_loop();
    age_sent_status();        /* SENDING -> FAILED after the ack timeout */
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
