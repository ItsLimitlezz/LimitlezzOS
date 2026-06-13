/**
 * Real Meshtastic radio backend for the LilyGO T-Deck (SX1262 via RadioLib).
 *
 * Implements the lz_backend_* contract from services/mesh.h: drives the
 * SX1262 on the default LongFast channel, decodes inbound Meshtastic packets
 * into lz_core_on_* events, and transmits text with CSMA + managed-flood
 * rebroadcast. All RF/protocol constants are sourced from the Meshtastic
 * firmware (master) — see mtproto.c and the comments below.
 *
 * Constants verified against meshtastic/firmware master (2026-06):
 *   pins  variants/esp32s3/t-deck/variant.h
 *   modem MeshRadio.h modemPresetToParams() LONG_FAST + US915 RDEF
 *   wire  RadioInterface.h PacketHeader, CryptoEngine.cpp, Channels.cpp
 */
#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <RadioLib.h>
#include <Ed25519.h>            /* rweather/Crypto — the lib MeshCore verifies adverts with */
#include "services/mesh.h"
#include "services/mtproto.h"
#include "services/mcproto.h"
#include "mtpki.h"

/* MeshCore identity persistence (store.c) */
extern "C" void lz_store_save_mc_key(const uint8_t *prv32);
extern "C" bool lz_store_load_mc_key(uint8_t *prv32);

/* Meshtastic companion bridge (mt_companion.cpp) */
extern "C" void lz_mtc_forward(uint32_t from, uint32_t to, uint32_t id, uint32_t chan,
                               uint8_t portnum, const uint8_t *payload, int plen,
                               float snr, uint32_t rxtime, int hop_limit);

/* ---- T-Deck SX1262 pin map (variant.h) ---- */
#define PIN_LORA_NSS    9
#define PIN_LORA_DIO1   45
#define PIN_LORA_BUSY   13      /* labeled LORA_DIO2 in variant.h, wired to BUSY */
#define PIN_LORA_RESET  17

/* ---- LONG_FAST / US915 (MeshRadio.h, RadioInterface.cpp) ---- */
#define RF_FREQ_MHZ     906.875f    /* US default LongFast slot 19 */
#define RF_BW_KHZ       250.0f
#define RF_SF           11
#define RF_CR           5           /* 4/5 */
#define RF_SYNCWORD     0x2B        /* RadioLib expands to 0x2B2B on SX126x */
#define RF_PREAMBLE     16
#define RF_TX_DBM       22          /* SX1262 PA max; region cap is 30 */
#define RF_TCXO_V       1.8f

/* shares the global SPI bus that main_tdeck sets up (SCK40/MISO38/MOSI41) */
static SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RESET, PIN_LORA_BUSY);

static volatile bool g_rx_flag;
static bool          g_ok;
static int           g_begin_state = 0x7FFF;   /* RadioLib begin() return code */
static lz_radio_stats_t g_stats;
static uint32_t      g_airtime_ms;        /* cumulative TX airtime */
static uint32_t      g_boot_ms;

/* ================= time-division multiplexing of the one radio =================
 * Two RF profiles share the SX1262. When both networks are on we round-robin:
 * tune to one profile, listen for a slot, retune to the other, repeat. When
 * only one network is on, that profile gets the radio 100% (no switching).
 * MeshCore params verified against meshcore-dev/MeshCore (US: 910.525 / 62.5 /
 * SF7 / CR4-5 / sync PRIVATE / preamble 32). */
typedef struct { float freq, bw; uint8_t sf, cr; uint16_t preamble; uint8_t sync; } rf_prof_t;
enum { PROF_MT = 0, PROF_MC = 1 };
static rf_prof_t g_prof[2] = {
    { RF_FREQ_MHZ, RF_BW_KHZ, RF_SF, RF_CR, RF_PREAMBLE, RF_SYNCWORD },          /* Meshtastic LongFast */
    { 910.525f,    62.5f,     7,     5,     32,          RADIOLIB_SX126X_SYNC_WORD_PRIVATE }, /* MeshCore US */
};

