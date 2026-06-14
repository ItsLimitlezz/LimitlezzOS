/**
 * sim_radio — virtual mesh + simulated radio for the desktop simulator.
 * See sim_radio.h for the design and the "real frames through the real decode
 * path" rationale.
 */
#include "sim_radio.h"
#include "../src/services/mtproto.h"
#include "../src/services/mcproto.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

extern uint32_t lz_tick_ms(void);

/* ======================================================================== *
 *  Virtual peer roster
 * ======================================================================== */

typedef struct {
    uint32_t    num;          /* Meshtastic node num (low-32). For MeshCore peers
                                 this mirrors mesh_core's pubkey-derived num. */
    const char *id;           /* "!7c3a91d0" or "MC-9d4f"                       */
    const char *name;
    const char *shortc;
    lz_net_t    net;
    int         mc_adv_type;  /* MeshCore only: MC_ADV_TYPE_* (1 Chat..4 Sensor) */
    uint8_t     mc_pub[32];   /* MeshCore only: a deterministic 32-byte pubkey   */
    bool        chatty;       /* auto-replies to our DMs                         */
    int         batt;
} sim_peer_t;

/* Our own identity num must match mesh_core's default g_id.num (0x7c3af1d0).
 * Inbound frames "from" a peer carry the peer's num; "to" us carries this.
 * NB: distinct from peer "Ava Reyes" (0x7c3a91d0) — note the f1 vs 91. */
#define SIM_SELF_NUM 0x7c3af1d0u

/* MeshCore peers get a deterministic 32-byte pubkey so the pubkey-derived node
 * num (mesh_core: pub[0..3] big-endian) and the "MC-xxxx" id are stable.       */
static void mc_fill_pub(uint8_t out[32], uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    for(int i = 0; i < 32; i++) out[i] = (uint8_t)(b0 + i * 7 + b1 * 3 + b2 + b3);
    out[0] = b0; out[1] = b1; out[2] = b2; out[3] = b3;     /* leading bytes = identity */
    if(out[0] == 0x00 || out[0] == 0xFF) out[0] = 0x42;     /* MeshCore rejects these */
}

/* roster (filled at init so MeshCore pubkeys can be derived) */
#define SIM_PEERS_MAX 12
static sim_peer_t g_peers[SIM_PEERS_MAX];
static int        g_peer_n;

static sim_peer_t *peer_by_num(uint32_t num)
{
    for(int i = 0; i < g_peer_n; i++) if(g_peers[i].num == num) return &g_peers[i];
    return NULL;
}
static sim_peer_t *peer_by_name(const char *name)
{
    for(int i = 0; i < g_peer_n; i++)
        if(strcmp(g_peers[i].name, name) == 0) return &g_peers[i];
    return NULL;
}
static sim_peer_t *first_chatty_mt(void)
{
    for(int i = 0; i < g_peer_n; i++)
        if(g_peers[i].net == LZ_NET_MT && g_peers[i].chatty) return &g_peers[i];
    return NULL;
}

/* num for a MeshCore peer matches mesh_core::lz_core_on_mc_node:
 * num = pub[0]<<24 | pub[1]<<16 | pub[2]<<8 | pub[3] */
static uint32_t mc_num(const uint8_t pub[32])
{
    return ((uint32_t)pub[0] << 24) | ((uint32_t)pub[1] << 16) |
           ((uint32_t)pub[2] << 8) | (uint32_t)pub[3];
}

