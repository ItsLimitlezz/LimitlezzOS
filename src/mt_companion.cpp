/**
 * Meshtastic companion bridge (USB serial Stream API).
 *
 * When companion mode is on, the USB serial port stops being our text console
 * and instead speaks the Meshtastic Stream API so the official Meshtastic phone
 * / desktop app can drive this radio:
 *
 *   frame = 0x94 0xC3 <len_hi> <len_lo> <protobuf>
 *   app -> radio: ToRadio   { want_config_id=3, packet=1, disconnect=4, heartbeat=7 }
 *   radio -> app: FromRadio { my_info=3, node_info=4, channel=10,
 *                             config_complete_id=7, packet=2 }
 *
 * On want_config_id the radio replies with MyNodeInfo, a NodeInfo per known
 * node, the primary channel, then config_complete_id (echoing the nonce). After
 * that, received mesh packets are pushed up as FromRadio.packet and packets the
 * app sends (ToRadio.packet) are transmitted on the SX1262.
 *
 * Field numbers verified against meshtastic/protobufs mesh.proto (master).
 * Detailed Config/ModuleConfig messages are omitted for now (the app connects
 * and shows the node DB + messages without them).
 */
#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include "services/mesh.h"
#include "services/mtproto.h"
#include "services/wifi.h"   /* BLE<->WiFi are mutually exclusive (shared internal RAM) */

static bool g_companion;          /* USB serial companion mode */

extern "C" bool lz_mtc_active(void) { return g_companion; }

/* Meshtastic BLE API UUIDs:
 * service   6ba1b218-15a8-461f-9fa8-5dcae273eafd
 * FromRadio 2c55e69e-4993-11ed-b878-0242ac120002
 * ToRadio   f75c76d2-129e-4dad-a1dd-7866124401e7
 * FromNum   ed9da18c-a800-4f66-a670-aa7547e34453
 */
#define MTC_BLE_SERVICE_UUID   "6ba1b218-15a8-461f-9fa8-5dcae273eafd"
#define MTC_BLE_FROM_UUID      "2c55e69e-4993-11ed-b878-0242ac120002"
#define MTC_BLE_TO_UUID        "f75c76d2-129e-4dad-a1dd-7866124401e7"
#define MTC_BLE_FROMNUM_UUID   "ed9da18c-a800-4f66-a670-aa7547e34453"

#define MTC_BLE_MAX_PACKET 512
#define MTC_BLE_ATT_MTU    517
/* Deep enough to hold a full want_config burst (my_info + metadata + channel +
 * configs + several NodeInfo frames) while the app is reading. The config burst
 * is paced from the main loop and consumed reads are popped from the FIFO, so
 * larger meshes no longer require the whole NodeDB to fit in one immediate
 * queue spike. PSRAM-backed, so depth is cheap. */
#define MTC_BLE_QUEUE_N    64
#define MTC_BLE_CFG_PER_POLL 2
#define MTC_BLE_CFG_GAP_MS 20
#define MTC_BLE_NOTIFY_DELAY_MS 100

static NimBLEServer *g_ble_server;
static NimBLECharacteristic *g_ble_fromradio;
static NimBLECharacteristic *g_ble_toradio;
static NimBLECharacteristic *g_ble_fromnum;
/* set on the main loop, read in the NimBLE task (and vice versa) — volatile so
 * neither side caches a stale value in a register across the task boundary */
static volatile bool g_ble_ready;
static volatile bool g_ble_enabled;
static volatile bool g_ble_connected;

typedef struct {
    uint32_t num;
    uint16_t len;
    uint8_t  data[MTC_BLE_MAX_PACKET];
} ble_from_t;

static ble_from_t *g_ble_q;
static int g_ble_head, g_ble_count;
static uint32_t g_ble_next_num = 1;
static uint32_t g_ble_read_num = 1;
static bool g_ble_notify_pending;
static uint32_t g_ble_notify_value;
static uint32_t g_ble_notify_due_ms;

/* The NimBLE host runs in its own FreeRTOS task: GATT onRead/onWrite callbacks
 * fire there, while FromRadio packets are produced — and the SX1262 is driven —
 * on the main loop. Two rules keep that safe:
 *   1. The radio (shared SPI bus) is touched ONLY on the main loop. A phone's
 *      ToRadio write is copied into g_ble_to_q here and replayed from
 *      lz_mtc_ble_poll(), so handle_toradio()/lz_backend_send() never run in the
 *      BLE task alongside the TDM scheduler (that corrupts the SPI transaction).
 *   2. The FromRadio/ToRadio rings are shared across both tasks, so every index
 *      manipulation is done under g_ble_mux. NimBLE calls (notify/setValue) are
 *      made OUTSIDE the critical section — they must not run with IRQs masked. */
static portMUX_TYPE g_ble_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    uint16_t len;
    uint8_t  data[MTC_BLE_MAX_PACKET];
} ble_to_t;

static ble_to_t *g_ble_to_q;          /* phone -> radio, drained on the main loop */
static int g_ble_to_head, g_ble_to_count;

static uint32_t g_fromradio_id = 1;

enum {
    BLE_CFG_IDLE = 0,
    BLE_CFG_MY_INFO,
    BLE_CFG_METADATA,
    BLE_CFG_CHANNEL,
    BLE_CFG_CONFIG_DEVICE,
    BLE_CFG_CONFIG_LORA,
    BLE_CFG_NODES,
    BLE_CFG_COMPLETE,
};
static volatile bool g_ble_cfg_active;
static int g_ble_cfg_phase;
static int g_ble_cfg_node_index;
static uint32_t g_ble_cfg_nonce;
static uint32_t g_ble_cfg_next_ms;

extern "C" bool btStarted(void);   /* Arduino core: true while the BT controller is enabled */