static bool     g_net_mt = true;          /* Meshtastic enabled */
static bool     g_net_mc = false;         /* MeshCore enabled   */
static int      g_active = PROF_MT;       /* profile the radio is tuned to now */
static uint32_t g_slot_until;             /* millis() when the current dwell ends (0 = no switching) */
static const uint32_t SLOT_MS = 500;      /* dwell per profile when both are on */
static uint32_t g_rx_mt, g_rx_mc;         /* per-network packet counts */
static uint32_t g_switches;               /* TDM profile switches */

/* retune the radio to a profile and resume RX (the "switch modes" step) */
static void apply_profile(int which)
{
    if(!g_ok) return;
    const rf_prof_t *p = &g_prof[which];
    radio.standby();
    radio.setFrequency(p->freq);
    radio.setBandwidth(p->bw);
    radio.setSpreadingFactor(p->sf);
    radio.setCodingRate(p->cr);
    radio.setSyncWord(p->sync);
    radio.setPreambleLength(p->preamble);
    radio.startReceive();
    g_active = which;
}

/* dedup ring of recently-seen (from,id) so we don't reprocess/rebroadcast */
#define SEEN_N 24
static struct { uint32_t from, id; } g_seen[SEEN_N];
static int g_seen_head;

static bool seen_before(uint32_t from, uint32_t id)
{
    for(int i = 0; i < SEEN_N; i++)
        if(g_seen[i].from == from && g_seen[i].id == id) return true;
    g_seen[g_seen_head].from = from;
    g_seen[g_seen_head].id = id;
    g_seen_head = (g_seen_head + 1) % SEEN_N;
    return false;
}

#if defined(ESP32)
ICACHE_RAM_ATTR
#endif
static void on_dio1(void) { g_rx_flag = true; }

/* approximate LoRa airtime (ms) for managed-flood/util accounting */
static uint32_t airtime_ms(int payload_len)
{
    /* symbol time = 2^SF / BW */
    float ts = (float)(1UL << RF_SF) / (RF_BW_KHZ * 1000.0f);
    float tPreamble = (RF_PREAMBLE + 4.25f) * ts;
    int de = (RF_SF >= 11) ? 1 : 0;     /* low-data-rate optimize on for SF11 */
    float num = 8.0f * payload_len - 4.0f * RF_SF + 28.0f + 16.0f;
    float den = 4.0f * (RF_SF - 2 * de);
    int nPayload = 8 + (int)(num / den + 0.999f) * RF_CR;   /* ceil * (CR=4/5 -> 5) */
    if(nPayload < 8) nPayload = 8;
    float tPayload = nPayload * ts;
    return (uint32_t)((tPreamble + tPayload) * 1000.0f);
}

/* ---- minimal Meshtastic User decode (NodeInfo payload) ---- */
static void parse_user(const uint8_t *b, int len, uint32_t from, float snr)
{
    char id[16] = {0}, longn[24] = {0}, shortn[6] = {0};
    int hw = 0, pos = 0;
    const uint8_t *pubkey = NULL;   /* User.public_key (field 8), 32 bytes */
    while(pos < len) {
        if(pos >= len) break;
        uint8_t tag = b[pos++];
        int field = tag >> 3, wire = tag & 7;
        if(wire == 2) {
            /* length is a protobuf varint, not a single byte (stock nodes can
             * send fields >= 128 bytes; decode properly for interop) */
            uint64_t l = 0; int sh = 0;
            while(pos < len && sh < 64) { uint8_t x = b[pos++]; l |= (uint64_t)(x & 0x7F) << sh; if(!(x & 0x80)) break; sh += 7; }
            if((uint64_t)pos + l > (uint64_t)len) break;
            const char *s = (const char *)(b + pos);
            int li = (int)l;
            if(field == 1) snprintf(id, sizeof id, "%.*s", li < 15 ? li : 15, s);
            else if(field == 2) snprintf(longn, sizeof longn, "%.*s", li < 23 ? li : 23, s);
            else if(field == 3) snprintf(shortn, sizeof shortn, "%.*s", li < 5 ? li : 5, s);
            else if(field == 8 && li == 32) pubkey = b + pos;   /* X25519 public key */
            pos += li;
        } else if(wire == 0) {
            uint64_t v = 0; int sh = 0;
            while(pos < len) { uint8_t x = b[pos++]; v |= (uint64_t)(x & 0x7F) << sh; if(!(x & 0x80)) break; sh += 7; }
            if(field == 5) hw = (int)v;
        } else if(wire == 5) pos += 4;
        else if(wire == 1) pos += 8;
        else break;
    }
    lz_core_on_nodeinfo(from, id[0] ? id : NULL, longn[0] ? longn : NULL,
                        shortn[0] ? shortn : NULL, 0, NULL, snr);
    if(pubkey) lz_core_on_pubkey(from, pubkey);   /* remember key for PKI DMs */
    (void)hw;
}