static void roster_init(void)
{
    if(g_peer_n) return;
    sim_peer_t *p;

    /* --- Meshtastic peers (real on-air frames) --- */
    p = &g_peers[g_peer_n++];
    *p = (sim_peer_t){ .num = 0x7c3a91d0, .id = "!7c3a91d0", .name = "Ava Reyes",
                       .shortc = "AVA", .net = LZ_NET_MT, .chatty = true, .batt = 68 };
    p = &g_peers[g_peer_n++];
    *p = (sim_peer_t){ .num = 0x9f21de33, .id = "!9f21de33", .name = "Sam OK1QRP",
                       .shortc = "SAM", .net = LZ_NET_MT, .chatty = true, .batt = 45 };
    p = &g_peers[g_peer_n++];
    *p = (sim_peer_t){ .num = 0xa1b2c3d4, .id = "!a1b2c3d4", .name = "Base-01",
                       .shortc = "B01", .net = LZ_NET_MT, .chatty = false, .batt = 92 };
    p = &g_peers[g_peer_n++];
    *p = (sim_peer_t){ .num = 0x336699cc, .id = "!336699cc", .name = "Ridge Hiker",
                       .shortc = "RDH", .net = LZ_NET_MT, .chatty = true, .batt = 73 };

    /* --- MeshCore peers (real Ed25519-shaped pubkeys; ADVERTs are real) --- */
    static uint8_t pub_lim[32], pub_dmi[32], pub_rid[32], pub_room[32];
    mc_fill_pub(pub_lim,  0x9d, 0x4f, 0x21, 0x07);   /* "Limitlezz" -> MC-9d4f */
    mc_fill_pub(pub_dmi,  0x4f, 0x8e, 0x33, 0x11);   /* "Dmitri K"  -> MC-4f8e */
    mc_fill_pub(pub_rid,  0xb2, 0x10, 0x55, 0x22);   /* "Ridge Repeater" */
    mc_fill_pub(pub_room, 0xc0, 0x01, 0x77, 0x33);   /* "Base Camp" room */

    p = &g_peers[g_peer_n++];
    *p = (sim_peer_t){ .num = mc_num(pub_lim), .id = "MC-9d4f", .name = "Limitlezz",
                       .shortc = "9d4f", .net = LZ_NET_MC, .mc_adv_type = MC_ADV_TYPE_CHAT,
                       .chatty = true, .batt = 81 };
    memcpy(p->mc_pub, pub_lim, 32);
    p = &g_peers[g_peer_n++];
    *p = (sim_peer_t){ .num = mc_num(pub_dmi), .id = "MC-4f8e", .name = "Dmitri K",
                       .shortc = "4f8e", .net = LZ_NET_MC, .mc_adv_type = MC_ADV_TYPE_CHAT,
                       .chatty = true, .batt = 54 };
    memcpy(p->mc_pub, pub_dmi, 32);
    p = &g_peers[g_peer_n++];
    *p = (sim_peer_t){ .num = mc_num(pub_rid), .id = "MC-b210", .name = "Ridge Repeater",
                       .shortc = "b210", .net = LZ_NET_MC, .mc_adv_type = MC_ADV_TYPE_REPEATER,
                       .chatty = false, .batt = 78 };
    memcpy(p->mc_pub, pub_rid, 32);
    p = &g_peers[g_peer_n++];
    *p = (sim_peer_t){ .num = mc_num(pub_room), .id = "MC-c001", .name = "Base Camp",
                       .shortc = "c001", .net = LZ_NET_MC, .mc_adv_type = MC_ADV_TYPE_ROOM,
                       .chatty = false, .batt = -1 };
    memcpy(p->mc_pub, pub_room, 32);
}

/* ======================================================================== *
 *  Stats + TDM
 * ======================================================================== */

static lz_radio_stats_t g_stats = { 412, 1284, 3.4f };
static bool g_net_mt = true, g_net_mc = false;   /* mirrors lz_backend_set_networks */

bool sim_net_mt_tuned(void) { return g_net_mt; }
bool sim_net_mc_tuned(void) { return g_net_mc; }

/* TDM gate: a network's inbound traffic is only delivered while it's tuned in.
 * (Mirrors the real backend, where you can only receive on the profile the
 * single radio is currently tuned to.) */
static bool tuned(lz_net_t net)
{
    return net == LZ_NET_MC ? g_net_mc : g_net_mt;
}

void sim_radio_set_networks(bool mt, bool mc) { g_net_mt = mt; g_net_mc = mc; }
void sim_radio_stats(lz_radio_stats_t *out) { *out = g_stats; }

/* ======================================================================== *
 *  Self-echo + dedup awareness (sim side)
 *
 *  The firmware's dedup (seen_before) and self-echo filter live in the real
 *  backend, NOT in mesh_core — so when we decode a frame here we replicate the
 *  exact same two guards before dispatching to lz_core_on_*. The scenario test
 *  deliberately feeds a duplicate (same from+id) and a self-from frame to prove
 *  these guards work end-to-end.
 * ======================================================================== */

#define SEEN_N 64
static struct { uint32_t from, id; } g_seen[SEEN_N];
static int g_seen_w;

static bool seen_before(uint32_t from, uint32_t id)
{
    for(int i = 0; i < SEEN_N; i++)
        if(g_seen[i].from == from && g_seen[i].id == id && id) return true;
    g_seen[g_seen_w].from = from; g_seen[g_seen_w].id = id;
    g_seen_w = (g_seen_w + 1) % SEEN_N;
    return false;
}

/* ======================================================================== *
 *  Meshtastic: build a REAL frame and decode it back through the codec,
 *  exactly like backend_sx1262.cpp::handle_rx_mt. This exercises header
 *  framing, channel hash, AES-CTR, the Data protobuf, dedup and self-echo.
 * ======================================================================== */

static uint32_t g_air_id = 0xA1000000;   /* packet-id source for inbound air frames */
static uint32_t next_air_id(void) { return ++g_air_id; }