extern "C" bool lz_mtc_ble_enabled(void) { return g_ble_enabled; }
extern "C" bool lz_mtc_ble_connected(void) { return g_ble_connected; }
extern "C" bool lz_mtc_any_active(void) { return g_companion || g_ble_connected; }

static void reset_fromradio_ids(void)
{
    g_fromradio_id = 1;
}

static uint32_t next_fromradio_id(void)
{
    uint32_t id = g_fromradio_id++;
    if(!g_fromradio_id) g_fromradio_id = 1;
    return id;
}

/* ---------- protobuf encode helpers ---------- */
static int pb_varint(uint8_t *b, uint64_t v)
{
    int n = 0;
    do { uint8_t x = v & 0x7F; v >>= 7; if(v) x |= 0x80; b[n++] = x; } while(v);
    return n;
}
static bool pb_room(int n, int cap, int add) { return add >= 0 && n >= 0 && cap >= 0 && n <= cap && add <= cap - n; }

static bool pb_put_varint(uint8_t *b, int cap, int *n, uint64_t v)
{
    uint8_t tmp[10]; int tn = pb_varint(tmp, v);
    if(!pb_room(*n, cap, tn)) return false;
    memcpy(b + *n, tmp, tn); *n += tn;
    return true;
}

static bool pb_put_key(uint8_t *b, int cap, int *n, int field, int wire)
{
    return pb_put_varint(b, cap, n, ((uint64_t)field << 3) | wire);
}

static bool pb_put_uint(uint8_t *b, int cap, int *n, int field, uint64_t v)
{
    return pb_put_key(b, cap, n, field, 0) && pb_put_varint(b, cap, n, v);
}

static bool pb_put_fixed32(uint8_t *b, int cap, int *n, int field, uint32_t v)
{
    if(!pb_put_key(b, cap, n, field, 5) || !pb_room(*n, cap, 4)) return false;
    b[(*n)++] = v & 0xFF; b[(*n)++] = (v >> 8) & 0xFF;
    b[(*n)++] = (v >> 16) & 0xFF; b[(*n)++] = (v >> 24) & 0xFF;
    return true;
}

static bool pb_put_float(uint8_t *b, int cap, int *n, int field, float f)
{
    uint32_t v; memcpy(&v, &f, 4);
    return pb_put_fixed32(b, cap, n, field, v);
}

static bool pb_put_bytes(uint8_t *b, int cap, int *n, int field, const uint8_t *data, int len)
{
    if(len < 0) return false;
    int before = *n;
    if(!pb_put_key(b, cap, n, field, 2) || !pb_put_varint(b, cap, n, (uint64_t)len) ||
       !pb_room(*n, cap, len)) {
        *n = before;
        return false;
    }
    if(len > 0) memcpy(b + *n, data, len);
    *n += len;
    return true;
}

static bool pb_put_str(uint8_t *b, int cap, int *n, int field, const char *s)
{
    if(!s) s = "";
    return pb_put_bytes(b, cap, n, field, (const uint8_t *)s, (int)strlen(s));
}

/* ---------- stream framing ---------- */
/* self-test capture: when capturing, frames go to a buffer instead of the wire */
static uint8_t *g_cap; static int g_caplen, g_capcap; static bool g_capturing, g_cap_overflow;

static bool cap_reserve(int need)
{
    const int CAP_MAX = 24 * 1024;
    if(need <= g_capcap) return true;
    if(need > CAP_MAX) { g_cap_overflow = true; return false; }
    int next_cap = g_capcap ? g_capcap : 1024;
    while(next_cap < need) next_cap *= 2;
    if(next_cap > CAP_MAX) next_cap = CAP_MAX;
    uint8_t *next = (uint8_t *)realloc(g_cap, next_cap);
    if(!next) { g_cap_overflow = true; return false; }
    g_cap = next;
    g_capcap = next_cap;
    return true;
}

static void send_usb_frame(const uint8_t *pb, int len)
{
    uint8_t hdr[4] = { 0x94, 0xC3, (uint8_t)(len >> 8), (uint8_t)(len & 0xFF) };
    if(g_capturing) {
        if(cap_reserve(g_caplen + 4 + len)) {
            memcpy(g_cap + g_caplen, hdr, 4); g_caplen += 4;
            memcpy(g_cap + g_caplen, pb, len); g_caplen += len;
        }
        return;
    }
    Serial.write(hdr, 4);
    Serial.write(pb, len);
    Serial.flush();
}

static void ble_set_fromnum(uint32_t v, bool notify)
{
    if(!g_ble_fromnum) return;
    uint8_t le[4] = {
        (uint8_t)(v & 0xFF),
        (uint8_t)((v >> 8) & 0xFF),
        (uint8_t)((v >> 16) & 0xFF),
        (uint8_t)((v >> 24) & 0xFF),
    };
    g_ble_fromnum->setValue(le, sizeof le);
    if(notify && g_ble_connected) g_ble_fromnum->notify(le, sizeof le);
}

static void ble_schedule_fromnum(uint32_t v)
{
    if(!g_ble_connected) return;
    uint32_t due = millis() + MTC_BLE_NOTIFY_DELAY_MS;
    taskENTER_CRITICAL(&g_ble_mux);
    g_ble_notify_value = v;
    g_ble_notify_due_ms = due;
    g_ble_notify_pending = true;
    taskEXIT_CRITICAL(&g_ble_mux);
}

static void ble_notify_poll(void)
{
    uint32_t value = 0;
    uint32_t now = millis();
    bool send = false;
    taskENTER_CRITICAL(&g_ble_mux);
    if(g_ble_notify_pending && (int32_t)(now - g_ble_notify_due_ms) >= 0) {
        value = g_ble_notify_value;
        g_ble_notify_pending = false;
        send = true;
    }
    taskEXIT_CRITICAL(&g_ble_mux);
    if(send) ble_set_fromnum(value, true);
}

/* rings are allocated once in lz_mtc_ble_begin() (single-threaded setup), never
 * lazily — concurrent lazy alloc from two tasks would race on the pointer. */