/* ---- transmit one assembled frame, with a light CSMA gate ---- */
static bool tx_frame(const uint8_t *frame, int len)
{
    /* CSMA: brief listen-before-talk with random backoff */
    for(int attempt = 0; attempt < 6; attempt++) {
        float rssi = radio.getRSSI(false);
        if(rssi < -95.0f) break;            /* channel looks clear */
        delay(20 + (esp_random() & 0x3F));
    }
    int st = radio.transmit((uint8_t *)frame, len);
    radio.startReceive();                   /* back to RX immediately */
    if(st == RADIOLIB_ERR_NONE) {
        g_stats.tx_count++;
        g_airtime_ms += airtime_ms(len);
        return true;
    }
    return false;
}

/* rebroadcast someone else's packet (managed flood): decrement hop_limit */
static void rebroadcast(uint8_t *frame, int len, mt_frame_t *f)
{
    if(f->hop_limit == 0) return;
    f->hop_limit--;
    frame[12] = (f->hop_limit & 0x07) | (f->flags & 0xF8);   /* rewrite flags low bits */
    f->relay_node = (uint8_t)(lz_svc_identity()->num & 0xFF);
    frame[15] = f->relay_node;
    delay(30 + (esp_random() & 0x7F));      /* contention window */
    tx_frame(frame, len);
}

/* ---- Meshtastic RX (active profile == PROF_MT) ---- */
static void handle_rx_mt(void)
{
    uint8_t buf[256];
    int len = radio.getPacketLength();
    if(len <= 0 || len > (int)sizeof buf) { radio.startReceive(); return; }
    int st = radio.readData(buf, len);
    radio.startReceive();
    if(st != RADIOLIB_ERR_NONE) return;

    float snr = radio.getSNR();
    g_stats.rx_count++;
    g_rx_mt++;

    mt_frame_t f;
    if(!mt_header_read(buf, len, &f)) return;
    if(seen_before(f.from, f.id)) return;        /* dup: ignore + don't rebroadcast */

    uint32_t me = lz_svc_identity()->num;
    if(f.from == me) return;                     /* our own echo */

    lz_core_on_heard(f.from, snr);

    /* recover the plaintext Data: PSK channel crypt (our channel hash), or a
     * PKI DM addressed to us (channel byte 0, decrypt with the sender's key) */
    uint8_t dec[251];
    int dl = -1;
    if(f.channel_hash == mt_channel_hash() && f.plen > 0) {
        dl = f.plen;
        memcpy(dec, f.payload, dl);
        mt_crypt(dec, dl, f.from, f.id);             /* CTR: decrypt == encrypt */
    } else if(f.channel_hash == 0 && f.to == me && f.plen > 12) {
        uint8_t sender_pub[32];
        if(lz_mtpki_ready() && lz_svc_node_pubkey(f.from, sender_pub))
            dl = lz_mtpki_decrypt(sender_pub, f.from, f.id, f.payload, f.plen, dec, sizeof dec);
    }
    if(dl > 0) {
        mt_data_t d;
        if(mt_data_decode(dec, dl, &d)) {
            int hops_used = (int)f.hop_start - (int)f.hop_limit;
            if(hops_used < 0) hops_used = 0;
            /* companion mode: hand the whole decoded packet to the phone app */
            if(lz_mtc_active())
                lz_mtc_forward(f.from, f.to, f.id, 0, d.portnum, d.payload, d.plen,
                               snr, lz_svc_epoch(), f.hop_limit);
            if(d.portnum == MT_PORT_TEXT && d.plen) {
                char text[LZ_TEXT_MAX];
                int tl = d.plen < (int)sizeof text - 1 ? d.plen : (int)sizeof text - 1;
                memcpy(text, d.payload, tl);
                text[tl] = 0;
                lz_core_on_text(f.from, f.to, text, hops_used, snr);
            } else if(d.portnum == MT_PORT_NODEINFO) {
                parse_user(d.payload, d.plen, f.from, snr);
            }
        }
    }

    /* managed flood: rebroadcast packets not addressed to us */
    if(f.to != me) rebroadcast(buf, len, &f);
}

