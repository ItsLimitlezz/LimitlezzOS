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
#define LZ_MAX_LOCAL_APPS 12
#define LZ_MAX_LOCAL_APP_ISSUES 8
#define LZ_LOCAL_APP_BODY_MAX 360
#define LZ_LOCAL_APP_ENTRY_MAX 1024u
#define LZ_LOCAL_APP_RUNTIME_BUDGET_BYTES 704u
#define LZ_LOCAL_APP_DATA_QUOTA_BYTES (64u * 1024u)
#define LZ_LOCAL_APP_ACTION_MAX 2
#define LZ_LOCAL_APP_ACTION_EFFECT_MAX 32
#define LZ_LOCAL_APP_ACTION_BODY_MAX 192
#define LZ_FEEDBACK_SOURCE_MAX 24
#define LZ_FEEDBACK_TITLE_MAX 32
#define LZ_FEEDBACK_BODY_MAX 96
#define LZ_APP_CATALOG_JSON_MAX 4096u
#define LZ_APP_CATALOG_MAX_APPS 24
#define LZ_OTA_MANIFEST_SCHEMA "limitlezz.ota_manifest.v1"
#define LZ_OTA_BOARD_TDECK "tdeck"
#define LZ_OTA_SLOT_MAX_BYTES 0x500000u
#define LZ_SECURITY_PIN_MIN 4
#define LZ_SECURITY_PIN_MAX 12
#define LZ_SECURITY_KDF_ROUNDS 2048u

#define LZ_APP_PERM_DISPLAY       0x0001u
#define LZ_APP_PERM_INPUT         0x0002u
#define LZ_APP_PERM_STORAGE       0x0004u
#define LZ_APP_PERM_MESH_READ     0x0008u
#define LZ_APP_PERM_MESH_SEND     0x0010u
#define LZ_APP_PERM_SYSTEM_TIME   0x0020u
#define LZ_APP_PERM_BATTERY       0x0040u
#define LZ_APP_PERM_NOTIFICATIONS 0x0080u
#define LZ_APP_PERM_NETWORK_WIFI  0x0100u

enum {
    LZ_APP_SOURCE_OFFICIAL = 0,
    LZ_APP_SOURCE_COMMUNITY = 1,
    LZ_APP_SOURCE_LOCAL_ONLY = 2,
    LZ_APP_SOURCE_COUNT = 3,
};

static inline int lz_app_source_clamp(int source)
{
    return (source >= 0 && source < LZ_APP_SOURCE_COUNT) ? source : LZ_APP_SOURCE_OFFICIAL;
}

static inline const char *lz_app_source_label(int source)
{
    switch(lz_app_source_clamp(source)) {
        case LZ_APP_SOURCE_COMMUNITY: return "Community";
        case LZ_APP_SOURCE_LOCAL_ONLY: return "Local only";
        default: return "Official";
    }
}

/* MeshCore (2nd RF profile, TDM with Meshtastic) is built but not receive-ready,
 * so it's shown as "Coming soon" / grayed for the Alpha. Default off; a dev/sim
 * build can enable the V0.6 work with -DLZ_MESHCORE_ENABLED=1. */
#ifndef LZ_MESHCORE_ENABLED
#define LZ_MESHCORE_ENABLED 0
#endif

enum {
    LZ_AIRTIME_MT_FIRST = 0,   /* 60/40: keeps LongFast's slower airtime favored */
    LZ_AIRTIME_BALANCED = 1,   /* 50/50 */
    LZ_AIRTIME_MC_FIRST = 2,   /* 40/60 */
    LZ_AIRTIME_COUNT = 3,
    LZ_AIRTIME_DEFAULT = LZ_AIRTIME_MT_FIRST
};

static inline int lz_airtime_mode_clamp(int mode)
{
    return (mode >= 0 && mode < LZ_AIRTIME_COUNT) ? mode : LZ_AIRTIME_DEFAULT;
}

static inline const char *lz_airtime_mode_label(int mode)
{
    switch(lz_airtime_mode_clamp(mode)) {
        case LZ_AIRTIME_BALANCED: return "Balanced";
        case LZ_AIRTIME_MC_FIRST: return "MeshCore first";
        default: return "Meshtastic first";
    }
}