/* feed a fully-built MT frame through the same RX pipeline the radio uses */
static void mt_rx_frame(const uint8_t *buf, int len, float snr)
{
    mt_frame_t f;
    if(!mt_header_read(buf, len, &f)) return;
    if(seen_before(f.from, f.id)) return;          /* dup: drop (as on hardware) */
    if(f.from == SIM_SELF_NUM) return;             /* our own echo: drop */

    lz_core_on_heard(f.from, snr);
    g_stats.rx_count++;

    if(f.channel_hash != mt_channel_hash() || f.plen == 0) return;  /* not our PSK chan */
    uint8_t dec[251];
    int dl = f.plen;
    memcpy(dec, f.payload, dl);
    mt_crypt(dec, dl, f.from, f.id);               /* CTR decrypt */

    mt_data_t d;
    if(!mt_data_decode(dec, dl, &d)) return;
    int hops_used = (int)f.hop_start - (int)f.hop_limit;
    if(hops_used < 0) hops_used = 0;

    if(d.portnum == MT_PORT_TEXT && d.plen) {
        char text[LZ_TEXT_MAX];
        int tl = d.plen < (int)sizeof text - 1 ? d.plen : (int)sizeof text - 1;
        memcpy(text, d.payload, tl); text[tl] = 0;
        lz_core_on_text(f.from, f.to, text, hops_used, snr);
    } else if(d.portnum == MT_PORT_NODEINFO) {
        /* parse the User protobuf the same way parse_user() does */
        const uint8_t *b = d.payload; int n = d.plen, pos = 0;
        char id[16] = {0}, longn[24] = {0}, shortn[6] = {0};
        while(pos < n) {
            uint8_t tag = b[pos++]; int field = tag >> 3, wire = tag & 7;
            if(wire == 2) {
                uint64_t l = 0; int sh = 0;
                while(pos < n && sh < 64) { uint8_t x = b[pos++]; l |= (uint64_t)(x & 0x7F) << sh; if(!(x & 0x80)) break; sh += 7; }
                if((uint64_t)pos + l > (uint64_t)n) break;
                int li = (int)l;
                if(field == 1) snprintf(id, sizeof id, "%.*s", li < 15 ? li : 15, (const char*)(b+pos));
                else if(field == 2) snprintf(longn, sizeof longn, "%.*s", li < 23 ? li : 23, (const char*)(b+pos));
                else if(field == 3) snprintf(shortn, sizeof shortn, "%.*s", li < 5 ? li : 5, (const char*)(b+pos));
                pos += li;
            } else if(wire == 0) { while(pos < n && (b[pos++] & 0x80)) {} }
            else if(wire == 5) pos += 4; else if(wire == 1) pos += 8; else break;
        }
        lz_core_on_nodeinfo(f.from, id[0]?id:NULL, longn[0]?longn:NULL,
                            shortn[0]?shortn:NULL, 0, NULL, snr);
    } else if(d.portnum == MT_PORT_POSITION) {
        mt_position_t pos;
        if(mt_position_decode(d.payload, d.plen, &pos) && pos.has_lat && pos.has_lon)
            lz_core_on_position(f.from, pos.latitude_i, pos.longitude_i,
                                pos.has_alt, pos.altitude_m, pos.time,
                                pos.precision_bits, snr);
    } else if(d.portnum == MT_PORT_TELEMETRY) {
        mt_telemetry_t mt;
        if(mt_telemetry_decode(d.payload, d.plen, &mt)) {
            lz_node_telemetry_t tel; memset(&tel, 0, sizeof tel);
            tel.has_battery = mt.has_battery; tel.battery_pct = mt.battery_level;
            tel.has_voltage = mt.has_voltage; tel.voltage = mt.voltage;
            tel.has_uptime = mt.has_uptime;   tel.uptime_s = mt.uptime_s;
            tel.has_temperature = mt.has_temperature; tel.temperature_c = mt.temperature_c;
            tel.has_humidity = mt.has_humidity; tel.humidity_pct = mt.humidity_pct;
            tel.has_pressure = mt.has_pressure; tel.pressure_hpa = mt.pressure_hpa;
            lz_core_on_telemetry(f.from, &tel, snr);
        }
    } else if(d.portnum == MT_PORT_ROUTING && d.request_id) {
        lz_core_on_ack(d.request_id);
    }
}

/* forward decl: announce a peer's NodeInfo so it's known by name (as on the air,
 * where a node you talk to has broadcast its User record) */
static bool mt_emit_nodeinfo(const sim_peer_t *p, float snr);
static void mt_ensure_known(uint32_t from, float snr)
{
    sim_peer_t *p = peer_by_num(from);
    if(p && p->net == LZ_NET_MT) mt_emit_nodeinfo(p, snr);
}

/* build + deliver a Meshtastic text frame from `from` to `to` */
static bool mt_emit_text(uint32_t from, uint32_t to, const char *text, uint8_t hop_limit, float snr)
{
    if(!tuned(LZ_NET_MT)) return false;
    mt_ensure_known(from, snr);          /* so the peer shows its real name */
    uint8_t frame[256];
    int n = mt_build_text(frame, sizeof frame, from, to, next_air_id(), hop_limit, false, text);
    if(n <= 0) return false;
    mt_rx_frame(frame, n, snr);
    return true;
}