/* ---- MeshCore RX (active profile == PROF_MC) ----
 * We decode ADVERTs (signed, unencrypted) to learn nodes by name + role.
 * Other MeshCore types are encrypted (need ECDH/channel keys we don't hold),
 * so they're counted but not opened. */
static void handle_rx_mc(void)
{
    uint8_t buf[256];
    int len = radio.getPacketLength();
    if(len <= 0 || len > (int)sizeof buf) { radio.startReceive(); return; }
    int st = radio.readData(buf, len);
    radio.startReceive();
    if(st != RADIOLIB_ERR_NONE) return;

    float snr = radio.getSNR();
    g_stats.rx_count++;
    g_rx_mc++;

    mc_pkt_t p;
    if(!mc_parse(buf, len, &p)) return;
    if(p.payload_type == MC_PAYLOAD_ADVERT) {
        mc_advert_t a;
        if(mc_advert_decode(&p, &a))
            lz_core_on_mc_node(a.pubkey, a.has_name ? a.name : NULL, a.adv_type, snr);
    }
}

/* ---- NodeInfo (our User) periodic broadcast so peers learn our name ---- */
static uint32_t g_last_nodeinfo;

static void broadcast_nodeinfo(void)
{
    const lz_identity_t *id = lz_svc_identity();
    uint8_t user[112];
    int n = 0;
    /* User{ id=1 str, long_name=2 str, short_name=3 str, public_key=8 bytes } */
    user[n++] = (1 << 3) | 2; user[n++] = (uint8_t)strlen(id->id);
    memcpy(user + n, id->id, strlen(id->id)); n += strlen(id->id);
    user[n++] = (2 << 3) | 2; user[n++] = (uint8_t)strlen(id->long_name);
    memcpy(user + n, id->long_name, strlen(id->long_name)); n += strlen(id->long_name);
    user[n++] = (3 << 3) | 2; user[n++] = (uint8_t)strlen(id->short_name);
    memcpy(user + n, id->short_name, strlen(id->short_name)); n += strlen(id->short_name);
    if(lz_mtpki_ready()) {                       /* advertise our X25519 key for PKI DMs */
        user[n++] = (8 << 3) | 2; user[n++] = 32;
        memcpy(user + n, lz_mtpki_pubkey(), 32); n += 32;
    }

    mt_data_t d;
    memset(&d, 0, sizeof d);
    d.portnum = MT_PORT_NODEINFO;
    memcpy(d.payload, user, n);
    d.plen = (uint8_t)n;

    uint8_t plain[128];
    int pn = mt_data_encode(plain, sizeof plain, &d);
    if(pn < 0) return;
    uint32_t pid = (uint32_t)(esp_random() | 1);
    mt_crypt(plain, pn, id->num, pid);

    mt_frame_t f;
    memset(&f, 0, sizeof f);
    f.to = MT_BROADCAST; f.from = id->num; f.id = pid;
    f.hop_limit = 3; f.hop_start = 3;
    f.channel_hash = mt_channel_hash();

    uint8_t frame[160];
    mt_header_write(frame, &f);
    memcpy(frame + MT_HEADER_LEN, plain, pn);
    tx_frame(frame, MT_HEADER_LEN + pn);
}

/* ---- MeshCore self-advert: a signed ADVERT so other nodes discover us ----
 * Identity is a standard Ed25519 keypair (rweather/Crypto). The 32-byte private
 * seed is persisted; the 32-byte public key is our MeshCore address and goes in
 * the advert. A real MeshCore node verifies the advert with Ed25519::verify over
 * pubkey||timestamp||app_data — the same library + algorithm, so it accepts us. */