static bool ble_queue_ready(void) { return g_ble_q != NULL; }

static void ble_enqueue_fromradio(const uint8_t *pb, int len)
{
    if(!g_ble_ready || !g_ble_enabled || !g_ble_connected || len < 0) return;
    if(!ble_queue_ready()) return;
    if(len > MTC_BLE_MAX_PACKET) return;
    uint32_t num;
    taskENTER_CRITICAL(&g_ble_mux);
    if(g_ble_count >= MTC_BLE_QUEUE_N) {
        g_ble_head = (g_ble_head + 1) % MTC_BLE_QUEUE_N;
        g_ble_count--;
    }
    int idx = (g_ble_head + g_ble_count) % MTC_BLE_QUEUE_N;
    num = g_ble_next_num++;
    g_ble_q[idx].num = num;
    g_ble_q[idx].len = (uint16_t)len;
    if(len) memcpy(g_ble_q[idx].data, pb, len);
    g_ble_count++;
    taskEXIT_CRITICAL(&g_ble_mux);
    ble_schedule_fromnum(num);          /* coalesced NimBLE notify: outside the lock */
}

static void ble_prepare_fromradio_value(void)
{
    if(!g_ble_fromradio) return;
    static uint8_t tmp[MTC_BLE_MAX_PACKET];   /* onRead is single-task (BLE host) */
    int tlen = -1;
    taskENTER_CRITICAL(&g_ble_mux);
    if(g_ble_q) {
        int best = -1;
        for(int i = 0; i < g_ble_count; i++) {
            int idx = (g_ble_head + i) % MTC_BLE_QUEUE_N;
            if(g_ble_q[idx].num >= g_ble_read_num) { best = idx; break; }
        }
        if(best >= 0) {
            tlen = g_ble_q[best].len;
            if(tlen) memcpy(tmp, g_ble_q[best].data, tlen);
            g_ble_read_num = g_ble_q[best].num + 1;
        }
        while(g_ble_count > 0 && g_ble_q[g_ble_head].num < g_ble_read_num) {
            g_ble_head = (g_ble_head + 1) % MTC_BLE_QUEUE_N;
            g_ble_count--;
        }
    }
    taskEXIT_CRITICAL(&g_ble_mux);
    if(tlen < 0) g_ble_fromradio->setValue((const uint8_t *)NULL, 0);
    else         g_ble_fromradio->setValue(tmp, tlen);   /* setValue: outside the lock */
}

static void send_frame(const uint8_t *pb, int len)
{
    if(g_capturing || g_companion) send_usb_frame(pb, len);
    ble_enqueue_fromradio(pb, len);
}

/* wrap a sub-message as a FromRadio field and send it */
static void send_fromradio(int field, const uint8_t *sub, int sublen)
{
    if(sublen < 0 || sublen > MTC_BLE_MAX_PACKET - 16) return;
    uint8_t buf[MTC_BLE_MAX_PACKET]; int n = 0;
    if(!pb_put_uint(buf, sizeof buf, &n, 1, next_fromradio_id())) return;       /* FromRadio.id */
    if(!pb_put_bytes(buf, sizeof buf, &n, field, sub, sublen)) return;
    send_frame(buf, n);
}

static void send_fromradio_uint(int field, uint32_t v)
{
    uint8_t buf[MTC_BLE_MAX_PACKET]; int n = 0;
    if(!pb_put_uint(buf, sizeof buf, &n, 1, next_fromradio_id())) return;       /* FromRadio.id */
    if(!pb_put_uint(buf, sizeof buf, &n, field, v)) return;
    send_frame(buf, n);
}

/* ---------- FromRadio builders ---------- */
static void send_my_info(void)
{
    uint8_t m[16]; int n = 0;
    if(!pb_put_uint(m, sizeof m, &n, 1, lz_svc_identity()->num)) return;   /* my_node_num */
    send_fromradio(3, m, n);
}

static void send_node_info(const lz_node_rt *nd, uint32_t snr_unused)
{
    (void)snr_unused;
    /* User sub-message */
    uint8_t user[80]; int un = 0;
    if(!pb_put_str(user, sizeof user, &un, 1, nd->id)) return;
    if(!pb_put_str(user, sizeof user, &un, 2, nd->name)) return;
    if(!pb_put_str(user, sizeof user, &un, 3, nd->shortcode)) return;
    /* NodeInfo */
    uint8_t ni[140]; int n = 0;
    if(!pb_put_uint(ni, sizeof ni, &n, 1, nd->num)) return;                 /* num */
    if(!pb_put_bytes(ni, sizeof ni, &n, 2, user, un)) return;               /* user */
    if(nd->snr == nd->snr && !pb_put_float(ni, sizeof ni, &n, 4, nd->snr)) return;   /* snr (skip NaN) */
    if(!pb_put_fixed32(ni, sizeof ni, &n, 5, nd->last_heard)) return;       /* last_heard */
    send_fromradio(4, ni, n);
}

static void send_channel_primary(void)
{
    /* ChannelSettings: name="" (=> LongFast), psk=0x01 (default key) */
    uint8_t cs[16]; int csn = 0;
    uint8_t psk1 = 0x01;
    if(!pb_put_bytes(cs, sizeof cs, &csn, 2, &psk1, 1)) return;             /* psk */
    /* Channel: index=1(0), settings=2, role=3(PRIMARY=1) */
    uint8_t ch[40]; int n = 0;
    if(!pb_put_uint(ch, sizeof ch, &n, 1, 0)) return;                       /* index 0 */
    if(!pb_put_bytes(ch, sizeof ch, &n, 2, cs, csn)) return;                /* settings */
    if(!pb_put_uint(ch, sizeof ch, &n, 3, 1)) return;                       /* role PRIMARY */
    send_fromradio(10, ch, n);
}