static inline void lz_airtime_split_pct(int mode, int *mt_pct, int *mc_pct)
{
    int mt = 60, mc = 40;
    switch(lz_airtime_mode_clamp(mode)) {
        case LZ_AIRTIME_BALANCED: mt = 50; mc = 50; break;
        case LZ_AIRTIME_MC_FIRST: mt = 40; mc = 60; break;
        default: break;
    }
    if(mt_pct) *mt_pct = mt;
    if(mc_pct) *mc_pct = mc;
}

typedef struct {
    bool     has_battery;
    int      battery_pct;
    bool     has_voltage;
    float    voltage;
    bool     has_uptime;
    uint32_t uptime_s;
    bool     has_temperature;
    float    temperature_c;
    bool     has_humidity;
    float    humidity_pct;
    bool     has_pressure;
    float    pressure_hpa;
} lz_node_telemetry_t;

#define LZ_NODE_POS_VALID   0x01u
#define LZ_NODE_POS_ALT     0x02u
#define LZ_NODE_POS_PREC    0x04u
#define LZ_NODE_TEL_VOLT    0x01u
#define LZ_NODE_TEL_TEMP    0x02u
#define LZ_NODE_TEL_HUM     0x04u
#define LZ_NODE_TEL_PRESS   0x08u
#define LZ_NODE_TEL_UPTIME  0x10u
#define LZ_NODE_DB_SCHEMA_VERSION 2u

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
    uint8_t  pos_flags;          /* LZ_NODE_POS_* */
    int32_t  lat_i, lon_i;       /* Meshtastic degrees * 1e7 */
    int32_t  alt_m;              /* meters, if LZ_NODE_POS_ALT */
    uint32_t pos_time;           /* GPS epoch/timestamp if sent */
    uint8_t  precision_bits;
    uint8_t  telem_flags;        /* LZ_NODE_TEL_* */
    uint16_t voltage_mv;
    int16_t  temp_c10;
    uint16_t humidity10;
    uint16_t pressure10;
    uint32_t uptime_s;
} lz_node_rt;

#define LZ_BROADCAST 0xFFFFFFFFu     /* Meshtastic broadcast addr (primary channel) */
#define LZ_MSG_RETRY_MAX 3

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

/* delivery status for our own DMs */
enum { LZ_MSG_NONE = 0, LZ_MSG_SENDING, LZ_MSG_DELIVERED, LZ_MSG_FAILED };
enum {
    LZ_FAIL_NONE = 0,
    LZ_FAIL_RADIO_SEND,
    LZ_FAIL_ACK_TIMEOUT,
    LZ_FAIL_RETRY_LIMIT
};

typedef struct {
    bool     self;
    uint32_t ts;
    char     text[LZ_TEXT_MAX];
    uint8_t  retries;            /* manual resend attempts after the first send */
    uint8_t  fail_reason;        /* LZ_FAIL_* when status == LZ_MSG_FAILED */
    uint8_t  status;             /* LZ_MSG_* — sent DMs only */
    uint32_t pkt_id;             /* packet id, to match the ROUTING ack */
    uint32_t sent_ms;            /* lz_tick_ms() at send, for ack timeout */
} lz_msg_rt;

typedef struct {
    char id[24];                 /* manifest id, safe filename token */
    char name[32];
    char version[16];
    char author[28];
    char summary[72];
    char entry[48];              /* relative script entrypoint */
    char api_version[12];        /* SDK compatibility gate */
    char icon[20];               /* symbolic icon token, mapped by UI */
    char path[112];              /* package directory */
    uint16_t permissions;         /* LZ_APP_PERM_* declared by manifest */
    int  hue;                    /* tile hue, -1 = neutral */
} lz_local_app_t;

typedef struct {
    char package[32];            /* folder name, safe for diagnostics only */
    char reason[48];             /* short plain-language rejection reason */
    char path[112];              /* package directory */
} lz_local_app_issue_t;

typedef struct {
    bool ok;
    int  app_count;
    int  rejected_count;
    char first_id[24];
    char first_error[64];
} lz_app_catalog_report_t;

typedef struct {
    char label[24];              /* app-provided foreground control label */
    char status[48];             /* bounded status shown after activation */
    char effect[LZ_LOCAL_APP_ACTION_EFFECT_MAX]; /* optional safe SDK effect */
    char body[LZ_LOCAL_APP_ACTION_BODY_MAX];
} lz_local_app_action_t;