/* build a NodeInfo (User) frame and feed it in */
static bool mt_emit_nodeinfo(const sim_peer_t *p, float snr)
{
    if(!tuned(LZ_NET_MT)) return false;
    uint8_t user[96]; int u = 0;
    user[u++] = (1 << 3) | 2; user[u++] = (uint8_t)strlen(p->id);
    memcpy(user+u, p->id, strlen(p->id)); u += strlen(p->id);
    user[u++] = (2 << 3) | 2; user[u++] = (uint8_t)strlen(p->name);
    memcpy(user+u, p->name, strlen(p->name)); u += strlen(p->name);
    user[u++] = (3 << 3) | 2; user[u++] = (uint8_t)strlen(p->shortc);
    memcpy(user+u, p->shortc, strlen(p->shortc)); u += strlen(p->shortc);
    user[u++] = (5 << 3) | 0; user[u++] = 50;          /* hw_model = T_DECK */

    mt_data_t d; memset(&d, 0, sizeof d);
    d.portnum = MT_PORT_NODEINFO; memcpy(d.payload, user, u); d.plen = (uint8_t)u;
    uint8_t plain[160]; int pn = mt_data_encode(plain, sizeof plain, &d);
    if(pn < 0) return false;
    uint32_t id = next_air_id();
    mt_crypt(plain, pn, p->num, id);
    mt_frame_t f; memset(&f, 0, sizeof f);
    f.to = MT_BROADCAST; f.from = p->num; f.id = id; f.hop_limit = 3; f.hop_start = 3;
    f.channel_hash = mt_channel_hash();
    uint8_t frame[200]; mt_header_write(frame, &f); memcpy(frame+MT_HEADER_LEN, plain, pn);
    mt_rx_frame(frame, MT_HEADER_LEN + pn, snr);
    return true;
}

static bool mt_emit_position(const sim_peer_t *p, int32_t lat_i, int32_t lon_i, int32_t alt, float snr)
{
    if(!tuned(LZ_NET_MT)) return false;
    mt_emit_nodeinfo(p, snr);            /* node announces itself before sending data */
    uint8_t pos[32]; int n = 0;
    pos[n++] = 0x0D; memcpy(pos+n, &lat_i, 4); n += 4;       /* f1 latitude_i  (fixed32) */
    pos[n++] = 0x15; memcpy(pos+n, &lon_i, 4); n += 4;       /* f2 longitude_i (fixed32) */
    pos[n++] = 0x18; pos[n++] = (uint8_t)alt;                /* f3 altitude (varint, small) */
    pos[n++] = 0xB8; pos[n++] = 0x01; pos[n++] = 32;         /* f23 precision_bits = 32   */

    mt_data_t d; memset(&d, 0, sizeof d);
    d.portnum = MT_PORT_POSITION; memcpy(d.payload, pos, n); d.plen = (uint8_t)n;
    uint8_t plain[96]; int pn = mt_data_encode(plain, sizeof plain, &d);
    if(pn < 0) return false;
    uint32_t id = next_air_id(); mt_crypt(plain, pn, p->num, id);
    mt_frame_t f; memset(&f, 0, sizeof f);
    f.to = MT_BROADCAST; f.from = p->num; f.id = id; f.hop_limit = 3; f.hop_start = 3;
    f.channel_hash = mt_channel_hash();
    uint8_t frame[160]; mt_header_write(frame, &f); memcpy(frame+MT_HEADER_LEN, plain, pn);
    mt_rx_frame(frame, MT_HEADER_LEN + pn, snr);
    return true;
}

static bool mt_emit_telemetry(const sim_peer_t *p, int batt, float voltage, float temp_c, float snr)
{
    if(!tuned(LZ_NET_MT)) return false;
    mt_emit_nodeinfo(p, snr);            /* node announces itself before sending data */
    /* device_metrics { battery=1 varint, voltage=2 float, uptime=5 varint } */
    uint8_t dm[24]; int dn = 0;
    dm[dn++] = 0x08; dm[dn++] = (uint8_t)batt;
    dm[dn++] = 0x15; memcpy(dm+dn, &voltage, 4); dn += 4;
    dm[dn++] = 0x28; dm[dn++] = 0x90; dm[dn++] = 0x1C;       /* uptime ~3600 */
    /* environment_metrics { temperature=1 float } */
    uint8_t em[8]; int en = 0;
    em[en++] = 0x0D; memcpy(em+en, &temp_c, 4); en += 4;
    uint8_t tel[48]; int tn = 0;
    tel[tn++] = 0x12; tel[tn++] = (uint8_t)dn; memcpy(tel+tn, dm, dn); tn += dn;
    tel[tn++] = 0x1A; tel[tn++] = (uint8_t)en; memcpy(tel+tn, em, en); tn += en;

    mt_data_t d; memset(&d, 0, sizeof d);
    d.portnum = MT_PORT_TELEMETRY; memcpy(d.payload, tel, tn); d.plen = (uint8_t)tn;
    uint8_t plain[96]; int pn = mt_data_encode(plain, sizeof plain, &d);
    if(pn < 0) return false;
    uint32_t id = next_air_id(); mt_crypt(plain, pn, p->num, id);
    mt_frame_t f; memset(&f, 0, sizeof f);
    f.to = MT_BROADCAST; f.from = p->num; f.id = id; f.hop_limit = 2; f.hop_start = 2;
    f.channel_hash = mt_channel_hash();
    uint8_t frame[160]; mt_header_write(frame, &f); memcpy(frame+MT_HEADER_LEN, plain, pn);
    mt_rx_frame(frame, MT_HEADER_LEN + pn, snr);
    return true;
}

/* ROUTING ack for one of OUR sent DMs -> sender sees "delivered". Built and
 * decoded as a real frame so the request_id (fixed32) path is exercised. */
