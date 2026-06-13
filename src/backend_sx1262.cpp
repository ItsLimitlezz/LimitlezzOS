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
#include "services/mesh.h"
#include "services/mtproto.h"

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

static void handle_rx(void)
{
    uint8_t buf[256];
    int len = radio.getPacketLength();
    if(len <= 0 || len > (int)sizeof buf) { radio.startReceive(); return; }
    int st = radio.readData(buf, len);
    radio.startReceive();
    if(st != RADIOLIB_ERR_NONE) return;

    float snr = radio.getSNR();
    g_stats.rx_count++;

    mt_frame_t f;
    if(!mt_header_read(buf, len, &f)) return;
    if(seen_before(f.from, f.id)) return;        /* dup: ignore + don't rebroadcast */

    uint32_t me = lz_svc_identity()->num;
    if(f.from == me) return;                     /* our own echo */

    lz_core_on_heard(f.from, snr);

    /* only decode if it's on our channel (hash hint) */
    if(f.channel_hash == mt_channel_hash() && f.plen > 0) {
        uint8_t dec[251];
        int dl = f.plen;
        memcpy(dec, f.payload, dl);
        mt_crypt(dec, dl, f.from, f.id);         /* CTR: decrypt == encrypt */
        mt_data_t d;
        if(mt_data_decode(dec, dl, &d)) {
            int hops_used = (int)f.hop_start - (int)f.hop_limit;
            if(hops_used < 0) hops_used = 0;
            if(d.portnum == MT_PORT_TEXT && d.plen) {
                char text[LZ_TEXT_MAX];
                int tl = d.plen < (int)sizeof text - 1 ? d.plen : (int)sizeof text - 1;
                memcpy(text, d.payload, tl);
                text[tl] = 0;
                uint32_t to = f.to;
                lz_core_on_text(f.from, to, text, hops_used, snr);
            } else if(d.portnum == MT_PORT_NODEINFO) {
                parse_user(d.payload, d.plen, f.from, snr);
            }
        }
    }

    /* managed flood: rebroadcast packets not addressed to us */
    if(f.to != me) rebroadcast(buf, len, &f);
}

/* ---- NodeInfo (our User) periodic broadcast so peers learn our name ---- */
static uint32_t g_last_nodeinfo;

static void broadcast_nodeinfo(void)
{
    const lz_identity_t *id = lz_svc_identity();
    uint8_t user[64];
    int n = 0;
    /* User{ id=field1 str, long_name=field2 str, short_name=field3 str } */
    user[n++] = (1 << 3) | 2; user[n++] = (uint8_t)strlen(id->id);
    memcpy(user + n, id->id, strlen(id->id)); n += strlen(id->id);
    user[n++] = (2 << 3) | 2; user[n++] = (uint8_t)strlen(id->long_name);
    memcpy(user + n, id->long_name, strlen(id->long_name)); n += strlen(id->long_name);
    user[n++] = (3 << 3) | 2; user[n++] = (uint8_t)strlen(id->short_name);
    memcpy(user + n, id->short_name, strlen(id->short_name)); n += strlen(id->short_name);

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
}

bool lz_backend_ok(void) { return g_ok; }
int  lz_backend_begin_state(void) { return g_begin_state; }

void lz_backend_loop(void)
{
    if(!g_ok) return;
    if(g_rx_flag) { g_rx_flag = false; handle_rx(); }

    /* announce our node info shortly after boot, then every ~5 min */
    uint32_t now = millis();
    if(g_last_nodeinfo == 0 ? (now - g_boot_ms > 4000)
                            : (now - g_last_nodeinfo > 300000)) {
        g_last_nodeinfo = now;
        broadcast_nodeinfo();
    }
}

bool lz_backend_send(lz_mt_packet_t *p)
{
    if(!g_ok) return false;
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
    mt_crypt(plain, pn, p->from, p->id);

    mt_frame_t f;
    memset(&f, 0, sizeof f);
    f.to = p->to; f.from = p->from; f.id = p->id;
    f.hop_limit = p->hop_limit; f.hop_start = p->hop_start ? p->hop_start : p->hop_limit;
    f.want_ack = p->want_ack;
    f.channel_hash = mt_channel_hash();

    uint8_t frame[256];
    if(MT_HEADER_LEN + pn > (int)sizeof frame) return false;
    mt_header_write(frame, &f);
    memcpy(frame + MT_HEADER_LEN, plain, pn);
    return tx_frame(frame, MT_HEADER_LEN + pn);
}

void lz_backend_stats(lz_radio_stats_t *out)
{
    uint32_t up = millis() - g_boot_ms;
    g_stats.util_pct = up ? (float)g_airtime_ms / (float)up * 100.0f : 0.0f;
    *out = g_stats;
}

#endif /* LZ_TARGET_TDECK */