typedef struct {
    char title[32];              /* app-provided display title */
    char body[LZ_LOCAL_APP_BODY_MAX];
    char status[64];             /* short runtime/sandbox state */
    char data_path[112];         /* prepared only when storage is declared */
    char error[48];              /* launch blocked reason, if any */
    char fault[64];              /* bounded launch/action fault snapshot */
    uint32_t data_used_bytes;
    uint32_t data_quota_bytes;
    uint32_t entry_source_bytes;
    uint32_t runtime_used_bytes;  /* app-controlled resident text/action state */
    uint32_t runtime_budget_bytes;
    uint16_t permissions;         /* manifest permissions captured at launch */
    bool entry_loaded;
    bool storage_ready;
    uint8_t action_count;
    uint8_t action_last;         /* 1-based selected action, 0 = none */
    lz_local_app_action_t actions[LZ_LOCAL_APP_ACTION_MAX];
} lz_local_app_session_t;

typedef struct {
    bool found;
    bool valid;
    char source[112];            /* cached manifest file path */
    char error[48];              /* plain-language rejection reason */
    char version[24];
    char channel[16];
    char board[16];
    char firmware_url[128];
    char sha256[65];
    uint32_t size_bytes;
    char min_version[24];
    char notes_url[96];
} lz_ota_manifest_t;

typedef struct {
    bool configured;             /* a device PIN verifier exists */
    bool valid;                  /* false = security.cfg is corrupt/unsupported */
    uint32_t rounds;             /* verifier KDF work factor */
    char salt[17];               /* hex salt, diagnostics only */
    char error[48];              /* unset / corrupt reason */
} lz_security_status_t;
#define LZ_APP_CATALOG_CACHE_MAX 4096

/* ---- lifecycle ---- */
void lz_svc_init(const char *datadir, bool seed_demo);  /* datadir NULL = RAM only */
void lz_svc_loop(void);                                 /* pump backend + timers   */
void lz_svc_set_dirty_cb(void (*cb)(void));             /* UI refresh request      */
void lz_svc_set_appfs_root(const char *root);           /* optional FAT appfs mount */
const char *lz_svc_file_root(void);                     /* read-only Files browser root */
int  lz_svc_file_roots(const char **out, int cap);      /* SD/local + appfs roots */
int  lz_svc_scan_apps(lz_local_app_t *out, int cap);    /* SD/appfs local manifests */
int  lz_svc_scan_app_issues(lz_local_app_issue_t *out, int cap); /* rejected manifests */
bool lz_svc_prepare_app_data(const lz_local_app_t *app, char *path_out, int path_cap,
                             char *err, int err_cap);   /* scoped app data dir */
bool lz_svc_app_data_usage(const lz_local_app_t *app, uint32_t *used, uint32_t *quota,
                           char *err, int err_cap);
bool lz_svc_clear_app_data(const lz_local_app_t *app, char *err, int err_cap);
bool lz_svc_uninstall_local_app(const lz_local_app_t *app, bool keep_data,
                                char *err, int err_cap);
bool lz_svc_save_app_catalog_cache(const char *json, int len, char *err, int err_cap);
bool lz_svc_load_app_catalog_cache(char *out, int cap, int *out_len, char *err, int err_cap);
bool lz_svc_clear_app_catalog_cache(char *err, int err_cap);
bool lz_svc_start_local_app(const lz_local_app_t *app, lz_local_app_session_t *out);
bool lz_svc_local_app_action(lz_local_app_session_t *session, int idx);
void lz_svc_stop_local_app(lz_local_app_session_t *session);
uint32_t lz_local_app_runtime_used(const lz_local_app_session_t *session);
void lz_local_app_runtime_refresh(lz_local_app_session_t *session);
bool lz_local_app_runtime_within_budget(lz_local_app_session_t *session);

/* ---- feedback / app notifications ----
 * Minimal V0.95 service boundary: local apps can request user-visible feedback
 * without owning LED, buzzer, keyboard backlight, or future DND policy. */
typedef struct {
    uint32_t request_count;
    uint32_t last_ms;
    char     last_source[LZ_FEEDBACK_SOURCE_MAX];
    char     last_title[LZ_FEEDBACK_TITLE_MAX];
    char     last_body[LZ_FEEDBACK_BODY_MAX];
} lz_feedback_status_t;