static void mt_emit_ack(uint32_t from_peer, uint32_t request_id, float snr)
{
    if(!tuned(LZ_NET_MT)) return;
    mt_data_t d; memset(&d, 0, sizeof d);
    d.portnum = MT_PORT_ROUTING; d.request_id = request_id; d.plen = 0;
    uint8_t plain[64]; int pn = mt_data_encode(plain, sizeof plain, &d);
    if(pn < 0) return;
    uint32_t id = next_air_id(); mt_crypt(plain, pn, from_peer, id);
    mt_frame_t f; memset(&f, 0, sizeof f);
    f.to = SIM_SELF_NUM; f.from = from_peer; f.id = id; f.hop_limit = 3; f.hop_start = 3;
    f.channel_hash = mt_channel_hash();
    uint8_t frame[96]; mt_header_write(frame, &f); memcpy(frame+MT_HEADER_LEN, plain, pn);
    mt_rx_frame(frame, MT_HEADER_LEN + pn, snr);
}

/* ======================================================================== *
 *  MeshCore: real ADVERT frames (parsed by mc_parse/mc_advert_decode) +
 *  Public/DM chat injected at the firmware hook boundary.
 * ======================================================================== */

/* build a real (unsigned-in-sim) ADVERT and decode it like handle_rx_mc.
 * MeshCore adverts are signed but UNENCRYPTED; mc_advert_decode does not verify
 * the signature, so a zero signature still decodes (the firmware's RX path does
 * the same — verification is a separate, hardware-only step). */
static bool mc_emit_advert(const sim_peer_t *p, float snr)
{
    if(!tuned(LZ_NET_MC)) return false;
    uint8_t app[40]; int al = 0;
    app[al++] = (uint8_t)(p->mc_adv_type | 0x80);           /* type + has-name */
    for(int i = 0; p->name[i] && al < 33; i++) app[al++] = (uint8_t)p->name[i];

    uint32_t ts = lz_svc_epoch();
    uint8_t frame[160]; int fl = 0;
    frame[fl++] = (MC_PAYLOAD_ADVERT << MC_TYPE_SHIFT) | MC_ROUTE_FLOOD;
    frame[fl++] = 0;                                         /* path_len = 0 */
    memcpy(frame+fl, p->mc_pub, 32); fl += 32;
    memcpy(frame+fl, &ts, 4); fl += 4;
    memset(frame+fl, 0, 64); fl += 64;                       /* signature (zeroed in sim) */
    memcpy(frame+fl, app, al); fl += al;

    mc_pkt_t pk;
    if(!mc_parse(frame, fl, &pk)) return false;
    if(pk.payload_type != MC_PAYLOAD_ADVERT) return false;
    mc_advert_t a;
    if(!mc_advert_decode(&pk, &a)) return false;
    lz_core_on_mc_node(a.pubkey, a.has_name ? a.name : NULL, a.adv_type, snr);
    g_stats.rx_count++;
    return true;
}

/* MeshCore Public-channel chat. The on-air group-text frame is encrypted with
 * the public channel key; this branch's codec only decodes ADVERTs on-air, so
 * we (a) ensure the speaker is known via a real ADVERT, then (b) deliver the
 * decoded chat line through lz_core_on_text on the LongFast/Public channel
 * thread — the same hook the MeshCore RX path will call once group decode
 * lands. The full inbox/channel/dedup path is still exercised. */
static bool mc_emit_public(sim_peer_t *speaker, const char *text, float snr)
{
    if(!tuned(LZ_NET_MC)) return false;
    if(speaker) mc_emit_advert(speaker, snr);               /* learn the node first */
    uint32_t from = speaker ? speaker->num : 0x00abcdefu;
    lz_core_on_text(from, LZ_BROADCAST, text, 1, snr);      /* -> channel thread */
    g_stats.rx_count++;
    return true;
}

/* MeshCore DM to us. ECDH/group crypto for MeshCore DMs is a later stage on
 * this branch, so the decoded plaintext is delivered through lz_core_on_text as
 * a directed message (to == us) — exercising the DM thread, unread bump and
 * reorder. The peer is learned via a real ADVERT first so it shows as a
 * MeshCore Chat node. */
static bool mc_emit_dm_to_us(sim_peer_t *speaker, const char *text, float snr)
{
    if(!tuned(LZ_NET_MC)) return false;
    if(speaker) mc_emit_advert(speaker, snr);
    uint32_t from = speaker ? speaker->num : mc_num((const uint8_t*)"\x9d\x4f\x21\x07");
    lz_core_on_text(from, SIM_SELF_NUM, text, 1, snr);      /* directed -> DM thread */
    g_stats.rx_count++;
    return true;
}

/* ======================================================================== *
 *  Deferred auto-reply queue (so a send can get a believable answer ~2s later)
 * ======================================================================== */

#define REPLY_N 8
static struct {
    bool      used;
    uint32_t  at_ms;
    uint32_t  from;       /* peer num */
    lz_net_t  net;
    char      text[80];
} g_reply[REPLY_N];

static const char *canned_reply(uint32_t from)
{
    switch(from) {
        case 0x7c3a91d0: return "copy that, moving now";
        case 0x9f21de33: return "73, catch you on the next pass";
        case 0x336699cc: return "loud and clear from the ridge";
        default:         return "received, thanks";
    }
}