#define MAX_ADVERT_DATA_SIZE_LZ 32     /* MeshCore MAX_ADVERT_DATA_SIZE */
static uint8_t  g_mc_prv[32], g_mc_pub[32];
static bool     g_mc_id_ok;
static uint32_t g_last_mc_advert;

static void mc_identity_init(void)
{
    if(!lz_store_load_mc_key(g_mc_prv)) {
        do {
            for(int i = 0; i < 32; i++) g_mc_prv[i] = (uint8_t)(esp_random() & 0xFF);
            Ed25519::derivePublicKey(g_mc_pub, g_mc_prv);
        } while(g_mc_pub[0] == 0x00 || g_mc_pub[0] == 0xFF);  /* MeshCore rejects these */
        lz_store_save_mc_key(g_mc_prv);
        Serial.printf("[ok] MeshCore identity generated: %02x%02x%02x%02x...\n",
                      g_mc_pub[0], g_mc_pub[1], g_mc_pub[2], g_mc_pub[3]);
    } else {
        Ed25519::derivePublicKey(g_mc_pub, g_mc_prv);
    }
    g_mc_id_ok = true;
}

/* assemble an ADVERT into `frame`. flood=true -> ROUTE_TYPE_FLOOD (propagates
 * mesh-wide via repeaters); flood=false -> ROUTE_TYPE_DIRECT + path_len 0 (zero
 * hop, neighbors only). Returns length or -1. */
static int build_mc_advert(uint8_t *frame, int cap, bool flood)
{
    if(!g_mc_id_ok) return -1;
    const lz_identity_t *id = lz_svc_identity();

    /* app_data: flags (Chat + has-name) then the node name */
    uint8_t app[MAX_ADVERT_DATA_SIZE_LZ]; int al = 0;
    app[al++] = MC_ADV_TYPE_CHAT | 0x80;          /* 0x81: Chat node, name present */
    for(int i = 0; id->long_name[i] && al < MAX_ADVERT_DATA_SIZE_LZ; i++)
        app[al++] = (uint8_t)id->long_name[i];

    uint32_t ts = lz_svc_epoch();

    /* signed message = pubkey || timestamp || app_data */
    uint8_t msg[32 + 4 + MAX_ADVERT_DATA_SIZE_LZ]; int ml = 0;
    memcpy(msg + ml, g_mc_pub, 32); ml += 32;
    memcpy(msg + ml, &ts, 4);       ml += 4;
    memcpy(msg + ml, app, al);      ml += al;
    uint8_t sig[64];
    Ed25519::sign(sig, g_mc_prv, g_mc_pub, msg, ml);

    /* frame: header(route|ADVERT) path_len=0  payload[pubkey ts sig app] */
    int fl = 0;
    uint8_t route = flood ? MC_ROUTE_FLOOD : MC_ROUTE_DIRECT;  /* 0x01 flood / 0x02 zero-hop */
    frame[fl++] = (MC_PAYLOAD_ADVERT << MC_TYPE_SHIFT) | route;
    frame[fl++] = 0;                              /* path_len = 0 */
    if(fl + 32 + 4 + 64 + al > cap) return -1;
    memcpy(frame + fl, g_mc_pub, 32); fl += 32;
    memcpy(frame + fl, &ts, 4);       fl += 4;
    memcpy(frame + fl, sig, 64);      fl += 64;
    memcpy(frame + fl, app, al);      fl += al;
    return fl;
}

/* transmit one advert (must be tuned to the MeshCore profile) */
static bool broadcast_mc_advert(bool flood)
{
    if(!g_mc_id_ok || g_active != PROF_MC) return false;
    uint8_t frame[160];
    int n = build_mc_advert(frame, sizeof frame, flood);
    if(n <= 0) return false;
    return tx_frame(frame, n);
}

/* ================= backend contract ================= */

