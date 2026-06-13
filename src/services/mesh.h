/**
 * LimitlezzOS mesh service — the real data layer behind the UI.
 *
 * One API, two backends:
 *   backend_sx1262.cpp  — real Meshtastic over SX1262/RadioLib (T-Deck)
 *   backend_sim.c       — simulated radio for the desktop simulator
 *
 * Spec mapping: Messaging service (§4.3), inbox data model (phase 1.6),
 * message history kept across network toggles (§6.5), plain-language
 * last-heard (§6.3). MeshCore + TDM arbiter arrive in Stage 2 behind this
 * same interface.
 */
#ifndef LZ_MESH_H
#define LZ_MESH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../ui/data.h"   /* lz_net_t */

#ifdef __cplusplus
extern "C" {
#endif

#define LZ_MAX_NODES    32
#define LZ_MAX_THREADS  16
#define LZ_TAIL_MAX     32
#define LZ_TEXT_MAX     200

typedef struct {
    uint32_t num;                /* node number (low 32 of MAC on Meshtastic) */
    char     id[16];             /* "!7c3af1d0" / "MC-4f8e" */
    char     name[28];
    char     shortcode[8];
    lz_net_t net;
    char     role[12];           /* Client / Router / Repeater / Chat / Sensor / Room */
    float    snr;                /* NAN = unknown */
    int      batt;               /* -1 = unknown */
    char     hw[16];
    char     dist[12];           /* "4.2 km" or "-" */
    uint32_t last_heard;         /* epoch seconds */
    bool     contact;            /* purposely added by the user */
} lz_node_rt;

typedef struct {
    char     addr[16];           /* node id string; also the log file key */
    char     name[28];
    lz_net_t net;
    uint32_t node_num;
    char     last_text[72];
    uint32_t last_ts;
    int      unread;
    char     path[12];           /* "2 hops" / "direct" */
    bool     messageable;
} lz_thread_rt;

typedef struct {
    bool     self;
    uint32_t ts;
    char     text[LZ_TEXT_MAX];
} lz_msg_rt;

/* ---- lifecycle ---- */
void lz_svc_init(const char *datadir, bool seed_demo);  /* datadir NULL = RAM only */
void lz_svc_loop(void);                                 /* pump backend + timers   */
void lz_svc_set_dirty_cb(void (*cb)(void));             /* UI refresh request      */

/* ---- nodes ---- */
int  lz_svc_nodes(const lz_node_rt **out);              /* all heard nodes */
lz_node_rt *lz_svc_node_by_name(const char *name);
void lz_svc_add_contact(lz_node_rt *n);
bool lz_node_messageable(const lz_node_rt *n);          /* people, not infrastructure */
int  lz_svc_node_count(lz_net_t net);

/* ---- threads / messages ----
 * Storage is stable: thread/node pointers never move, so the UI can hold a
 * pointer to an open conversation. Display order (newest-first) is a separate
 * index, read via lz_svc_thread_at(). */
int  lz_svc_thread_count_all(void);
lz_thread_rt *lz_svc_thread_at(int display_idx);        /* newest-first */
lz_thread_rt *lz_svc_thread_for_node(lz_node_rt *n);    /* find or create */
void lz_svc_open_thread(lz_thread_rt *t);               /* load tail, clear unread */
int  lz_svc_tail(const lz_msg_rt **out);                /* tail of the open thread */
bool lz_svc_send_text(lz_thread_rt *t, const char *text);

/* ---- radio stats (airtime accounting) ---- */
typedef struct { uint32_t tx_count, rx_count; float util_pct; } lz_radio_stats_t;
void lz_svc_radio_stats(lz_radio_stats_t *out);

/* ---- identity ---- */
typedef struct { uint32_t num; char id[16]; char long_name[24]; char short_name[6]; } lz_identity_t;
const lz_identity_t *lz_svc_identity(void);
bool lz_svc_needs_onboarding(void);                       /* no saved identity yet */
void lz_svc_set_identity(const char *long_name, const char *short_name);

/* ---- formatting helpers ---- */
const char *lz_fmt_ago(uint32_t ts, char *buf, size_t n);   /* "now" "2m" "1h" "3d" */
const char *lz_fmt_hm(uint32_t ts, char *buf, size_t n);    /* "14:23" */

/* ---- internal: core <-> backend contract ---- */
typedef struct {
    uint32_t to, from, id;
    uint8_t  hop_limit, hop_start;
    bool     want_ack;
    uint8_t  portnum;
    uint8_t  payload[237];
    uint8_t  plen;
} lz_mt_packet_t;

void lz_backend_init(void);
void lz_backend_loop(void);
bool lz_backend_send(lz_mt_packet_t *p);
void lz_backend_stats(lz_radio_stats_t *out);

/* called by backends on radio events */
void lz_core_on_text(uint32_t from, uint32_t to, const char *text, int hops_used, float snr);
void lz_core_on_nodeinfo(uint32_t from, const char *id, const char *long_name,
                         const char *short_name, int role, const char *hw, float snr);
void lz_core_on_heard(uint32_t from, float snr);        /* any packet from a node */
void lz_core_on_battery(uint32_t from, int batt);
void lz_core_on_ack(uint32_t request_id);

#ifdef __cplusplus
}
#endif

#endif
