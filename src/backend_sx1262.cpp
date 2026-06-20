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
#include "services/mc_x25519.h"
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
static uint32_t g_slot_until;             /* millis() when the current dwell ends (0 = LOCKED sentinel, no switching) */
static uint32_t g_rx_guard_until;         /* hard cap for deferring a switch while a packet is mid-RX */
static uint32_t g_ack_dwell_until;        /* no-yield window after our MC DM to catch its ACK (both-on only) */
static int      g_airtime_mode = LZ_AIRTIME_DEFAULT;
/* Both-on dwell floors. A blocking TX can run a slot long (the loop only
 * re-evaluates between blocking ops), so these are targets rather than hard
 * real-time guarantees. The default preserves the prior 300/200ms MT bias. */
static const uint32_t SLOT_TOTAL_MS = 500;
static const uint32_t ACK_DWELL_MC = 700; /* linger on MC after our want-ack DM (peer TXT_ACK_DELAY 200 + ack airtime + ~1 hop) */
static uint32_t g_rx_mt, g_rx_mc;         /* per-network packet counts */
static uint32_t g_switches;               /* TDM profile switches */
static bool     g_switch_waiting;         /* slot expired and is waiting to retune */
static bool     g_switch_wait_rx;         /* current late switch saw RX hold */
static bool     g_switch_wait_ack;        /* current late switch saw ACK-dwell hold */
static uint32_t g_switch_due_ms;          /* original slot deadline for the waiting switch */
static uint32_t g_switch_late_count;      /* switches delayed by RX/ACK holds */
static uint64_t g_switch_late_total_ms;
static uint32_t g_switch_late_max_ms;
static uint32_t g_rx_hold_count;          /* expired-slot episodes held for in-flight RX */
static uint32_t g_ack_hold_count;         /* expired-slot episodes held for MC ACK dwell */

static void tdm_wait_reset(void)
{
    g_switch_waiting = false;
    g_switch_wait_rx = false;
    g_switch_wait_ack = false;
    g_switch_due_ms = 0;
}

static void tdm_wait_due(uint32_t due)
{
    if(g_switch_waiting) return;
    g_switch_waiting = true;
    g_switch_due_ms = due;
    g_switch_wait_rx = false;
    g_switch_wait_ack = false;
}

static void tdm_note_rx_hold(void)
{
    if(!g_switch_wait_rx) {
        g_switch_wait_rx = true;
        g_rx_hold_count++;
    }
}

static void tdm_note_ack_hold(void)
{
    if(!g_switch_wait_ack) {
        g_switch_wait_ack = true;
        g_ack_hold_count++;
    }
}

static void tdm_note_switch(uint32_t now)
{
    if(g_switch_waiting && (g_switch_wait_rx || g_switch_wait_ack)) {
        uint32_t late = now - g_switch_due_ms;
        g_switch_late_count++;
        g_switch_late_total_ms += late;
        if(late > g_switch_late_max_ms) g_switch_late_max_ms = late;
    }
    tdm_wait_reset();
}

static uint32_t slot_ms_for(int which)
{
    int mt_pct, mc_pct;
    lz_airtime_split_pct(g_airtime_mode, &mt_pct, &mc_pct);
    uint32_t pct = (uint32_t)(which == PROF_MT ? mt_pct : mc_pct);
    return (SLOT_TOTAL_MS * pct + 50u) / 100u;
}

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

/* tune to a profile and (re)start its dwell. g_rx_guard_until caps how long the
 * NEXT switch may be deferred to finish an in-flight packet — roughly the max
 * airtime of a frame on that profile (SF11/BW250 is far slower than SF7/BW62.5). */
static void slot_begin(int which, uint32_t now)
{
    tdm_wait_reset();
    apply_profile(which);
    g_slot_until = now + slot_ms_for(which);
    g_rx_guard_until = g_slot_until + (which == PROF_MT ? 2000u : 600u);
    g_ack_dwell_until = 0;   /* any retune ends a prior MC ACK-dwell; the MC DM re-arms after */
}

static void handle_rx_mt(void);   /* fwd */
static void handle_rx_mc(void);   /* fwd */

/* drain a completed frame on the current profile (clears g_rx_flag) */
static void drain_rx(void)
{
    if(g_rx_flag) { g_rx_flag = false; if(g_active == PROF_MT) handle_rx_mt(); else handle_rx_mc(); }
}