void lz_backend_init(void)
{
    g_boot_ms = millis();
    /* hard-reset the SX1262 before begin (working examples do this; without
     * it begin() can spin on BUSY up to a multi-second SPI timeout) */
    pinMode(PIN_LORA_RESET, OUTPUT);
    digitalWrite(PIN_LORA_RESET, LOW);  delay(10);
    digitalWrite(PIN_LORA_RESET, HIGH); delay(10);
    int st = radio.begin(RF_FREQ_MHZ, RF_BW_KHZ, RF_SF, RF_CR,
                         RF_SYNCWORD, RF_TX_DBM, RF_PREAMBLE, RF_TCXO_V);
    g_begin_state = st;
    if(st != RADIOLIB_ERR_NONE) { g_ok = false; return; }
    radio.setDio2AsRfSwitch(true);
    radio.setCRC(true);                  /* Meshtastic uses hardware CRC */
    radio.setDio1Action(on_dio1);
    radio.startReceive();
    g_ok = true;
    mc_identity_init();                  /* prepare our MeshCore Ed25519 identity */
    lz_mtpki_init();                     /* prepare our Meshtastic X25519 (PKI DM) identity */
}

bool lz_backend_ok(void) { return g_ok; }
int  lz_backend_begin_state(void) { return g_begin_state; }

void lz_backend_set_tx_power(int dbm)
{
    if(!g_ok) return;
    if(dbm < -9) dbm = -9; if(dbm > 22) dbm = 22;   /* SX1262 range */
    radio.setOutputPower(dbm);
}

void lz_backend_loop(void)
{
    if(!g_ok) return;

    /* TDM: when both networks are on, hand the radio to the other profile once
     * the current dwell expires (listen -> retune -> listen, round-robin) */
    uint32_t now = millis();
    if(g_net_mt && g_net_mc && g_slot_until && now >= g_slot_until) {
        apply_profile(g_active == PROF_MT ? PROF_MC : PROF_MT);
        g_switches++;
        g_slot_until = now + SLOT_MS;
    }

    /* a received packet belongs to whichever profile we're tuned to right now */
    if(g_rx_flag) {
        g_rx_flag = false;
        if(g_active == PROF_MT) handle_rx_mt();
        else                    handle_rx_mc();
    }

    /* announce our Meshtastic node info shortly after boot, then every ~5 min —
     * only while actually listening on Meshtastic (tx must use the MT profile) */
    if(g_net_mt && g_active == PROF_MT) {
        if(g_last_nodeinfo == 0 ? (now - g_boot_ms > 4000)
                                : (now - g_last_nodeinfo > 300000)) {
            g_last_nodeinfo = now;
            broadcast_nodeinfo();
        }
    }

    /* advertise ourselves on MeshCore so other nodes discover us — only while
     * tuned to the MeshCore profile; first shortly after boot, then ~every 4 min */
    if(g_net_mc && g_active == PROF_MC) {
        if(g_last_mc_advert == 0 ? (now - g_boot_ms > 6000)
                                 : (now - g_last_mc_advert > 240000)) {
            g_last_mc_advert = now;
            broadcast_mc_advert(true);   /* periodic auto-advert floods for discovery */
        }
    }
}