/* DeviceMetadata (FromRadio.metadata=13): firmware, role, hw_model */
static void send_metadata(void)
{
    uint8_t m[48]; int n = 0;
    if(!pb_put_str(m, sizeof m, &n, 1, "0.6.0-limitlezz")) return;         /* firmware_version */
    if(!pb_put_uint(m, sizeof m, &n, 7, 0)) return;                        /* role = CLIENT */
    if(!pb_put_uint(m, sizeof m, &n, 9, 50)) return;                       /* hw_model = T_DECK */
    send_fromradio(13, m, n);
}

/* Config (FromRadio.config=5): device (empty = defaults) + lora (US / LongFast) */
static void send_config_device(void)
{
    uint8_t cfg[8]; int n = 0;
    if(!pb_put_bytes(cfg, sizeof cfg, &n, 1, NULL, 0)) return;   /* Config.device = {} */
    send_fromradio(5, cfg, n);
}

static void send_config_lora(void)
{
    uint8_t lc[16]; int ln = 0;
    if(!pb_put_uint(lc, sizeof lc, &ln, 1, 1)) return;                     /* use_preset = true */
    if(!pb_put_uint(lc, sizeof lc, &ln, 2, 0)) return;                     /* modem_preset = LONG_FAST */
    if(!pb_put_uint(lc, sizeof lc, &ln, 7, 1)) return;                     /* region = US */
    uint8_t c2[24]; int cn = 0;
    if(!pb_put_bytes(c2, sizeof c2, &cn, 6, lc, ln)) return;               /* Config.lora */
    send_fromradio(5, c2, cn);
}

static void send_configs(void)
{
    send_config_device();
    send_config_lora();
}

static void send_config_complete(uint32_t nonce)
{
    send_fromradio_uint(7, nonce);                    /* FromRadio.config_complete_id */
}

/* push a received mesh packet up to the app */
extern "C" void lz_mtc_forward(uint32_t from, uint32_t to, uint32_t id, uint32_t chan,
                    uint8_t portnum, const uint8_t *payload, int plen,
                    float snr, uint32_t rxtime, int hop_limit)
{
    if(!g_companion && !g_ble_connected) return;   /* forward over USB or BLE */
    /* Data sub-message: portnum=1(varint), payload=2(bytes) */
    uint8_t data[256]; int dn = 0;
    if(!pb_put_uint(data, sizeof data, &dn, 1, portnum)) return;
    if(plen > 0 && !pb_put_bytes(data, sizeof data, &dn, 2, payload, plen)) return;
    /* MeshPacket */
    uint8_t mp[300]; int n = 0;
    if(!pb_put_fixed32(mp, sizeof mp, &n, 1, from)) return;
    if(!pb_put_fixed32(mp, sizeof mp, &n, 2, to)) return;
    if(!pb_put_uint(mp, sizeof mp, &n, 3, chan)) return;
    if(!pb_put_bytes(mp, sizeof mp, &n, 4, data, dn)) return;              /* decoded */
    if(!pb_put_fixed32(mp, sizeof mp, &n, 6, id)) return;
    if(!pb_put_fixed32(mp, sizeof mp, &n, 7, rxtime)) return;
    if(!pb_put_float(mp, sizeof mp, &n, 8, snr)) return;
    if(!pb_put_uint(mp, sizeof mp, &n, 9, hop_limit)) return;
    send_fromradio(2, mp, n);                         /* FromRadio.packet */
}

/* ---------- ToRadio handling ---------- */
static void do_want_config(uint32_t nonce)
{
    /* the order the official app's PhoneAPI uses: my_info, metadata, channel,
     * config, then node DB, then config_complete */
    send_my_info();
    send_metadata();
    send_channel_primary();
    send_configs();
    const lz_node_rt *nodes;
    int nn = lz_svc_nodes(&nodes);
    for(int i = 0; i < nn; i++)
        if(nodes[i].net == LZ_NET_MT) send_node_info(&nodes[i], 0);
    send_config_complete(nonce);
}

static void ble_config_start(uint32_t nonce)
{
    if(!g_ble_ready || !g_ble_enabled || !g_ble_connected) return;
    taskENTER_CRITICAL(&g_ble_mux);
    g_ble_head = g_ble_count = 0;
    g_ble_read_num = g_ble_next_num;
    g_ble_notify_pending = false;
    g_ble_cfg_active = true;
    g_ble_cfg_phase = BLE_CFG_MY_INFO;
    g_ble_cfg_node_index = 0;
    g_ble_cfg_nonce = nonce;
    taskEXIT_CRITICAL(&g_ble_mux);
    g_ble_cfg_next_ms = millis() + MTC_BLE_CFG_GAP_MS;
}

static bool ble_config_send_one(void)
{
    if(!g_ble_cfg_active) return false;
    if(!g_ble_ready || !g_ble_enabled || !g_ble_connected) {
        g_ble_cfg_active = false;
        return false;
    }

    switch(g_ble_cfg_phase) {
        case BLE_CFG_MY_INFO:
            send_my_info();
            g_ble_cfg_phase = BLE_CFG_METADATA;
            return true;
        case BLE_CFG_METADATA:
            send_metadata();
            g_ble_cfg_phase = BLE_CFG_CHANNEL;
            return true;
        case BLE_CFG_CHANNEL:
            send_channel_primary();
            g_ble_cfg_phase = BLE_CFG_CONFIG_DEVICE;
            return true;
        case BLE_CFG_CONFIG_DEVICE:
            send_config_device();
            g_ble_cfg_phase = BLE_CFG_CONFIG_LORA;
            return true;
        case BLE_CFG_CONFIG_LORA:
            send_config_lora();
            g_ble_cfg_phase = BLE_CFG_NODES;
            return true;
        case BLE_CFG_NODES: {
            const lz_node_rt *nodes;
            int nn = lz_svc_nodes(&nodes);
            while(g_ble_cfg_node_index < nn) {
                const lz_node_rt *nd = &nodes[g_ble_cfg_node_index++];
                if(nd->net == LZ_NET_MT) {
                    send_node_info(nd, 0);
                    return true;
                }
            }
            g_ble_cfg_phase = BLE_CFG_COMPLETE;
            return true;
        }
        case BLE_CFG_COMPLETE:
            send_config_complete(g_ble_cfg_nonce);
            g_ble_cfg_active = false;
            g_ble_cfg_phase = BLE_CFG_IDLE;
            return true;
        default:
            g_ble_cfg_active = false;
            g_ble_cfg_phase = BLE_CFG_IDLE;
            return false;
    }
}