/* Retune for a SEND/RETRY (off the loop's yield path). The loop's mid-RX guard
 * only covers timer-driven switches; a send, a 30s DM retry, or a NodeInfo
 * request can also retune and would standby() the radio mid-RX, clobbering an
 * inbound frame. This finishes any in-flight RX first, then retunes. In LOCKED
 * (one net) it never writes the g_slot_until sentinel. */
static void retune_guarded(int target, uint32_t now)
{
    if(g_active == target) return;                 /* already tuned (incl. LOCKED): nothing to do */
    if(g_net_mt && g_net_mc) {
        drain_rx();                                /* a completed frame is waiting */
        uint16_t irq = radio.getIrqStatus();
        if((irq & RADIOLIB_SX126X_IRQ_HEADER_VALID) && !(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) {
            uint32_t cap = now + (g_active == PROF_MT ? 2000u : 600u);   /* current profile's max airtime */
            while((uint32_t)millis() < cap) {      /* let the in-flight frame finish */
                if(g_rx_flag) { drain_rx(); break; }
                if(radio.getIrqStatus() & RADIOLIB_SX126X_IRQ_RX_DONE) { drain_rx(); break; }
                delay(2);
            }
        }
        slot_begin(target, now);                   /* both-on: retune + (re)start dwell */
    } else {
        apply_profile(target);                     /* single-net edge: retune, keep sentinel */
    }
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

/* MeshCore dedup: a message reflooded by several repeaters arrives multiple
 * times. The path bytes change but the payload ([hash][mac][ciphertext]) is
 * identical, so hash that to drop duplicates. */
#define MC_SEEN_N 64        /* busy tree/repeater meshes reflood a lot; keep a deep ring */
static uint32_t g_mc_seen[MC_SEEN_N];
static int      g_mc_seen_head;
static bool mc_seen_before(const uint8_t *payload, int len)
{
    uint32_t h = 2166136261u;
    for(int i = 0; i < len; i++) { h ^= payload[i]; h *= 16777619u; }
    h ^= (uint32_t)len;
    if(!h) h = 1;
    for(int i = 0; i < MC_SEEN_N; i++) if(g_mc_seen[i] == h) return true;
    g_mc_seen[g_mc_seen_head] = h;
    g_mc_seen_head = (g_mc_seen_head + 1) % MC_SEEN_N;
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

static void send_nodeinfo(uint32_t to, bool want_response);   /* fwd decl */
static void send_routing_ack(uint32_t to, uint32_t req_id);   /* fwd decl */
static void flush_pending_dm(uint32_t to);                    /* fwd decl */

/* DMs to a node whose key we don't have yet are queued here while we request the
 * key (retried), then sent for real once it arrives — so a DM never silently
 * falls back to PSK (which modern nodes ignore). */
#define LZ_PEND_N 6
static struct { lz_mt_packet_t pkt; uint32_t req_ms; uint8_t tries; bool used; } g_pend[LZ_PEND_N];
static volatile bool g_rxlog;     /* serial: log inbound MT packet metadata + decode */
extern "C" void lz_backend_set_rxlog(bool on) { g_rxlog = on; }
extern "C" bool lz_backend_rxlog_enabled(void) { return g_rxlog; }

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
    if(g_rxlog) Serial.printf("[ni] from=!%08x name='%s' short='%s' hw=%d pubkey=%s (userlen=%d)\n",
                              (unsigned)from, longn, shortn, hw, pubkey ? "YES" : "no", len);
    lz_core_on_nodeinfo(from, id[0] ? id : NULL, longn[0] ? longn : NULL,
                        shortn[0] ? shortn : NULL, -1,
                        hw > 0 ? lz_svc_mt_hw_label(hw) : NULL, snr);
    if(pubkey) { lz_core_on_pubkey(from, pubkey);   /* remember key for PKI DMs */
                 flush_pending_dm(from); }           /* send DMs that were waiting on it */
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

    if(g_rxlog)
        Serial.printf("[rx] ch=0x%02x from=!%08x to=!%08x id=%08x plen=%d%s\n",
                      f.channel_hash, (unsigned)f.from, (unsigned)f.to, (unsigned)f.id,
                      f.plen, f.to == me ? " <-US" : "");

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
        bool have = lz_mtpki_ready() && lz_svc_node_pubkey(f.from, sender_pub);
        if(have) dl = lz_mtpki_decrypt(sender_pub, f.from, f.id, f.payload, f.plen, dec, sizeof dec);
        if(g_rxlog) Serial.printf("[rx]   PKI DM to us: have_key=%d decrypt=%s\n",
                                  have, dl > 0 ? "OK" : "FAIL");
    }
    if(dl > 0) {
        mt_data_t d;
        if(mt_data_decode(dec, dl, &d)) {
            if(g_rxlog) Serial.printf("[rx]   decoded portnum=%d len=%d%s\n",
                                      d.portnum, d.plen, d.want_response ? " want_response" : "");
            /* acknowledge a want-ack packet addressed to us so the sender sees
             * "delivered" (don't ack an ack) */
            if(f.to == me && f.want_ack && d.portnum != MT_PORT_ROUTING)
                send_routing_ack(f.from, f.id);
            int hops_used = (int)f.hop_start - (int)f.hop_limit;
            if(hops_used < 0) hops_used = 0;
            /* companion mode: hand the whole decoded packet to the phone app */
            if(lz_mtc_any_active())
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
                /* a targeted NodeInfo request -> reply with ours (incl. our key) */
                if(d.want_response && f.to == me) send_nodeinfo(f.from, false);
            } else if(d.portnum == MT_PORT_POSITION) {
                mt_position_t pos;
                if(mt_position_decode(d.payload, d.plen, &pos) && pos.has_lat && pos.has_lon) {
                    if(g_rxlog)
                        Serial.printf("[rx]   position lat_i=%ld lon_i=%ld alt=%ld prec=%u\n",
                                      (long)pos.latitude_i, (long)pos.longitude_i,
                                      pos.has_alt ? (long)pos.altitude_m : 0L,
                                      (unsigned)pos.precision_bits);
                    lz_core_on_position(f.from, pos.latitude_i, pos.longitude_i,
                                        pos.has_alt, pos.altitude_m, pos.time,
                                        pos.precision_bits, snr);
                }
            } else if(d.portnum == MT_PORT_TELEMETRY) {
                mt_telemetry_t mt;
                if(mt_telemetry_decode(d.payload, d.plen, &mt)) {
                    lz_node_telemetry_t tel;
                    memset(&tel, 0, sizeof tel);
                    tel.has_battery = mt.has_battery;
                    tel.battery_pct = mt.battery_level;
                    tel.has_voltage = mt.has_voltage;
                    tel.voltage = mt.voltage;
                    tel.has_uptime = mt.has_uptime;
                    tel.uptime_s = mt.uptime_s;
                    tel.has_temperature = mt.has_temperature;
                    tel.temperature_c = mt.temperature_c;
                    tel.has_humidity = mt.has_humidity;
                    tel.humidity_pct = mt.humidity_pct;
                    tel.has_pressure = mt.has_pressure;
                    tel.pressure_hpa = mt.pressure_hpa;
                    if(g_rxlog)
                        Serial.printf("[rx]   telemetry batt=%s volt=%s env=%s\n",
                                      mt.has_battery ? "Y" : "n",
                                      mt.has_voltage ? "Y" : "n",
                                      (mt.has_temperature || mt.has_humidity || mt.has_pressure) ? "Y" : "n");
                    lz_core_on_telemetry(f.from, &tel, snr);
                }
            } else if(d.portnum == MT_PORT_ROUTING && d.request_id) {
                lz_core_on_ack(d.request_id);   /* delivery ack for one of our DMs */
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
static void handle_mc_dm(const mc_pkt_t *p, float snr);               /* fwd */
static void handle_mc_ack(const uint8_t *ack4);                       /* fwd */
static void handle_mc_path(const mc_pkt_t *p);                        /* fwd */

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
        if(mc_advert_decode(&p, &a)) {
            lz_core_on_mc_node(a.pubkey, a.has_name ? a.name : NULL, a.adv_type, snr);  /* persists key for DM ECDH */
            Serial.printf("[mc] ADVERT %s (snr %.1f)\n", a.has_name ? a.name : "(noname)", snr);
        }
    } else if(p.payload_type == MC_PAYLOAD_GRP_TXT) {
        /* V0.6: try the default Public channel. MAC mismatch => some other channel. */
        mc_group_msg_t g;
        if(mc_group_decode(&p, MC_PUBLIC_SECRET, &g)) {
            if(mc_seen_before(p.payload, p.payload_len)) return;   /* dup reflood */
            const char *me = lz_svc_identity()->long_name;
            bool mine = g.sender[0] && me[0] && strcmp(g.sender, me) == 0;
            Serial.printf("[mc] Public  %s: %s  (snr %.1f)%s\n",
                          g.sender[0] ? g.sender : "?", g.text, snr, mine ? " [self]" : "");
            if(!mine) lz_core_on_mc_channel_text(g.sender, g.text, snr);  /* not our own echo */
        } else {
            Serial.printf("[mc] GRP_TXT chan=%02x (not Public / undecodable)\n",
                          mc_group_channel_hash(&p));
        }
    } else if(p.payload_type == MC_PAYLOAD_TXT_MSG) {
        if(mc_seen_before(p.payload, p.payload_len)) return;       /* dup reflood */
        handle_mc_dm(&p, snr);
    } else if(p.payload_type == MC_PAYLOAD_ACK) {
        if(p.payload_len >= 4) handle_mc_ack(p.payload);
    } else if(p.payload_type == MC_PAYLOAD_PATH) {
        /* a flood DM is ACKed via a PATH packet carrying the ack as 'extra' */
        handle_mc_path(&p);
    } else {
        Serial.printf("[mc] %s (encrypted, snr %.1f)\n", mc_type_name(p.payload_type), snr);
    }
}

/* ---- NodeInfo: broadcast our User so peers learn our name + public key, or
 *      send it to one node with want_response to REQUEST their NodeInfo back
 *      (how keys are exchanged promptly instead of waiting hours) ---- */
static uint32_t g_last_nodeinfo;

static void send_nodeinfo(uint32_t to, bool want_response)
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
    user[n++] = (5 << 3) | 0; user[n++] = 50;    /* hw_model = T_DECK (so peers show the right model) */
    if(lz_mtpki_ready()) {                       /* advertise our X25519 key for PKI DMs */
        user[n++] = (8 << 3) | 2; user[n++] = 32;
        memcpy(user + n, lz_mtpki_pubkey(), 32); n += 32;
    }

    mt_data_t d;
    memset(&d, 0, sizeof d);
    d.portnum = MT_PORT_NODEINFO;
    d.want_response = want_response;             /* set on a request so the peer replies */
    memcpy(d.payload, user, n);
    d.plen = (uint8_t)n;

    uint8_t plain[128];
    int pn = mt_data_encode(plain, sizeof plain, &d);
    if(pn < 0) return;
    uint32_t pid = (uint32_t)(esp_random() | 1);
    mt_crypt(plain, pn, id->num, pid);

    mt_frame_t f;
    memset(&f, 0, sizeof f);
    f.to = to; f.from = id->num; f.id = pid;
    f.hop_limit = 3; f.hop_start = 3;
    f.want_ack = false;
    f.channel_hash = mt_channel_hash();

    uint8_t frame[160];
    mt_header_write(frame, &f);
    memcpy(frame + MT_HEADER_LEN, plain, pn);
    tx_frame(frame, MT_HEADER_LEN + pn);
}

static void broadcast_nodeinfo(void) { send_nodeinfo(MT_BROADCAST, false); }

/* send a ROUTING_APP ack for a received want-ack packet, so the sender sees
 * "delivered". Empty Routing payload = implicit ACK (error_reason NONE). Goes
 * out PSK on the channel (ROUTING is not PKI-encrypted). */
static void send_routing_ack(uint32_t to, uint32_t req_id)
{
    if(!g_ok) return;
    const lz_identity_t *id = lz_svc_identity();
    mt_data_t d;
    memset(&d, 0, sizeof d);
    d.portnum = MT_PORT_ROUTING;
    d.request_id = req_id;
    d.plen = 0;
    uint8_t plain[64];
    int pn = mt_data_encode(plain, sizeof plain, &d);
    if(pn < 0) return;
    uint32_t pid = (uint32_t)(esp_random() | 1);
    mt_crypt(plain, pn, id->num, pid);

    mt_frame_t f;
    memset(&f, 0, sizeof f);
    f.to = to; f.from = id->num; f.id = pid;
    f.hop_limit = 3; f.hop_start = 3;
    f.channel_hash = mt_channel_hash();

    uint8_t frame[96];
    mt_header_write(frame, &f);
    memcpy(frame + MT_HEADER_LEN, plain, pn);
    tx_frame(frame, MT_HEADER_LEN + pn);
}

/* ask one node for its NodeInfo (so we learn its public key for PKI DMs) */
extern "C" void lz_backend_request_nodeinfo(uint32_t to)
{
    if(!g_ok || !g_net_mt || to == MT_BROADCAST) return;
    retune_guarded(PROF_MT, millis());
    send_nodeinfo(to, true);
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

/* MeshCore group text is length-limited on air; longer messages get clipped by
 * peers/repeaters. Keep each frame's body ("name: text") under this. */
#define MC_PUBLIC_BODY_MAX 140

static bool mc_send_one(const lz_identity_t *id, const char *text)
{
    retune_guarded(PROF_MC, millis());
    uint8_t frame[200];
    int n = mc_group_encode(frame, sizeof frame, MC_PUBLIC_SECRET,
                            lz_svc_epoch(), id->long_name, text);
    if(n <= 0) return false;
    return tx_frame(frame, n);
}

/* send a text on the MeshCore default Public channel (GRP_TXT). Long messages
 * are auto-split into "(i/n) ..." parts so none gets clipped at the cap. */
extern "C" bool lz_backend_mc_send_public(const char *text)
{
    if(!g_ok || !g_net_mc || !text || !text[0]) return false;
    const lz_identity_t *id = lz_svc_identity();
    int budget = MC_PUBLIC_BODY_MAX - (int)strlen(id->long_name) - 2;  /* room after "name: " */
    if(budget < 16) budget = 16;
    int tlen = (int)strlen(text);

    bool ok;
    if(tlen <= budget) {
        ok = mc_send_one(id, text);                           /* fits in one frame */
    } else {
        int slice = budget - 6;                               /* leave room for "(i/n) " */
        if(slice < 8) slice = 8;
        int nparts = (tlen + slice - 1) / slice;
        if(nparts > 4) nparts = 4;                            /* cap: a multi-part burst blocks the loop (no RX, UI frozen) ~1.5s/part */
        ok = true;
        for(int i = 0; i < nparts; i++) {
            char part[180];
            int off = i * slice, n = tlen - off;
            if(n > slice) n = slice;
            if(n < 0) n = 0;
            snprintf(part, sizeof part, "(%d/%d) %.*s", i + 1, nparts, n, text + off);
            ok = mc_send_one(id, part) && ok;
            if(i + 1 < nparts) delay(400);                    /* spacing so parts don't self-collide */
        }
    }
    if(ok) lz_core_on_mc_channel_self(text);                  /* show our send in the Public thread */
    return ok;
}

/* ---- MeshCore direct messages (TXT_MSG, M4) ---------------------------------
 * DMs use a per-peer X25519 ECDH secret from the Ed25519 identities. Peer pubkeys
 * live in the node table (persisted to nodes.db via lz_core_on_mc_node), so DM
 * decode/targeting survives a reboot — no separate RAM cache. */
#define MC_DM_PEND_N  6
static struct { uint8_t ack4[4]; char peer[24]; uint32_t t_ms; bool used; } g_mc_dm_pend[MC_DM_PEND_N];

static void send_mc_ack(const uint8_t ack4[4])
{
    retune_guarded(PROF_MC, millis());
    uint8_t frame[8]; int fl = 0;
    frame[fl++] = (MC_PAYLOAD_ACK << MC_TYPE_SHIFT) | MC_ROUTE_FLOOD;
    frame[fl++] = 0;                                  /* path_len = 0 */
    memcpy(frame + fl, ack4, 4); fl += 4;
    delay(200);                                       /* TXT_ACK_DELAY */
    tx_frame(frame, fl);
}

static void handle_mc_dm(const mc_pkt_t *p, float snr)
{
    if(p->payload_len < 6) return;
    uint8_t dest_hash = p->payload[0], src_hash = p->payload[1];
    if(dest_hash != g_mc_pub[0]) return;              /* not addressed to us */

    /* 1-byte src_hash collides: try every MeshCore node whose key starts with it */
    const lz_node_rt *nodes; int nn = lz_svc_nodes(&nodes);
    mc_dm_msg_t dm;
    uint8_t ppub[32]; char pname[24]; bool found = false;
    for(int i = 0; i < nn; i++) {
        if(nodes[i].net != LZ_NET_MC || !nodes[i].has_key || nodes[i].pubkey[0] != src_hash) continue;
        uint8_t shared[32];
        mc_ed25519_dh(shared, nodes[i].pubkey, g_mc_prv);
        if(mc_dm_decode(p, shared, &dm)) {
            memcpy(ppub, nodes[i].pubkey, 32);
            snprintf(pname, sizeof pname, "%s", nodes[i].name);
            found = true; break;
        }
    }
    if(!found) {
        Serial.printf("[mc] DM to us from %02x: undecodable (peer key unknown?)\n", src_hash);
        return;
    }
    if(dm.txt_type == MC_TXT_TYPE_CLI_DATA) return;   /* CLI command, not chat, no ack */
    Serial.printf("[mc] DM from %s: %s  (snr %.1f)\n", pname[0] ? pname : "?", dm.text, snr);
    lz_core_on_mc_dm(ppub, pname, dm.text, snr);      /* -> Messages app */

    uint8_t ack4[4];
    uint8_t type_byte = (uint8_t)((dm.txt_type << 2) | dm.attempt);
    mc_dm_ack4(ack4, dm.timestamp, type_byte, dm.text, ppub);
    send_mc_ack(ack4);                                /* so the sender sees delivery */
}

static void handle_mc_ack(const uint8_t *ack4)
{
    for(int i = 0; i < MC_DM_PEND_N; i++)
        if(g_mc_dm_pend[i].used && memcmp(g_mc_dm_pend[i].ack4, ack4, 4) == 0) {
            Serial.printf("[mc] DM to %s DELIVERED\n", g_mc_dm_pend[i].peer);
            g_mc_dm_pend[i].used = false;
            return;
        }
}

static void handle_mc_path(const mc_pkt_t *p)
{
    /* a flood DM is ACKed via PATH carrying the ack as 'extra':
     * [path_len:1][path bytes][extra_type:1][extra...]. Best-effort plaintext parse. */
    if(p->payload_len < 2) return;
    int i = 0;
    uint8_t plen = p->payload[i++];
    if(i + plen + 1 > p->payload_len) return;
    i += plen;
    uint8_t extra_type = p->payload[i++];
    if(extra_type == MC_PAYLOAD_ACK && i + 4 <= p->payload_len)
        handle_mc_ack(p->payload + i);
}

static bool mc_name_eq_ci(const char *a, const char *b)
{
    if(!a || !b) return false;
    while(*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if(ca != cb) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static bool send_mc_dm_to_key(const uint8_t ppub[32], const char *name_hint, const char *text)
{
    if(!g_ok || !g_net_mc || !ppub || !text || !text[0]) return false;
    char pname[24];
    snprintf(pname, sizeof pname, "%s", (name_hint && name_hint[0]) ? name_hint : "MeshCore peer");

    const lz_node_rt *nodes; int nn = lz_svc_nodes(&nodes);
    for(int i = 0; i < nn; i++) {
        if(nodes[i].net == LZ_NET_MC && nodes[i].has_key &&
           memcmp(nodes[i].pubkey, ppub, 32) == 0) {
            if(nodes[i].name[0]) snprintf(pname, sizeof pname, "%s", nodes[i].name);
            break;
        }
    }

    retune_guarded(PROF_MC, millis());
    uint8_t shared[32];
    mc_ed25519_dh(shared, ppub, g_mc_prv);
    uint8_t frame[200], ack4[4];
    uint32_t ts = lz_svc_epoch();
    int n = mc_dm_encode(frame, sizeof frame, shared, ppub[0], g_mc_pub[0],
                         ts, MC_TXT_TYPE_PLAIN, 0, text, g_mc_pub, ack4);
    if(n <= 0) return false;
    bool sent = tx_frame(frame, n);
    if(sent) {
        lz_core_on_mc_dm_self(ppub, pname, text);     /* show our send in the thread */
        int slot = -1;
        for(int i = 0; i < MC_DM_PEND_N; i++) if(!g_mc_dm_pend[i].used) { slot = i; break; }
        if(slot < 0) slot = 0;
        memcpy(g_mc_dm_pend[slot].ack4, ack4, 4);
        snprintf(g_mc_dm_pend[slot].peer, sizeof g_mc_dm_pend[slot].peer, "%s", pname);
        g_mc_dm_pend[slot].t_ms = millis();
        g_mc_dm_pend[slot].used = true;
        if(g_net_mt && g_net_mc)                      /* both-on: linger on MC to catch the ACK */
            g_ack_dwell_until = millis() + ACK_DWELL_MC;
    }
    return sent;
}

/* send a direct message to an exact MeshCore public key selected by the core. */
extern "C" bool lz_backend_mc_dm_key(const uint8_t peer_pub[32],
                                     const char *name_hint,
                                     const char *text)
{
    return send_mc_dm_to_key(peer_pub, name_hint, text);
}

/* legacy serial-console wrapper: exact unique display-name match only. */
extern "C" bool lz_backend_mc_dm(const char *name, const char *text)
{
    if(!name || !name[0]) return false;
    const lz_node_rt *nodes; int nn = lz_svc_nodes(&nodes);
    const lz_node_rt *match = NULL;
    for(int i = 0; i < nn; i++) {
        if(nodes[i].net != LZ_NET_MC || !nodes[i].has_key || !nodes[i].name[0])
            continue;
        if(!mc_name_eq_ci(nodes[i].name, name)) continue;
        if(match) return false;
        match = &nodes[i];
    }
    return match && send_mc_dm_to_key(match->pubkey, match->name, text);
}

/* list known MeshCore peers (serial `mc peers`) */
extern "C" int lz_backend_mc_peers(char *buf, int n)
{
    const lz_node_rt *nodes; int nn = lz_svc_nodes(&nodes);
    int k = 0, count = 0;
    for(int i = 0; i < nn; i++) if(nodes[i].net == LZ_NET_MC && nodes[i].has_key) {
        count++;
        if(k < n - 1)
            k += snprintf(buf + k, n - k, "  %02x  %s\n", nodes[i].pubkey[0],
                          nodes[i].name[0] ? nodes[i].name : "(noname)");
    }
    if(count == 0) snprintf(buf, n, "no MeshCore peers with keys yet\n");
    return count;
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

    uint32_t now = millis();

    /* Process a completed packet FIRST, on the profile we received it on: a retune
     * (apply_profile -> standby) would drop the just-received frame still in the
     * radio buffer. */
    drain_rx();

    /* age out stale MC-DM delivery-tracking entries so the table can't fill (30s) */
    for(int i = 0; i < MC_DM_PEND_N; i++)
        if(g_mc_dm_pend[i].used && (uint32_t)(now - g_mc_dm_pend[i].t_ms) > 30000u)
            g_mc_dm_pend[i].used = false;

    /* TDM: hand the radio to the other profile when the dwell expires — but never
     * mid-packet. If a valid LoRa header is mid-reception, hold the slot open and
     * switch the instant the packet finishes (RX_DONE) or the guard elapses, so a
     * stuck/aborted header can't freeze the cycle. */
    if(g_net_mt && g_net_mc && g_slot_until && (int32_t)(now - g_slot_until) >= 0) {
        drain_rx();                       /* same-tick: a frame may have completed since the top */
        now = millis();
        tdm_wait_due(g_slot_until);
        uint16_t irq = radio.getIrqStatus();
        bool rx_in_flight = (irq & RADIOLIB_SX126X_IRQ_HEADER_VALID) &&
                           !(irq & RADIOLIB_SX126X_IRQ_RX_DONE);
        if(rx_in_flight && (int32_t)(now - g_rx_guard_until) < 0) {
            tdm_note_rx_hold();
            /* hold: finish the in-flight frame */
        } else if((int32_t)(now - g_ack_dwell_until) < 0) {
            tdm_note_ack_hold();
            /* hold: lingering on MC to catch our DM's ACK (g_ack_dwell_until=0 => no hold) */
        } else {
            tdm_note_switch(now);
            slot_begin(g_active == PROF_MT ? PROF_MC : PROF_MT, now);   /* clears g_ack_dwell_until */
            g_switches++;
        }
    }

    /* re-request keys for queued DMs every ~4s until they arrive; give up after
     * ~8 tries so a message to an unreachable node doesn't queue forever */
    if(g_net_mt && g_active == PROF_MT) {
        for(int i = 0; i < LZ_PEND_N; i++) {
            if(!g_pend[i].used) continue;
            if(now - g_pend[i].req_ms < 4000) continue;
            if(g_pend[i].tries >= 8) { g_pend[i].used = false; continue; }   /* failed (delivery-status will flag it) */
            g_pend[i].req_ms = now; g_pend[i].tries++;
            lz_backend_request_nodeinfo(g_pend[i].pkt.to);
        }
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
    retune_guarded(PROF_MT, millis());

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
    bool is_dm = p->to != MT_BROADCAST && p->to != p->from;
    bool pki = is_dm && lz_mtpki_ready() && lz_svc_node_pubkey(p->to, peer_pub);
    /* DM but we don't have the peer's key yet: queue it + request the key
     * (retried in the loop), then send for real when the key lands */
    if(is_dm && !pki && lz_mtpki_ready()) {
        for(int i = 0; i < LZ_PEND_N; i++) if(!g_pend[i].used) {
            g_pend[i].pkt = *p; g_pend[i].req_ms = millis(); g_pend[i].tries = 1; g_pend[i].used = true; break;
        }
        lz_backend_request_nodeinfo(p->to);
        return true;                                 /* queued (will send on key arrival) */
    }
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

/* a key just arrived for `to` — send any DMs we queued waiting for it */
static void flush_pending_dm(uint32_t to)
{
    for(int i = 0; i < LZ_PEND_N; i++) {
        if(g_pend[i].used && g_pend[i].pkt.to == to) {
            lz_mt_packet_t pk = g_pend[i].pkt;
            g_pend[i].used = false;
            lz_backend_send(&pk);                    /* we have the key now -> PKI */
        }
    }
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
    drain_rx();                          /* don't drop a frame just received on the old profile */
    g_ack_dwell_until = 0;
    tdm_wait_reset();
    if(mt && mc) {                       /* share the radio, start on Meshtastic */
        slot_begin(PROF_MT, millis());
    } else if(mt) {
        apply_profile(PROF_MT); g_slot_until = 0;
    } else if(mc) {
        apply_profile(PROF_MC); g_slot_until = 0;
    } else {
        radio.standby(); g_slot_until = 0; /* neither: stop listening */
    }
}

extern "C" void lz_backend_set_airtime(int mode)
{
    int next = lz_airtime_mode_clamp(mode);
    if(g_airtime_mode == next) return;
    g_airtime_mode = next;
    if(!g_ok || !(g_net_mt && g_net_mc)) return;
    drain_rx();
    tdm_wait_reset();
    slot_begin(g_active, millis());
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
    if(g_active == PROF_MC) { drain_rx(); apply_profile(PROF_MC); }   /* take effect now (don't drop a just-RXed frame) */
}

/* MeshCore identity (pubkey hex) for the serial console */
extern "C" int lz_backend_mc_id(char *buf, int n)
{
    if(!g_mc_id_ok) return snprintf(buf, n, "(no MeshCore identity)");
    int k = snprintf(buf, n, "MeshCore pubkey: ");
    for(int i = 0; i < 32 && k < n - 2; i++) k += snprintf(buf + k, n - k, "%02x", g_mc_pub[i]);
    return k;
}

extern "C" bool lz_backend_mc_pubkey(uint8_t out32[32])
{
    if(!g_mc_id_ok || !out32) return false;
    memcpy(out32, g_mc_pub, 32);
    return true;
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
    retune_guarded(PROF_MC, millis());   /* coherent slot under split; plain retune in LOCKED */
    bool ok = broadcast_mc_advert(flood);
    g_last_mc_advert = millis();
    if(prev != PROF_MC && !(g_net_mt && g_net_mc)) { drain_rx(); apply_profile(prev); }  /* restore if not round-robin */
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
    int mt_pct, mc_pct;
    lz_airtime_split_pct(g_airtime_mode, &mt_pct, &mc_pct);
    char mode[64];
    if(g_net_mt && g_net_mc)
        snprintf(mode, sizeof mode, "SPLIT MT %d%% / MC %d%% (%s)", mt_pct, mc_pct,
                 lz_airtime_mode_label(g_airtime_mode));
    else
        snprintf(mode, sizeof mode, "%s", g_net_mt ? "Meshtastic 100%"
                                      : g_net_mc ? "MeshCore 100%" : "idle");
    const rf_prof_t *a = &g_prof[g_active];
    uint32_t rem = (g_slot_until && millis() < g_slot_until) ? g_slot_until - millis() : 0;
    uint32_t late_avg = g_switch_late_count
        ? (uint32_t)(g_switch_late_total_ms / g_switch_late_count)
        : 0;
    return snprintf(buf, n,
        "mode: %s\n"
        "dwell: Meshtastic %lums / MeshCore %lums\n"
        "active: %s  %.3f MHz  BW %.1f  SF%d  CR4/%d  (slot %lums left)\n"
        "Meshtastic: %.3f MHz BW%.0f SF%d   rx %lu\n"
        "MeshCore:   %.3f MHz BW%.1f SF%d   rx %lu\n"
        "switches: %lu\n"
        "timing: late %lu avg %lums max %lums | holds rx %lu ack %lu",
        mode, (unsigned long)slot_ms_for(PROF_MT), (unsigned long)slot_ms_for(PROF_MC),
        g_active == PROF_MT ? "Meshtastic" : "MeshCore",
        (double)a->freq, (double)a->bw, a->sf, a->cr, (unsigned long)rem,
        (double)g_prof[PROF_MT].freq, (double)g_prof[PROF_MT].bw, g_prof[PROF_MT].sf, (unsigned long)g_rx_mt,
        (double)g_prof[PROF_MC].freq, (double)g_prof[PROF_MC].bw, g_prof[PROF_MC].sf, (unsigned long)g_rx_mc,
        (unsigned long)g_switches,
        (unsigned long)g_switch_late_count, (unsigned long)late_avg,
        (unsigned long)g_switch_late_max_ms,
        (unsigned long)g_rx_hold_count, (unsigned long)g_ack_hold_count);
}

#endif /* LZ_TARGET_TDECK */