bool lz_backend_send(lz_mt_packet_t *p)
{
    if(!g_ok || !g_net_mt) return false;        /* Meshtastic TX needs the MT network on */
    /* a Meshtastic frame must go out on the MT profile; if a TDM slot has us on
     * MeshCore right now, retune to MT for the transmit (the scheduler resumes
     * round-robin on its next tick) */
    if(g_active != PROF_MT) { apply_profile(PROF_MT); g_slot_until = millis() + SLOT_MS; }

    /* wrap the service payload in a Data protobuf, encrypt, frame, transmit */
    mt_data_t d;
    memset(&d, 0, sizeof d);
    d.portnum = p->portnum;
    int pl = p->plen < (int)sizeof d.payload ? p->plen : (int)sizeof d.payload;
    memcpy(d.payload, p->payload, pl);
    d.plen = (uint8_t)pl;
    d.want_response = p->want_ack;

    uint8_t plain[251];
    int pn = mt_data_encode(plain, sizeof plain, &d);
    if(pn < 0) return false;

    mt_frame_t f;
    memset(&f, 0, sizeof f);
    f.to = p->to; f.from = p->from; f.id = p->id;
    f.hop_limit = p->hop_limit; f.hop_start = p->hop_start ? p->hop_start : p->hop_limit;
    f.want_ack = p->want_ack;

    uint8_t frame[256];
    int payload_len;

    /* DM to a node whose public key we know -> PKI-encrypt (channel byte 0).
     * Otherwise (broadcast/channel, or no key yet) -> legacy PSK channel crypt. */
    uint8_t peer_pub[32];
    bool pki = p->to != MT_BROADCAST && p->to != p->from &&
               lz_mtpki_ready() && lz_svc_node_pubkey(p->to, peer_pub);
    if(pki) {
        uint8_t blob[251];
        int bl = lz_mtpki_encrypt(peer_pub, p->from, p->id, plain, pn, blob, sizeof blob);
        if(bl < 0) return false;
        f.channel_hash = 0;                              /* PKI DM sentinel */
        if(MT_HEADER_LEN + bl > (int)sizeof frame) return false;
        mt_header_write(frame, &f);
        memcpy(frame + MT_HEADER_LEN, blob, bl);
        payload_len = bl;
    } else {
        mt_crypt(plain, pn, p->from, p->id);             /* AES-CTR with the channel PSK */
        f.channel_hash = mt_channel_hash();
        if(MT_HEADER_LEN + pn > (int)sizeof frame) return false;
        mt_header_write(frame, &f);
        memcpy(frame + MT_HEADER_LEN, plain, pn);
        payload_len = pn;
    }
    return tx_frame(frame, MT_HEADER_LEN + payload_len);
}

void lz_backend_stats(lz_radio_stats_t *out)
{
    uint32_t up = millis() - g_boot_ms;
    g_stats.util_pct = up ? (float)g_airtime_ms / (float)up * 100.0f : 0.0f;
    *out = g_stats;
}

/* ================= TDM control (called from the UI / serial console) ================= */

/* Enable/disable each network. Drives the schedule: both -> round-robin split,
 * one -> that profile 100% (no switching), neither -> radio idle. */
extern "C" void lz_backend_set_networks(bool mt, bool mc)
{
    g_net_mt = mt;
    g_net_mc = mc;
    if(!g_ok) return;
    if(mt && mc) {                       /* share the radio, start on Meshtastic */
        apply_profile(PROF_MT);
        g_slot_until = millis() + SLOT_MS;
    } else if(mt) {
        apply_profile(PROF_MT); g_slot_until = 0;
    } else if(mc) {
        apply_profile(PROF_MC); g_slot_until = 0;
    } else {
        radio.standby(); g_slot_until = 0; /* neither: stop listening */
    }
}

/* live-tune the MeshCore RF profile (serial `rf mc <freq> <bw> <sf> <cr> [sync]`).
 * sync < 0 leaves the sync word unchanged. */
extern "C" void lz_backend_mc_tune(float freq, float bw, int sf, int cr, int sync)
{
    if(freq > 100.0f) g_prof[PROF_MC].freq = freq;
    if(bw > 0.0f)     g_prof[PROF_MC].bw = bw;
    if(sf >= 5 && sf <= 12) { g_prof[PROF_MC].sf = sf; g_prof[PROF_MC].preamble = sf <= 8 ? 32 : 16; }
    if(cr >= 5 && cr <= 8)  g_prof[PROF_MC].cr = cr;
    if(sync >= 0)           g_prof[PROF_MC].sync = (uint8_t)sync;
    if(g_active == PROF_MC) apply_profile(PROF_MC);   /* take effect now */
}

/* MeshCore identity (pubkey hex) for the serial console */
extern "C" int lz_backend_mc_id(char *buf, int n)
{
    if(!g_mc_id_ok) return snprintf(buf, n, "(no MeshCore identity)");
    int k = snprintf(buf, n, "MeshCore pubkey: ");
    for(int i = 0; i < 32 && k < n - 2; i++) k += snprintf(buf + k, n - k, "%02x", g_mc_pub[i]);
    return k;
}