static void ble_config_poll(void)
{
    if(!g_ble_cfg_active) return;
    uint32_t now = millis();
    if((int32_t)(now - g_ble_cfg_next_ms) < 0) return;
    for(int i = 0; i < MTC_BLE_CFG_PER_POLL; i++)
        if(!ble_config_send_one()) break;
    g_ble_cfg_next_ms = millis() + MTC_BLE_CFG_GAP_MS;
}

static bool pb_read_varint(const uint8_t *b, int len, int *pos, uint64_t *out)
{
    uint64_t v = 0;
    int sh = 0;
    while(*pos < len && sh <= 63) {
        uint8_t x = b[(*pos)++];
        v |= (uint64_t)(x & 0x7F) << sh;
        if(!(x & 0x80)) {
            *out = v;
            return true;
        }
        sh += 7;
    }
    return false;
}

static bool pb_read_len(const uint8_t *b, int len, int *pos, int *out_len)
{
    uint64_t l = 0;
    if(!pb_read_varint(b, len, pos, &l)) return false;
    if(l > (uint64_t)(len - *pos)) return false;
    *out_len = (int)l;
    return true;
}

static bool pb_skip_field(const uint8_t *b, int len, int *pos, int wire)
{
    uint64_t ignored = 0;
    int l = 0;
    switch(wire) {
        case 0:
            return pb_read_varint(b, len, pos, &ignored);
        case 1:
            if(len - *pos < 8) return false;
            *pos += 8;
            return true;
        case 2:
            if(!pb_read_len(b, len, pos, &l)) return false;
            *pos += l;
            return true;
        case 5:
            if(len - *pos < 4) return false;
            *pos += 4;
            return true;
        default:
            return false;
    }
}

static uint32_t read_le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static bool decode_data(const uint8_t *data, int dlen, uint8_t *portnum,
                        const uint8_t **payload, int *payload_len)
{
    int q = 0;
    while(q < dlen) {
        uint64_t key = 0;
        if(!pb_read_varint(data, dlen, &q, &key)) return false;
        int field = (int)(key >> 3), wire = (int)(key & 7);
        if(field == 1 && wire == 0) {
            uint64_t v = 0;
            if(!pb_read_varint(data, dlen, &q, &v)) return false;
            *portnum = (uint8_t)v;
        } else if(field == 2 && wire == 2) {
            int l = 0;
            if(!pb_read_len(data, dlen, &q, &l)) return false;
            *payload = data + q;
            *payload_len = l;
            q += l;
        } else if(!pb_skip_field(data, dlen, &q, wire)) {
            return false;
        }
    }
    return true;
}

static bool decode_meshpacket(const uint8_t *mp, int mlen, uint32_t *to,
                              const uint8_t **data, int *dlen)
{
    int p = 0;
    while(p < mlen) {
        uint64_t key = 0;
        if(!pb_read_varint(mp, mlen, &p, &key)) return false;
        int field = (int)(key >> 3), wire = (int)(key & 7);
        if(field == 2 && wire == 5) {
            if(mlen - p < 4) return false;
            *to = read_le32(mp + p);
            p += 4;
        } else if(field == 4 && wire == 2) {
            int l = 0;
            if(!pb_read_len(mp, mlen, &p, &l)) return false;
            *data = mp + p;
            *dlen = l;
            p += l;
        } else if(!pb_skip_field(mp, mlen, &p, wire)) {
            return false;
        }
    }
    return true;
}

static void transmit_toradio_packet(const uint8_t *mp, int mlen)
{
    uint32_t to = LZ_BROADCAST;
    const uint8_t *data = NULL, *pl = NULL;
    int dlen = 0, pll = 0;
    uint8_t portnum = 1;
    if(!decode_meshpacket(mp, mlen, &to, &data, &dlen) || !data) return;
    if(!decode_data(data, dlen, &portnum, &pl, &pll)) return;
    if(!pl || pll <= 0) return;

    lz_mt_packet_t p2; memset(&p2, 0, sizeof p2);
    p2.to = to; p2.from = lz_svc_identity()->num;
    p2.id = (uint32_t)(esp_random() | 1);
    p2.hop_limit = 3; p2.hop_start = 3;
    p2.portnum = portnum;
    int c = pll < (int)sizeof p2.payload ? pll : (int)sizeof p2.payload;
    memcpy(p2.payload, pl, c); p2.plen = (uint8_t)c;
    lz_backend_send(&p2);
}

/* decode a ToRadio: extract a varint field, a packet, or want_config */
static void handle_toradio(const uint8_t *b, int len, bool from_ble)
{
    if(!b || len <= 0 || len > MTC_BLE_MAX_PACKET) return;
    int pos = 0;
    while(pos < len) {
        uint64_t key = 0;
        if(!pb_read_varint(b, len, &pos, &key)) break;
        int field = (int)(key >> 3), wire = (int)(key & 7);
        if(wire == 0) {                               /* varint */
            uint64_t v = 0;
            if(!pb_read_varint(b, len, &pos, &v)) break;
            if(field == 3) {                              /* want_config_id */
                if(from_ble) ble_config_start((uint32_t)v);
                else         do_want_config((uint32_t)v);
            }
        } else if(wire == 2) {                        /* length-delimited */
            int l = 0;
            if(!pb_read_len(b, len, &pos, &l)) break;
            if(field == 1) transmit_toradio_packet(b + pos, l);  /* ToRadio.packet -> transmit */
            pos += l;
        } else if(!pb_skip_field(b, len, &pos, wire)) break;
    }
}