bool lz_svc_feedback_notify(const char *source, const char *title, const char *body);
void lz_svc_feedback_status(lz_feedback_status_t *out);
int  lz_svc_feedback_diag(char *buf, int n);
int  lz_svc_feedback_selftest(char *buf, int n);
bool lz_svc_validate_app_catalog_json(const char *json, lz_app_catalog_report_t *out);
int  lz_svc_app_catalog_diag(char *buf, int n);
int  lz_svc_app_catalog_selftest(char *buf, int n);
bool lz_svc_ota_manifest_status(lz_ota_manifest_t *out);
int  lz_svc_ota_manifest_selftest(char *buf, int n);
bool lz_svc_security_status(lz_security_status_t *out);
bool lz_svc_security_set_pin(const char *pin, char *err, int err_cap);
bool lz_svc_security_check_pin(const char *pin);
bool lz_svc_security_clear_pin(const char *pin, char *err, int err_cap);
int  lz_svc_security_selftest(char *buf, int n);

/* ---- nodes ---- */
int  lz_svc_nodes(const lz_node_rt **out);              /* all heard nodes */
lz_node_rt *lz_svc_node_by_name(const char *name);
lz_node_rt *lz_svc_node_by_shortcode(const char *sc);  /* for channel long-press DM */
lz_node_rt *lz_svc_node_by_num(uint32_t num);          /* node for a DM thread */
void lz_svc_add_contact(lz_node_rt *n);
bool lz_node_messageable(const lz_node_rt *n);          /* people, not infrastructure */
int  lz_svc_node_count(lz_net_t net);
int  lz_svc_node_trace(const lz_node_rt *n, char *buf, int nbuf); /* contact detail trace */
const char *lz_svc_mt_role_label(int role);
const char *lz_svc_mt_hw_label(int hw);

/* ---- threads / messages ----
 * Storage is stable: thread/node pointers never move, so the UI can hold a
 * pointer to an open conversation. Display order (newest-first) is a separate
 * index, read via lz_svc_thread_at(). */
int  lz_svc_thread_count_all(void);
lz_thread_rt *lz_svc_top_unread(void);    /* most recent unread (lock-screen notification) */
int  lz_svc_unread_count(void);           /* number of conversations with unread (muted excluded) */
int  lz_svc_unread_total(void);           /* total unread messages across chats (muted excluded) */
void lz_svc_toggle_mute(lz_thread_rt *t); /* silence/unsilence a chat (no notify, no badge) */
lz_thread_rt *lz_svc_thread_at(int display_idx);        /* newest-first */
lz_thread_rt *lz_svc_thread_for_node(lz_node_rt *n);    /* find or create */
lz_thread_rt *lz_svc_channel_thread(void);              /* LongFast broadcast channel */
void lz_svc_open_thread(lz_thread_rt *t);               /* load tail, clear unread */
int  lz_svc_tail(const lz_msg_rt **out);                /* tail of the open thread */
bool lz_svc_send_text(lz_thread_rt *t, const char *text);
bool lz_svc_resend(int tail_idx);     /* retry a failed sent DM (long-press) */
const char *lz_svc_delivery_fail_label(uint8_t reason);
int  lz_svc_delivery_diag(char *buf, int n);  /* serial: pending DM ACK state */

/* MeshCore companion v0: line-oriented snapshot/send surface for USB smoke and
 * future external bridge work. This is not an external app compatibility claim. */
int  lz_svc_mc_companion_hello(char *buf, int n);
int  lz_svc_mc_companion_status(char *buf, int n);
int  lz_svc_mc_companion_nodes(char *buf, int n);
int  lz_svc_mc_companion_threads(char *buf, int n);
bool lz_svc_mc_companion_send_public(const char *text);
bool lz_svc_mc_companion_send_dm(const char *name, const char *text);
int  lz_svc_mc_companion_handle_line(const char *line, char *buf, int n, bool *exit_mode);
int  lz_svc_mc_companion_selftest(char *buf, int n);

/* ---- radio stats (airtime accounting) ---- */
typedef struct { uint32_t tx_count, rx_count; float util_pct; } lz_radio_stats_t;
void lz_svc_radio_stats(lz_radio_stats_t *out);