/* self-test: build our advert, parse + Ed25519-verify it exactly as a remote
 * MeshCore node would. VALID here == a real node will accept our advert. */
extern "C" int lz_backend_mc_selftest(char *buf, int n)
{
    if(!g_mc_id_ok) return snprintf(buf, n, "no MeshCore identity");
    uint8_t frame[160];
    int fl = build_mc_advert(frame, sizeof frame, true);
    if(fl <= 0) return snprintf(buf, n, "build failed");

    mc_pkt_t p;
    if(!mc_parse(frame, fl, &p)) return snprintf(buf, n, "parse failed");
    mc_advert_t a;
    if(!mc_advert_decode(&p, &a)) return snprintf(buf, n, "decode failed (type=%d)", p.payload_type);

    const uint8_t *pk = p.payload, *ts = p.payload + 32, *sig = p.payload + 36, *app = p.payload + 100;
    int app_len = p.payload_len - 100;
    uint8_t msg[32 + 4 + MAX_ADVERT_DATA_SIZE_LZ]; int ml = 0;
    memcpy(msg + ml, pk, 32);     ml += 32;
    memcpy(msg + ml, ts, 4);      ml += 4;
    memcpy(msg + ml, app, app_len); ml += app_len;
    bool ok = Ed25519::verify(sig, pk, msg, ml);

    return snprintf(buf, n, "advert %d bytes | type=%s route=FLOOD | name='%s' | Ed25519 sig=%s",
                    fl, mc_type_name(p.payload_type), a.has_name ? a.name : "", ok ? "VALID" : "INVALID");
}

/* force-send one self-advert now (retunes to MeshCore briefly if needed).
 * flood=true -> mesh-wide; flood=false -> zero-hop (neighbors only). */
extern "C" bool lz_backend_mc_advert_now(bool flood)
{
    if(!g_ok || !g_mc_id_ok) return false;
    int prev = g_active;
    if(g_active != PROF_MC) apply_profile(PROF_MC);
    bool ok = broadcast_mc_advert(flood);
    g_last_mc_advert = millis();
    if(prev != PROF_MC && !(g_net_mt && g_net_mc)) apply_profile(prev);  /* restore if not round-robin */
    return ok;
}

/* short MeshCore address ("MC-978bbe5f") for the UI identity card */
extern "C" void lz_backend_mc_addr(char *buf, int n)
{
    if(g_mc_id_ok)
        snprintf(buf, n, "MC-%02x%02x%02x%02x", g_mc_pub[0], g_mc_pub[1], g_mc_pub[2], g_mc_pub[3]);
    else
        snprintf(buf, n, "MeshCore");
}

/* one-line schedule/diagnostic summary for the serial console */
extern "C" int lz_backend_tdm_info(char *buf, int n)
{
    const char *mode = (g_net_mt && g_net_mc) ? "SPLIT (round-robin)"
                     : g_net_mt ? "Meshtastic 100%"
                     : g_net_mc ? "MeshCore 100%" : "idle";
    const rf_prof_t *a = &g_prof[g_active];
    uint32_t rem = (g_slot_until && millis() < g_slot_until) ? g_slot_until - millis() : 0;
    return snprintf(buf, n,
        "mode: %s\n"
        "active: %s  %.3f MHz  BW %.1f  SF%d  CR4/%d  (slot %lums left)\n"
        "Meshtastic: %.3f MHz BW%.0f SF%d   rx %lu\n"
        "MeshCore:   %.3f MHz BW%.1f SF%d   rx %lu\n"
        "switches: %lu",
        mode, g_active == PROF_MT ? "Meshtastic" : "MeshCore",
        (double)a->freq, (double)a->bw, a->sf, a->cr, (unsigned long)rem,
        (double)g_prof[PROF_MT].freq, (double)g_prof[PROF_MT].bw, g_prof[PROF_MT].sf, (unsigned long)g_rx_mt,
        (double)g_prof[PROF_MC].freq, (double)g_prof[PROF_MC].bw, g_prof[PROF_MC].sf, (unsigned long)g_rx_mc,
        (unsigned long)g_switches);
}

#endif /* LZ_TARGET_TDECK */
