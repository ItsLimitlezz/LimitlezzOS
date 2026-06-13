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

#define LZ_MAX_NODES    250    /* matches the official Meshtastic device-ui (MUI) */
#define LZ_MAX_THREADS  48
#define LZ_TAIL_MAX     32
#define LZ_TEXT_MAX     200

/* MeshCore (2nd RF profile, TDM with Meshtastic) is built but not receive-ready,
 * so it's shown as "Coming soon" / grayed for the Alpha. Flip to 1 to enable. */
#define LZ_MESHCORE_ENABLED 0

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
    uint8_t  pubkey[32];         /* Meshtastic X25519 public key (PKI DMs) */
    bool     has_key;            /* pubkey known (learned from NodeInfo this session) */
} lz_node_rt;

#define LZ_BROADCAST 0xFFFFFFFFu     /* Meshtastic broadcast addr (primary channel) */

typedef struct {
    char     addr[16];           /* node id string; also the log file key */
    char     name[28];
    lz_net_t net;
    uint32_t node_num;           /* LZ_BROADCAST for the broadcast channel */
    char     last_text[72];
    uint32_t last_ts;
    int      unread;
    char     path[12];           /* "2 hops" / "direct" / "broadcast" */
    bool     messageable;
    bool     is_channel;         /* true = broadcast channel (e.g. LongFast) */
    bool     muted;              /* silenced: no notification / unread badge (v0.44) */
} lz_thread_rt;

/* delivery status for our own DMs (RAM-only, not persisted) */
enum { LZ_MSG_NONE = 0, LZ_MSG_SENDING, LZ_MSG_DELIVERED, LZ_MSG_FAILED };

typedef struct {
    bool     self;
    uint32_t ts;
    char     text[LZ_TEXT_MAX];
    uint8_t  status;             /* LZ_MSG_* — sent DMs only */
    uint32_t pkt_id;             /* packet id, to match the ROUTING ack */
    uint32_t sent_ms;            /* lz_tick_ms() at send, for ack timeout */
} lz_msg_rt;

/* ---- lifecycle ---- */
void lz_svc_init(const char *datadir, bool seed_demo);  /* datadir NULL = RAM only */
void lz_svc_loop(void);                                 /* pump backend + timers   */
void lz_svc_set_dirty_cb(void (*cb)(void));             /* UI refresh request      */

/* ---- nodes ---- */
int  lz_svc_nodes(const lz_node_rt **out);              /* all heard nodes */
lz_node_rt *lz_svc_node_by_name(const char *name);
lz_node_rt *lz_svc_node_by_shortcode(const char *sc);  /* for channel long-press DM */
lz_node_rt *lz_svc_node_by_num(uint32_t num);          /* node for a DM thread */
void lz_svc_add_contact(lz_node_rt *n);
bool lz_node_messageable(const lz_node_rt *n);          /* people, not infrastructure */
int  lz_svc_node_count(lz_net_t net);

/* ---- threads / messages ----
 * Storage is stable: thread/node pointers never move, so the UI can hold a
 * pointer to an open conversation. Display order (newest-first) is a separate
 * index, read via lz_svc_thread_at(). */
int  lz_svc_thread_count_all(void);
lz_thread_rt *lz_svc_top_unread(void);    /* most recent unread (lock-screen notification) */
lz_thread_rt *lz_svc_thread_at(int display_idx);        /* newest-first */
lz_thread_rt *lz_svc_thread_for_node(lz_node_rt *n);    /* find or create */
lz_thread_rt *lz_svc_channel_thread(void);              /* LongFast broadcast channel */
void lz_svc_open_thread(lz_thread_rt *t);               /* load tail, clear unread */
int  lz_svc_tail(const lz_msg_rt **out);                /* tail of the open thread */
bool lz_svc_send_text(lz_thread_rt *t, const char *text);
bool lz_svc_resend(int tail_idx);     /* retry a failed sent DM (long-press) */

/* ---- radio stats (airtime accounting) ---- */
typedef struct { uint32_t tx_count, rx_count; float util_pct; } lz_radio_stats_t;
void lz_svc_radio_stats(lz_radio_stats_t *out);