static void arm_reply(uint32_t from, lz_net_t net, const char *text, uint32_t delay_ms)
{
    for(int i = 0; i < REPLY_N; i++) if(!g_reply[i].used) {
        g_reply[i].used = true;
        g_reply[i].at_ms = lz_tick_ms() + delay_ms;
        g_reply[i].from = from;
        g_reply[i].net = net;
        snprintf(g_reply[i].text, sizeof g_reply[i].text, "%s", text);
        return;
    }
}

static void pump_replies(void)
{
    uint32_t now = lz_tick_ms();
    for(int i = 0; i < REPLY_N; i++) {
        if(!g_reply[i].used || now < g_reply[i].at_ms) continue;
        g_reply[i].used = false;
        sim_peer_t *p = peer_by_num(g_reply[i].from);
        if(g_reply[i].net == LZ_NET_MC)
            mc_emit_dm_to_us(p, g_reply[i].text, -6.0f);
        else
            mt_emit_text(g_reply[i].from, SIM_SELF_NUM, g_reply[i].text, 2, -6.5f);
    }
}

/* ======================================================================== *
 *  Outbound: firmware -> air. Route, ACK and maybe auto-reply.
 * ======================================================================== */

bool sim_radio_send(lz_mt_packet_t *p)
{
    g_stats.tx_count++;
    if(!p) return false;

    /* DM (not broadcast) to a real peer: ack it, and let a chatty peer reply */
    if(p->to != LZ_BROADCAST && p->portnum == 1 /* TEXT */) {
        sim_peer_t *peer = peer_by_num(p->to);
        if(p->id) mt_emit_ack(p->to, p->id, -5.0f);   /* delivered (real ROUTING frame) */
        if(peer && peer->chatty) {
            char body[80];
            snprintf(body, sizeof body, "%s", canned_reply(p->to));
            arm_reply(p->to, peer->net, body, 2200);
        }
    }
    return true;
}

/* ======================================================================== *
 *  Periodic traffic pump: drip a rotating mix of inbound events.
 * ======================================================================== */

static uint32_t g_next_traffic;
static int      g_traffic_step;
static bool     g_auto_traffic = true;   /* live sim: ambient traffic on by default */

void sim_set_auto_traffic(bool on) { g_auto_traffic = on; }
bool sim_get_auto_traffic(void) { return g_auto_traffic; }

static void traffic_tick(void)
{
    uint32_t now = lz_tick_ms();
    if(now < g_next_traffic) return;
    g_next_traffic = now + 9000;          /* one ambient event every ~9s */
    if(!g_auto_traffic) return;

    int step = g_traffic_step++ % 8;
    switch(step) {
        case 0: sim_inject_mt_nodeinfo(); break;
        case 1: sim_inject_mt_channel_text("anyone copy on the ridge?"); break;
        case 2: sim_inject_mc_advert(); break;
        case 3: sim_inject_mt_position(); break;
        case 4: sim_inject_mc_public("Limitlezz", "public net check, all good here"); break;
        case 5: sim_inject_mt_telemetry(); break;
        case 6: sim_inject_mt_dm_to_us("Ridge Hiker", "you around? need a relay check"); break;
        case 7: sim_inject_mc_dm_from_limitlezz("hey, you on MeshCore now?"); break;
    }
}

/* ======================================================================== *
 *  Public injection API (keyboard controls + scenario)
 * ======================================================================== */

bool sim_inject_mc_dm_from_limitlezz(const char *text)
{
    sim_peer_t *lim = peer_by_name("Limitlezz");
    return mc_emit_dm_to_us(lim, text && text[0] ? text : "ping from Limitlezz", -6.0f);
}

bool sim_inject_mc_public(const char *who, const char *text)
{
    sim_peer_t *sp = who ? peer_by_name(who) : NULL;
    if(!sp) sp = peer_by_name("Limitlezz");
    return mc_emit_public(sp, text && text[0] ? text : "hello from the public channel", -7.5f);
}

bool sim_inject_mc_advert(void)
{
    /* rotate through the MeshCore peers so each advert refreshes a node */
    static int idx;
    int tried = 0;
    while(tried < g_peer_n) {
        sim_peer_t *p = &g_peers[idx % g_peer_n];
        idx++; tried++;
        if(p->net == LZ_NET_MC) return mc_emit_advert(p, p->batt > 60 ? 4.0f : -3.0f);
    }
    return false;
}

bool sim_inject_mt_channel_text(const char *text)
{
    sim_peer_t *p = peer_by_name("Ridge Hiker");
    if(!p) p = first_chatty_mt();
    if(!p) return false;
    return mt_emit_text(p->num, LZ_BROADCAST, text && text[0] ? text : "on my way up now", 2, -8.0f);
}

bool sim_inject_mt_dm_to_us(const char *peer_name, const char *text)
{
    sim_peer_t *p = peer_name ? peer_by_name(peer_name) : NULL;
    if(!p) p = first_chatty_mt();
    if(!p) return false;
    return mt_emit_text(p->num, SIM_SELF_NUM, text && text[0] ? text : "you copy?", 1, -7.0f);
}