/* ---------- BLE GATT transport ---------- */
class MtcBleServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override
    {
        taskENTER_CRITICAL(&g_ble_mux);
        g_ble_head = g_ble_count = 0;
        g_ble_to_head = g_ble_to_count = 0;
        g_ble_next_num = 1;
        g_ble_read_num = 1;
        g_ble_notify_pending = false;
        g_ble_cfg_active = false;
        g_ble_cfg_phase = BLE_CFG_IDLE;
        g_ble_connected = true;
        taskEXIT_CRITICAL(&g_ble_mux);
        reset_fromradio_ids();
        if(server) {
            server->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 180);
            server->setDataLen(connInfo.getConnHandle(), 251);
        }
        ble_set_fromnum(g_ble_next_num ? g_ble_next_num - 1 : 0, false);
    }
    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override
    {
        (void)server; (void)connInfo; (void)reason;
        taskENTER_CRITICAL(&g_ble_mux);
        g_ble_connected = false;
        g_ble_head = g_ble_count = 0;
        g_ble_to_head = g_ble_to_count = 0;
        g_ble_notify_pending = false;
        g_ble_cfg_active = false;
        g_ble_cfg_phase = BLE_CFG_IDLE;
        taskEXIT_CRITICAL(&g_ble_mux);
        if(g_ble_enabled) NimBLEDevice::startAdvertising();
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo &connInfo) override
    {
        (void)mtu; (void)connInfo;
    }
};

class MtcBleToRadioCallbacks : public NimBLECharacteristicCallbacks {
    /* Runs in the NimBLE host task. Copy the frame into the ToRadio ring and let
     * lz_mtc_ble_poll() replay it on the main loop — handle_toradio() drives the
     * radio, which must never be touched from this task. */
    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &connInfo) override
    {
        (void)connInfo;
        std::string v = chr->getValue();
        if(v.empty() || v.size() > MTC_BLE_MAX_PACKET || !g_ble_to_q) return;
        taskENTER_CRITICAL(&g_ble_mux);
        if(g_ble_to_count >= MTC_BLE_QUEUE_N) {     /* drop oldest on overflow */
            g_ble_to_head = (g_ble_to_head + 1) % MTC_BLE_QUEUE_N;
            g_ble_to_count--;
        }
        int idx = (g_ble_to_head + g_ble_to_count) % MTC_BLE_QUEUE_N;
        g_ble_to_q[idx].len = (uint16_t)v.size();
        memcpy(g_ble_to_q[idx].data, v.data(), v.size());
        g_ble_to_count++;
        taskEXIT_CRITICAL(&g_ble_mux);
    }
};

class MtcBleFromRadioCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &connInfo) override
    {
        (void)chr; (void)connInfo;
        ble_prepare_fromradio_value();
    }
};

class MtcBleFromNumCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &connInfo) override
    {
        (void)chr; (void)connInfo;
        ble_set_fromnum(g_ble_next_num ? g_ble_next_num - 1 : 0, false);
    }
    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &connInfo) override
    {
        (void)connInfo;
        std::string v = chr->getValue();
        if(v.size() >= 4) {
            uint32_t want = (uint32_t)(uint8_t)v[0]
                          | ((uint32_t)(uint8_t)v[1] << 8)
                          | ((uint32_t)(uint8_t)v[2] << 16)
                          | ((uint32_t)(uint8_t)v[3] << 24);
            taskENTER_CRITICAL(&g_ble_mux);
            g_ble_read_num = want;
            taskEXIT_CRITICAL(&g_ble_mux);
        }
    }
};

static MtcBleServerCallbacks g_ble_server_cb;
static MtcBleToRadioCallbacks g_ble_to_cb;
static MtcBleFromRadioCallbacks g_ble_from_cb;
static MtcBleFromNumCallbacks g_ble_num_cb;