/* ---- identity ---- */
typedef struct { uint32_t num; char id[16]; char long_name[24]; char short_name[6]; } lz_identity_t;
const lz_identity_t *lz_svc_identity(void);
bool lz_svc_needs_onboarding(void);                       /* no saved identity yet */
void lz_svc_set_identity(const char *long_name, const char *short_name);
void lz_svc_set_node_num(uint32_t num);                   /* real node id (from MAC); call before init */

/* ---- persisted user settings ---- */
#define LZ_SETTINGS_SCHEMA_VERSION 4   /* v4 adds app_source; loads v1-v3 */

typedef struct {
    bool net_mt, net_mc;
    int  airtime;
    int  tx;
    bool gps;
    int  bright;
    int  timeout;
    int  kb_light;
    int  tz_idx;
    bool clock24;
    bool save;
    bool developer;
    int  app_source;
} lz_user_settings_t;

void lz_store_save_settings(const lz_user_settings_t *s);
bool lz_store_load_settings(lz_user_settings_t *s);
bool lz_store_settings_selftest(char *err, int err_cap);
bool lz_store_nodes_selftest(char *err, int err_cap);

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
void lz_backend_set_airtime(int mode);  /* choose the both-networks split */
void lz_backend_request_nodeinfo(uint32_t to);   /* ask a node for its NodeInfo (PKI key) */
bool lz_backend_mc_advert_now(bool flood);       /* send a MeshCore self-advert (flood/zero-hop) */
void lz_backend_mc_addr(char *buf, int n);       /* our MeshCore address, e.g. "MC-978bbe5f" */
/* companion bridge: USB serial speaks the Meshtastic app protocol when active */
bool lz_mtc_active(void);
void lz_mtc_set_active(bool on);
bool lz_mtc_any_active(void);
void lz_mtc_ble_begin(void);
void lz_mtc_ble_poll(void);
bool lz_mtc_ble_enabled(void);
bool lz_mtc_ble_connected(void);
void lz_mtc_ble_set_enabled(bool on);
int  lz_mtc_ble_status(char *buf, int n);
int  lz_mtc_ble_selftest(char *buf, int n);

/* MeshCore companion bridge: USB serial speaks the MC0 line protocol when active. */
bool lz_mcc_usb_active(void);
void lz_mcc_usb_set_active(bool on);
void lz_mcc_usb_poll(void);
int  lz_mcc_usb_status(char *buf, int n);
int  lz_mcc_usb_selftest(char *buf, int n);

/* called by backends on radio events */
void lz_core_on_text(uint32_t from, uint32_t to, const char *text, int hops_used, float snr);
void lz_core_on_nodeinfo(uint32_t from, const char *id, const char *long_name,
                         const char *short_name, int role, const char *hw, float snr);
void lz_core_on_heard(uint32_t from, float snr);        /* any packet from a node */
void lz_core_on_battery(uint32_t from, int batt);
void lz_core_on_position(uint32_t from, int32_t lat_i, int32_t lon_i,
                         bool has_alt, int32_t alt_m, uint32_t pos_time,
                         uint8_t precision_bits, float snr);
void lz_core_on_telemetry(uint32_t from, const lz_node_telemetry_t *telem, float snr);
void lz_core_on_ack(uint32_t request_id);
/* MeshCore: learn a node from a (signed, unencrypted) ADVERT.
 * adv_type: 1=Chat 2=Repeater 3=Room 4=Sensor */
void lz_core_on_mc_node(const uint8_t *pubkey, const char *name, int adv_type, float snr);
/* MeshCore Public channel + DM ingress -> unified inbox threads */
lz_thread_rt *lz_svc_mc_channel_thread(void);
void lz_core_on_mc_channel_text(const char *sender, const char *text, float snr);
void lz_core_on_mc_channel_self(const char *text);                       /* our Public send */
void lz_core_on_mc_dm(const uint8_t *pubkey, const char *name, const char *text, float snr);
void lz_core_on_mc_dm_self(const uint8_t *pubkey, const char *name, const char *text);  /* our DM send */
/* Meshtastic PKI: learn a node's X25519 public key (from its NodeInfo) */
void lz_core_on_pubkey(uint32_t from, const uint8_t *pub32);
bool lz_svc_node_pubkey(uint32_t num, uint8_t out32[32]);   /* true if known */

#ifdef __cplusplus
}
#endif

#endif