bool sim_inject_mt_nodeinfo(void)
{
    sim_peer_t *p = peer_by_name("Sam OK1QRP");
    if(!p) p = first_chatty_mt();
    if(!p) return false;
    return mt_emit_nodeinfo(p, -9.0f);
}

bool sim_inject_mt_position(void)
{
    sim_peer_t *p = peer_by_name("Ava Reyes");
    if(!p) p = first_chatty_mt();
    if(!p) return false;
    /* ~Boulder CO, deg*1e7 */
    return mt_emit_position(p, 400150000, -1052700000, 1655, -6.0f);
}

bool sim_inject_mt_telemetry(void)
{
    sim_peer_t *p = peer_by_name("Base-01");
    if(!p) p = first_chatty_mt();
    if(!p) return false;
    return mt_emit_telemetry(p, p->batt, 4.05f, 21.5f, 5.0f);
}

void sim_inject_burst(void)
{
    sim_inject_mt_nodeinfo();
    sim_inject_mt_channel_text("burst: ridge net check");
    sim_inject_mt_position();
    sim_inject_mt_telemetry();
    sim_inject_mc_advert();
    sim_inject_mc_public("Limitlezz", "burst: public chatter");
    sim_inject_mc_dm_from_limitlezz("burst: DM to you");
}

/* ======================================================================== *
 *  Lifecycle
 * ======================================================================== */

bool sim_radio_mc_advert_now(bool flood) { (void)flood; return true; }  /* our advert: pretend sent */

void sim_radio_init(void)
{
    roster_init();
    g_next_traffic = lz_tick_ms() + 5000;   /* first ambient event ~5s after boot */
}

void sim_radio_loop(void)
{
    pump_replies();
    traffic_tick();
}

/* ======================================================================== *
 *  Deterministic scenario / regression harness  (program --simtest)
 *
 *  Runs a fixed sequence against a CLEAN, RAM-only inbox and asserts the
 *  resulting state: threads created, messages decoded, dedup working,
 *  self-echo dropped, delivery ACK landing, and the TDM gate. This is the
 *  CI value — it catches inbox/decode/dedup/filter regressions in the REAL
 *  firmware code (src/services/mesh_core.c et al.) without hardware.
 * ======================================================================== */

/* tail access from mesh_core for assertions */
extern int lz_svc_tail(const lz_msg_rt **out);

static int g_fails;
#define ST_CHECK(cond, msg) do { \
        if(cond) printf("ok  : %s\n", msg); \
        else { printf("FAIL: %s\n", msg); g_fails++; } } while(0)

/* count messages in a thread's persisted tail */
static int thread_tail_len(lz_thread_rt *t)
{
    if(!t) return 0;
    lz_svc_open_thread(t);
    const lz_msg_rt *tail;
    return lz_svc_tail(&tail);
}