extern "C" void lz_mtc_ble_begin(void)
{
    if(g_ble_ready) return;
    const lz_identity_t *id = lz_svc_identity();
    char name[32];
    snprintf(name, sizeof name, "Limitlezz-%s", id->short_name[0] ? id->short_name : "TDeck");

    if(!NimBLEDevice::init(name)) return;   /* controller init failed: leave g_ble_ready false */
    NimBLEDevice::setMTU(MTC_BLE_ATT_MTU);
    g_ble_server = NimBLEDevice::createServer();
    g_ble_server->setCallbacks(&g_ble_server_cb, false);

    NimBLEService *svc = g_ble_server->createService(MTC_BLE_SERVICE_UUID);
    g_ble_fromradio = svc->createCharacteristic(MTC_BLE_FROM_UUID,
                                                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
                                                MTC_BLE_MAX_PACKET);
    g_ble_toradio = svc->createCharacteristic(MTC_BLE_TO_UUID,
                                              NIMBLE_PROPERTY::WRITE,
                                              MTC_BLE_MAX_PACKET);
    g_ble_fromnum = svc->createCharacteristic(MTC_BLE_FROMNUM_UUID,
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE,
                                              4);
    g_ble_fromradio->setCallbacks(&g_ble_from_cb);
    g_ble_toradio->setCallbacks(&g_ble_to_cb);
    g_ble_fromnum->setCallbacks(&g_ble_num_cb);
    ble_set_fromnum(0, false);

    /* Allocate both rings once (PSRAM preferred — ~66 KB total — internal heap as
     * a fallback). They persist across BLE on/off toggles (the controller is
     * deinited but the PSRAM rings are reused), so only allocate if not present. */
    if(!g_ble_q) {
        g_ble_q = (ble_from_t *)heap_caps_calloc(MTC_BLE_QUEUE_N, sizeof(ble_from_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(!g_ble_q) g_ble_q = (ble_from_t *)calloc(MTC_BLE_QUEUE_N, sizeof(ble_from_t));
    }
    if(!g_ble_to_q) {
        g_ble_to_q = (ble_to_t *)heap_caps_calloc(MTC_BLE_QUEUE_N, sizeof(ble_to_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(!g_ble_to_q) g_ble_to_q = (ble_to_t *)calloc(MTC_BLE_QUEUE_N, sizeof(ble_to_t));
    }

    g_ble_server->start();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setName(name);
    adv->addServiceUUID(MTC_BLE_SERVICE_UUID);
    adv->enableScanResponse(true);
    g_ble_ready = true;
}

extern "C" void lz_mtc_ble_set_enabled(bool on)
{
    if(on) {
        /* BLE controller and the WiFi driver both need internal DMA RAM and can't
         * both be resident — free WiFi first so the controller can init. */
        if(lz_wifi_enabled()) lz_wifi_set_enabled(false);
        if(g_companion) g_companion = false;   /* one external app bridge at a time */
        if(!g_ble_ready) lz_mtc_ble_begin();
        if(!g_ble_ready) {                     /* controller init failed (out of RAM): */
            lz_wifi_set_enabled(true);         /* don't strand the user — restore WiFi */
            return;
        }
        g_ble_enabled = true;
        NimBLEDevice::startAdvertising();
    } else {
        g_ble_enabled = false;
        if(!g_ble_ready) return;
        /* Graceful GAP teardown first: stop advertising, drop the phone, and let
         * the host task run its disconnect callbacks... */
        NimBLEDevice::stopAdvertising();
        if(g_ble_server && g_ble_connected) {
            std::vector<uint16_t> peers = g_ble_server->getPeerDevices();
            for(size_t i = 0; i < peers.size(); i++)
                g_ble_server->disconnect(peers[i]);
        }
        taskENTER_CRITICAL(&g_ble_mux);
        g_ble_connected = false;
        g_ble_head = g_ble_count = 0;
        g_ble_to_head = g_ble_to_count = 0;
        g_ble_notify_pending = false;
        g_ble_cfg_active = false;
        g_ble_cfg_phase = BLE_CFG_IDLE;
        taskEXIT_CRITICAL(&g_ble_mux);
        vTaskDelay(pdMS_TO_TICKS(100));        /* ...let disconnect/host events fire */
        /* ...then fully deinit: NimBLEDevice::deinit(true) runs
         * esp_bt_controller_disable()+_deinit(), returning the controller's
         * internal DMA RAM to the heap so WiFi can init again. It does NOT call
         * the one-way esp_bt_controller_mem_release(), so BLE can be re-enabled. */
        NimBLEDevice::deinit(true);
        for(int i = 0; i < 50 && btStarted(); i++) vTaskDelay(pdMS_TO_TICKS(10));
        /* server/characteristics are deleted by deinit(true); null our dangling
         * pointers and clear the ready flag so a later begin() rebuilds cleanly.
         * The PSRAM rings are kept and reused. */
        g_ble_server = NULL;
        g_ble_fromradio = g_ble_toradio = g_ble_fromnum = NULL;
        g_ble_ready = false;
    }
}

extern "C" void lz_mtc_ble_poll(void)
{
    /* Drain phone->radio writes here, on the main loop, so the SX1262 is only
     * ever driven from one task (never the BLE host task alongside the TDM
     * scheduler). Copy each frame out under the lock, then transmit unlocked. */
    if(g_ble_to_q) {
        static uint8_t frame[MTC_BLE_MAX_PACKET];   /* main-loop only: single-task */
        for(;;) {
            int flen = -1;
            taskENTER_CRITICAL(&g_ble_mux);
            if(g_ble_to_count > 0) {
                int idx = g_ble_to_head;
                flen = g_ble_to_q[idx].len;
                if(flen) memcpy(frame, g_ble_to_q[idx].data, flen);
                g_ble_to_head = (g_ble_to_head + 1) % MTC_BLE_QUEUE_N;
                g_ble_to_count--;
            }
            taskEXIT_CRITICAL(&g_ble_mux);
            if(flen < 0) break;
            if(flen > 0) handle_toradio(frame, flen, true);
        }
    }

    ble_config_poll();
    ble_notify_poll();

    if(g_ble_enabled && !g_ble_connected && g_ble_ready && !NimBLEDevice::getAdvertising()->isAdvertising())
        NimBLEDevice::startAdvertising();
}

extern "C" int lz_mtc_ble_status(char *buf, int n)
{
    const char *state = !g_ble_enabled ? "off"
                      : g_ble_connected ? "connected"
                      : "advertising";
    uint32_t latest;
    int from_q, to_q;
    bool syncing;
    taskENTER_CRITICAL(&g_ble_mux);
    latest = g_ble_next_num ? g_ble_next_num - 1 : 0;
    from_q = g_ble_count;
    to_q = g_ble_to_count;
    syncing = g_ble_cfg_active;
    taskEXIT_CRITICAL(&g_ble_mux);
    return snprintf(buf, n, "BLE companion: %s | from_q=%d to_q=%d sync=%s latest=%lu service=%s",
                    state, from_q, to_q, syncing ? "config" : "idle",
                    (unsigned long)latest, MTC_BLE_SERVICE_UUID);
}

extern "C" int lz_mtc_ble_selftest(char *buf, int n)
{
    /* Don't lazily init here: bringing up the BT controller would steal the
     * internal RAM WiFi needs. Require the companion to be on already. */
    if(!g_ble_ready)
        return snprintf(buf, n, "BLE mailbox selftest: enable BLE first (companion ble on)");
    if(g_ble_connected || g_ble_count)
        return snprintf(buf, n, "BLE mailbox selftest skipped (active connection/queue)");
    bool old_enabled = g_ble_enabled;
    bool old_connected = g_ble_connected;
    int old_head = g_ble_head;
    uint32_t old_next = g_ble_next_num;
    uint32_t old_read = g_ble_read_num;
    bool old_notify_pending = g_ble_notify_pending;
    uint32_t old_notify_value = g_ble_notify_value;
    uint32_t old_notify_due = g_ble_notify_due_ms;

    uint8_t sample[2] = { 0x38, 0x2A };   /* FromRadio.config_complete_id = 42 */
    g_ble_enabled = true;
    g_ble_connected = true;
    g_ble_head = g_ble_count = 0;
    g_ble_read_num = g_ble_next_num;
    ble_enqueue_fromradio(sample, sizeof sample);
    bool ok = g_ble_count == 1 && g_ble_q && g_ble_q[0].len == sizeof sample &&
              memcmp(g_ble_q[0].data, sample, sizeof sample) == 0 &&
              g_ble_q[0].num == old_next;

    g_ble_enabled = old_enabled;
    g_ble_connected = old_connected;
    g_ble_head = old_head;
    g_ble_count = 0;
    g_ble_next_num = old_next;
    g_ble_read_num = old_read;
    g_ble_notify_pending = old_notify_pending;
    g_ble_notify_value = old_notify_value;
    g_ble_notify_due_ms = old_notify_due;
    ble_set_fromnum(g_ble_next_num ? g_ble_next_num - 1 : 0, false);

    return snprintf(buf, n, "BLE mailbox enqueue/fromnum -> %s", ok ? "PASS" : "FAIL");
}

/* ---------- serial frame reader ---------- */
extern "C" void lz_mtc_poll(void)
{
    static uint8_t buf[600];
    static int state = 0, want = 0, idx = 0;
    while(Serial.available()) {
        uint8_t c = (uint8_t)Serial.read();
        switch(state) {
            case 0: state = (c == 0x94) ? 1 : 0; break;
            case 1: state = (c == 0xC3) ? 2 : (c == 0x94 ? 1 : 0); break;
            case 2: want = c << 8; state = 3; break;
            case 3: want |= c; idx = 0;
                    state = (want > 0 && want <= (int)sizeof buf) ? 4 : 0; break;
            case 4: buf[idx++] = c;
                    if(idx >= want) { handle_toradio(buf, want, false); state = 0; }
                    break;
        }
    }
}

/* ---------- mode control + self-test ---------- */
extern "C" void lz_mtc_set_active(bool on)
{
    if(on && g_ble_enabled) lz_mtc_ble_set_enabled(false);
    g_companion = on;
    if(on) {
        reset_fromradio_ids();
        send_fromradio_uint(8, 1);          /* FromRadio{rebooted=true} */
    }
}

/* loopback self-test: synthesize a want_config_id ToRadio and confirm the
 * emitted FromRadio stream is well-framed and ends with config_complete_id.
 * Captures send_frame output to a buffer instead of the wire. */
extern "C" int lz_mtc_selftest(char *out, int n)
{
    /* temporarily capture frames */
    g_cap = NULL; g_caplen = 0; g_capcap = 0; g_cap_overflow = false; g_capturing = true;
    bool was = g_companion; g_companion = true;
    reset_fromradio_ids();
    do_want_config(0x1234abcd);
    g_companion = was; g_capturing = false;

    /* walk frames, confirm the full handshake content the app needs is present */
    int p = 0, frames = 0; uint32_t nonce = 0;
    bool my_info = false, meta = false, cfg = false, chan = false, complete = false;
    while(p + 4 <= g_caplen) {
        if(g_cap[p] != 0x94 || g_cap[p+1] != 0xC3) break;
        int len = (g_cap[p+2] << 8) | g_cap[p+3]; p += 4;
        if(p + len > g_caplen) break;
        const uint8_t *f = g_cap + p;
        int field = 0;                     /* FromRadio payload_variant field */
        int q = 0;
        while(q < len) {
            uint64_t key = 0; int sh = 0;
            while(q < len) { uint8_t x = f[q++]; key |= (uint64_t)(x & 0x7F) << sh; if(!(x & 0x80)) break; sh += 7; }
            int ff = key >> 3, wire = key & 7;
            if(wire == 0) {
                uint64_t v = 0; sh = 0;
                while(q < len) { uint8_t x = f[q++]; v |= (uint64_t)(x & 0x7F) << sh; if(!(x & 0x80)) break; sh += 7; }
                if(ff == 7) { field = ff; nonce = (uint32_t)v; }
            } else if(wire == 2) {
                uint64_t l = 0; sh = 0;
                while(q < len) { uint8_t x = f[q++]; l |= (uint64_t)(x & 0x7F) << sh; if(!(x & 0x80)) break; sh += 7; }
                if(ff != 1) field = ff;
                if((uint64_t)q + l > (uint64_t)len) { q = len; break; }
                q += (int)l;
            } else if(wire == 5) q += 4;
            else if(wire == 1) q += 8;
            else break;
        }
        if(field == 3)  my_info = true;
        if(field == 13) meta = true;
        if(field == 5)  cfg = true;
        if(field == 10) chan = true;
        if(field == 7)  complete = true;
        frames++; p += len;
    }
    bool ok = !g_cap_overflow && my_info && meta && cfg && chan && complete && nonce == 0x1234abcd;
    int written = snprintf(out, n, "%d frames | my_info=%d metadata=%d config=%d channel=%d complete=%d nonce=%08x%s -> %s",
                           frames, my_info, meta, cfg, chan, complete, (unsigned)nonce,
                           g_cap_overflow ? " overflow" : "", ok ? "PASS" : "FAIL");
    free(g_cap);
    g_cap = NULL; g_caplen = 0; g_capcap = 0;
    return written;
}

#endif /* LZ_TARGET_TDECK */
