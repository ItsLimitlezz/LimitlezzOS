/**
 * Mesh service core — owns the node table and thread index, mediates
 * between the radio backend and the UI. Backend-agnostic: the sim and the
 * real SX1262 driver both call the lz_core_on_* hooks below.
 */
#include "mesh.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

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
static uint32_t      g_epoch_base = 1718200980;  /* maps tick 0 -> a wall clock */

/* ---------- helpers ---------- */

static uint32_t now_epoch(void)
{
    return g_epoch_base + lz_tick_ms() / 1000;
}

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

const char *lz_fmt_hm(uint32_t ts, char *buf, size_t n)
{
    uint32_t secs = ts % 86400;
    snprintf(buf, n, "%02u:%02u", (unsigned)(secs / 3600), (unsigned)((secs % 3600) / 60));
    return buf;
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

lz_node_rt *lz_svc_node_by_name(const char *name)
{
    for(int i = 0; i < g_node_count; i++)
        if(strcmp(g_nodes[i].name, name) == 0) return &g_nodes[i];
    return NULL;
}

static lz_node_rt *ensure_node(uint32_t num, const char *id, lz_net_t net)
{
    lz_node_rt *n = find_node(num);
    if(n) return n;
    if(g_node_count >= LZ_MAX_NODES) return NULL;
    n = &g_nodes[g_node_count++];
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
void lz_svc_set_time(uint32_t epoch)
{
    g_epoch_base = epoch - lz_tick_ms() / 1000;
    g_time_synced = true;
}
bool lz_svc_time_synced(void) { return g_time_synced; }
const char *lz_fmt_now(char *buf, size_t n)
{
    if(!g_time_synced) { snprintf(buf, n, "--:--"); return buf; }
    return lz_fmt_hm(now_epoch(), buf, n);
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
    if(t) return t;
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
}

int lz_svc_tail(const lz_msg_rt **out) { *out = g_tail; return g_tail_count; }

static void tail_push(bool self, const char *text, uint32_t ts)
{
    if(g_tail_count < LZ_TAIL_MAX) {
        lz_msg_rt *m = &g_tail[g_tail_count++];
        m->self = self; m->ts = ts;
        snprintf(m->text, sizeof m->text, "%s", text);
    } else {
        memmove(&g_tail[0], &g_tail[1], sizeof(lz_msg_rt) * (LZ_TAIL_MAX - 1));
        lz_msg_rt *m = &g_tail[LZ_TAIL_MAX - 1];
        m->self = self; m->ts = ts;
        snprintf(m->text, sizeof m->text, "%s", text);
    }
}

bool lz_svc_send_text(lz_thread_rt *t, const char *text)
{
    if(!t || !text[0] || !t->messageable) return false;
    uint32_t ts = now_epoch();
    lz_msg_rt m = { .self = true, .ts = ts };
    snprintf(m.text, sizeof m.text, "%s", text);
    lz_store_append(t->addr, &m);
    if(g_open == t) tail_push(true, text, ts);
    touch_thread_meta(t, text, ts, false);

    lz_mt_packet_t p;
    memset(&p, 0, sizeof p);
    p.to = t->node_num;                  /* LZ_BROADCAST for the channel */
    p.from = g_id.num;
    p.id = lz_tick_ms();
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
    lz_node_rt *n = ensure_node(from, NULL, LZ_NET_MT);
    if(!isnan(snr)) n->snr = snr;
    n->last_heard = now_epoch();

    /* broadcasts go to the LongFast channel (a group chat); directed messages
     * go to that sender's DM thread */
    lz_thread_rt *t = broadcast ? lz_svc_channel_thread() : ensure_thread(n);
    if(!t) return;                       /* table full: drop, never corrupt */
    if(!broadcast) {
        if(hops_used <= 0) snprintf(t->path, sizeof t->path, "direct");
        else snprintf(t->path, sizeof t->path, "%d hop%s", hops_used, hops_used > 1 ? "s" : "");
    }

    /* in a channel, prefix the sender so you can tell who said what */
    char stored[LZ_TEXT_MAX];
    if(broadcast) snprintf(stored, sizeof stored, "%s: %s",
                           n->shortcode[0] ? n->shortcode : n->name, text);
    else          snprintf(stored, sizeof stored, "%s", text);

    uint32_t ts = now_epoch();
    lz_msg_rt m = { .self = false, .ts = ts };
    snprintf(m.text, sizeof m.text, "%s", stored);
    lz_store_append(t->addr, &m);
    if(g_open == t) tail_push(false, stored, ts);
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

void lz_core_on_battery(uint32_t from, int batt)
{
    lz_node_rt *n = find_node(from);
    if(n) { n->batt = batt; mark_dirty(); }
}

void lz_core_on_ack(uint32_t request_id) { (void)request_id; }

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