/* ---- identity ---- */
typedef struct { uint32_t num; char id[16]; char long_name[24]; char short_name[6]; } lz_identity_t;
const lz_identity_t *lz_svc_identity(void);
bool lz_svc_needs_onboarding(void);                       /* no saved identity yet */
void lz_svc_set_identity(const char *long_name, const char *short_name);
void lz_svc_set_node_num(uint32_t num);                   /* real node id (from MAC); call before init */

/* ---- time ---- */
void lz_svc_set_time(uint32_t epoch);                     /* set UTC (e.g. NTP) */
bool lz_svc_time_synced(void);
uint32_t lz_svc_epoch(void);                              /* current UTC epoch seconds */
void lz_svc_set_tz(int offset_min);                       /* fixed offset, no DST (legacy) */
int  lz_svc_tz(void);                                     /* effective offset incl. DST, min */
/* DST-aware zones: a standard offset + a daylight rule. now_local() and the
 * formatters apply the +1h shift automatically for the current date. */
enum { LZ_DST_NONE = 0, LZ_DST_US = 1, LZ_DST_EU = 2 };
void lz_svc_set_tz_zone(int std_min, int dst_rule, const char *std_ab, const char *dst_ab);
bool lz_svc_dst_active(void);                             /* is DST in effect right now? */
const char *lz_svc_tz_abbrev(void);                       /* "EST"/"EDT" for the current date */
void lz_svc_set_clock24(bool on);                         /* 24-hour vs 12-hour (AM/PM) */
bool lz_svc_clock24(void);
void lz_svc_set_clock(int y, int mo, int d, int h, int mi);  /* manual set (local) */
void lz_svc_get_clock(int *y, int *mo, int *d, int *h, int *mi);
const char *lz_fmt_now(char *buf, size_t n);              /* current HH:MM, or "--:--" if unsynced */
const char *lz_fmt_date(char *buf, size_t n);            /* "Friday, Jun 13", or a hint if unsynced */

/* ---- live system info (real hardware values; the sim uses demo numbers) ---- */
typedef struct {
    int      battery_pct;     /* -1 if unknown */
    float    battery_v;       /* 0 if unknown   */
    bool     charging;
    bool     usb;             /* on USB power   */
    int      cpu_mhz;
    int      ram_used_kb, ram_total_kb;
    int      flash_used_kb, flash_total_kb;
    int      temp_c;          /* -1000 if unknown */
    uint32_t uptime_s;
} lz_sysinfo_t;
void lz_svc_sysinfo(lz_sysinfo_t *out);
void lz_set_sysinfo_cb(void (*cb)(lz_sysinfo_t *out));    /* platform provides real values */

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
bool lz_backend_ok(void);            /* radio init succeeded (diagnostics) */
int  lz_backend_begin_state(void);   /* RadioLib begin() return code */
void lz_backend_set_tx_power(int dbm);  /* live TX power change */
void lz_backend_set_networks(bool mt, bool mc);  /* drive the TDM schedule */
void lz_backend_request_nodeinfo(uint32_t to);   /* ask a node for its NodeInfo (PKI key) */
bool lz_backend_mc_advert_now(bool flood);       /* send a MeshCore self-advert (flood/zero-hop) */
void lz_backend_mc_addr(char *buf, int n);       /* our MeshCore address, e.g. "MC-978bbe5f" */
/* companion bridge: USB serial speaks the Meshtastic app protocol when active */
bool lz_mtc_active(void);
void lz_mtc_set_active(bool on);

/* called by backends on radio events */
void lz_core_on_text(uint32_t from, uint32_t to, const char *text, int hops_used, float snr);
void lz_core_on_nodeinfo(uint32_t from, const char *id, const char *long_name,
                         const char *short_name, int role, const char *hw, float snr);
void lz_core_on_heard(uint32_t from, float snr);        /* any packet from a node */
void lz_core_on_battery(uint32_t from, int batt);
void lz_core_on_ack(uint32_t request_id);
/* MeshCore: learn a node from a (signed, unencrypted) ADVERT.
 * adv_type: 1=Chat 2=Repeater 3=Room 4=Sensor */
void lz_core_on_mc_node(const uint8_t *pubkey, const char *name, int adv_type, float snr);
/* Meshtastic PKI: learn a node's X25519 public key (from its NodeInfo) */
void lz_core_on_pubkey(uint32_t from, const uint8_t *pub32);
bool lz_svc_node_pubkey(uint32_t num, uint8_t out32[32]);   /* true if known */

#ifdef __cplusplus
}
#endif

#endif