int sim_scenario_run(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: last line printed = crash site */
    g_fails = 0;
    g_seen_w = 0; memset(g_seen, 0, sizeof g_seen);
    g_air_id = 0xA1000000;
    roster_init();

    /* clean slate: a wiped on-disk store (so tail counts are real and
     * persistence is exercised), no demo seed, both networks tuned in */
    system("rm -rf lzdata_simtest && mkdir -p lzdata_simtest");
    lz_svc_init("lzdata_simtest", false);
    lz_svc_set_time(1781274180);
    g_net_mt = true; g_net_mc = true;

    int base_threads = lz_svc_thread_count_all();   /* LongFast channel exists from init */
    printf("\n=== sim scenario ===\n");

    /* 1. Meshtastic DM from Ava -> a DM thread appears, message decoded */
    sim_inject_mt_dm_to_us("Ava Reyes", "heading up the summit trail now");
    lz_node_rt *ava = lz_svc_node_by_name("Ava Reyes");
    ST_CHECK(ava != NULL, "MT: Ava learned as a node");
    lz_thread_rt *ava_t = ava ? lz_svc_thread_for_node(ava) : NULL;
    ST_CHECK(ava_t != NULL, "MT: Ava DM thread created");
    ST_CHECK(ava_t && strstr(ava_t->last_text, "summit trail") != NULL,
             "MT: DM text decoded into thread");
    ST_CHECK(ava_t && ava_t->unread >= 1, "MT: DM bumped unread");

    /* 2. dedup: re-feeding the SAME built frame (same from+id) must NOT double */
    int len_before = thread_tail_len(ava_t);
    {
        uint8_t frame[256];
        int n = mt_build_text(frame, sizeof frame, ava->num, SIM_SELF_NUM,
                              0xDED00001, 1, false, "dup-guard probe");
        mt_rx_frame(frame, n, -7.0f);     /* first time: should append */
        int after_first = thread_tail_len(ava_t);
        mt_rx_frame(frame, n, -7.0f);     /* same from+id: dedup must drop */
        int after_dup = thread_tail_len(ava_t);
        ST_CHECK(after_first == len_before + 1, "DEDUP: first copy appended");
        ST_CHECK(after_dup == after_first, "DEDUP: duplicate frame dropped");
    }

    /* 3. self-echo: a frame "from us" must be dropped, not stored */
    {
        int before = lz_svc_thread_count_all();
        uint8_t frame[256];
        int n = mt_build_text(frame, sizeof frame, SIM_SELF_NUM, LZ_BROADCAST,
                              0x5E1F0001, 2, false, "this is our own echo");
        mt_rx_frame(frame, n, 0.0f);
        lz_thread_rt *ch = lz_svc_channel_thread();
        ST_CHECK(lz_svc_thread_count_all() == before, "SELF-ECHO: no new thread");
        ST_CHECK(ch && strstr(ch->last_text, "own echo") == NULL,
                 "SELF-ECHO: our own frame not stored on the channel");
    }

    /* 4. Meshtastic broadcast -> LongFast channel thread, sender prefixed */
    sim_inject_mt_channel_text("anyone on the ridge?");
    lz_thread_rt *ch = lz_svc_channel_thread();
    ST_CHECK(ch != NULL, "MT: LongFast channel thread exists");
    ST_CHECK(ch && strstr(ch->last_text, "ridge") != NULL, "MT: broadcast decoded to channel");

    /* 5. NodeInfo enriches a node's name/short code */
    sim_inject_mt_nodeinfo();
    lz_node_rt *sam = lz_svc_node_by_name("Sam OK1QRP");
    ST_CHECK(sam != NULL, "MT: NodeInfo created/named the node");
    ST_CHECK(sam && strcmp(sam->shortcode, "SAM") == 0, "MT: NodeInfo short code applied");

    /* 6. POSITION + TELEMETRY decode onto a node */
    sim_inject_mt_position();
    ST_CHECK(ava && (ava->pos_flags & LZ_NODE_POS_VALID), "MT: POSITION decoded onto node");
    sim_inject_mt_telemetry();
    lz_node_rt *base = lz_svc_node_by_name("Base-01");
    ST_CHECK(base && (base->telem_flags & LZ_NODE_TEL_VOLT), "MT: TELEMETRY voltage decoded");

    /* 7. MeshCore ADVERT -> a MeshCore Chat node named "Limitlezz" */
    sim_inject_mc_advert();   /* rotates; force Limitlezz explicitly too */
    mc_emit_advert(peer_by_name("Limitlezz"), 4.0f);
    lz_node_rt *lim = lz_svc_node_by_name("Limitlezz");
    ST_CHECK(lim != NULL, "MC: Limitlezz learned from ADVERT");
    ST_CHECK(lim && lim->net == LZ_NET_MC, "MC: Limitlezz is a MeshCore node");
    ST_CHECK(lim && strcmp(lim->role, "Chat") == 0, "MC: Limitlezz role = Chat");

    /* 8. MeshCore Public chat -> channel thread */
    int ch_before = thread_tail_len(ch);
    sim_inject_mc_public("Limitlezz", "public net check, all good here");
    ST_CHECK(thread_tail_len(ch) == ch_before + 1, "MC: public chat appended to channel");

    /* 9. MeshCore DM from Limitlezz -> its DM thread, decoded */
    sim_inject_mc_dm_from_limitlezz("hey, you on MeshCore now?");
    lz_thread_rt *lim_t = lim ? lz_svc_thread_for_node(lim) : NULL;
    ST_CHECK(lim_t != NULL, "MC: Limitlezz DM thread created");
    ST_CHECK(lim_t && strstr(lim_t->last_text, "MeshCore now") != NULL,
             "MC: DM text decoded into thread");

    /* 10. our send -> ACK loopback -> sent DM goes DELIVERED */
    {
        lz_svc_open_thread(ava_t);
        bool ok = lz_svc_send_text(ava_t, "on my way up now");
        ST_CHECK(ok, "SEND: send_text accepted");
        /* sim_radio_send already emitted the real ROUTING ack synchronously */
        const lz_msg_rt *tail; int tn = lz_svc_tail(&tail);
        bool delivered = false;
        for(int i = tn - 1; i >= 0; i--)
            if(tail[i].self && tail[i].status == LZ_MSG_DELIVERED) { delivered = true; break; }
        ST_CHECK(delivered, "SEND: sent DM marked DELIVERED via ROUTING ACK");
    }

    /* 11. TDM gate: with MeshCore tuned OUT, a MeshCore event is NOT delivered */
    {
        g_net_mc = false;
        int nodes_before; { const lz_node_rt *all; nodes_before = lz_svc_nodes(&all); }
        bool delivered = mc_emit_advert(peer_by_name("Dmitri K"), 2.0f);
        int nodes_after; { const lz_node_rt *all; nodes_after = lz_svc_nodes(&all); }
        ST_CHECK(!delivered, "TDM: MeshCore advert blocked while MC tuned out");
        ST_CHECK(nodes_after == nodes_before, "TDM: no MeshCore node added while tuned out");
        g_net_mc = true;
        ST_CHECK(mc_emit_advert(peer_by_name("Dmitri K"), 2.0f),
                 "TDM: MeshCore advert delivered once tuned back in");
    }

    /* sanity: we created real conversations */
    ST_CHECK(lz_svc_thread_count_all() > base_threads, "INBOX: new threads were created");

    printf(g_fails ? "\nSIM SCENARIO: %d FAILURE(S)\n" : "\nSIM SCENARIO: all passed\n", g_fails);
    return g_fails;
}
