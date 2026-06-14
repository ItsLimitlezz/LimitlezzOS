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
#include <stdlib.h>
#include "services/mesh.h"
#include "services/mtproto.h"

static bool g_companion;

extern "C" bool lz_mtc_active(void) { return g_companion; }

/* ---------- protobuf encode helpers ---------- */
static int pb_varint(uint8_t *b, uint64_t v)
{
    int n = 0;
    do { uint8_t x = v & 0x7F; v >>= 7; if(v) x |= 0x80; b[n++] = x; } while(v);
    return n;
}
static int pb_key(uint8_t *b, int field, int wire) { return pb_varint(b, ((uint64_t)field << 3) | wire); }
static int pb_uint(uint8_t *b, int field, uint64_t v) { int n = pb_key(b, field, 0); return n + pb_varint(b + n, v); }
static int pb_fixed32(uint8_t *b, int field, uint32_t v)
{
    int n = pb_key(b, field, 5);
    b[n++] = v & 0xFF; b[n++] = (v >> 8) & 0xFF; b[n++] = (v >> 16) & 0xFF; b[n++] = (v >> 24) & 0xFF;
    return n;
}
static int pb_float(uint8_t *b, int field, float f) { uint32_t v; memcpy(&v, &f, 4); return pb_fixed32(b, field, v); }
static int pb_bytes(uint8_t *b, int field, const uint8_t *data, int len)
{
    int n = pb_key(b, field, 2);
    n += pb_varint(b + n, len);
    memcpy(b + n, data, len); n += len;
    return n;
}
static int pb_str(uint8_t *b, int field, const char *s) { return pb_bytes(b, field, (const uint8_t *)s, strlen(s)); }

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

static void send_frame(const uint8_t *pb, int len)
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

/* wrap a sub-message as a FromRadio field and send it */
static void send_fromradio(int field, const uint8_t *sub, int sublen)
{
    uint8_t buf[300]; int n = 0;
    n += pb_bytes(buf + n, field, sub, sublen);
    send_frame(buf, n);
}

/* ---------- FromRadio builders ---------- */
static void send_my_info(void)
{
    uint8_t m[16]; int n = 0;
    n += pb_uint(m + n, 1, lz_svc_identity()->num);   /* my_node_num */
    send_fromradio(3, m, n);
}

static void send_node_info(const lz_node_rt *nd, uint32_t snr_unused)
{
    (void)snr_unused;
    /* User sub-message */
    uint8_t user[80]; int un = 0;
    un += pb_str(user + un, 1, nd->id);
    un += pb_str(user + un, 2, nd->name);
    un += pb_str(user + un, 3, nd->shortcode);
    /* NodeInfo */
    uint8_t ni[140]; int n = 0;
    n += pb_uint(ni + n, 1, nd->num);                 /* num */
    n += pb_bytes(ni + n, 2, user, un);               /* user */
    if(nd->snr == nd->snr) n += pb_float(ni + n, 4, nd->snr);   /* snr (skip NaN) */
    n += pb_fixed32(ni + n, 5, nd->last_heard);       /* last_heard */
    send_fromradio(4, ni, n);
}

static void send_channel_primary(void)
{
    /* ChannelSettings: name="" (=> LongFast), psk=0x01 (default key) */
    uint8_t cs[16]; int csn = 0;
    uint8_t psk1 = 0x01;
    csn += pb_bytes(cs + csn, 2, &psk1, 1);           /* psk */
    /* Channel: index=1(0), settings=2, role=3(PRIMARY=1) */
    uint8_t ch[40]; int n = 0;
    n += pb_uint(ch + n, 1, 0);                       /* index 0 */
    n += pb_bytes(ch + n, 2, cs, csn);                /* settings */
    n += pb_uint(ch + n, 3, 1);                       /* role PRIMARY */
    send_fromradio(10, ch, n);
}

/* DeviceMetadata (FromRadio.metadata=13): firmware, role, hw_model */
static void send_metadata(void)
{
    uint8_t m[48]; int n = 0;
    n += pb_str(m + n, 1, "0.44.0-limitlezz");        /* firmware_version */
    n += pb_uint(m + n, 7, 0);                        /* role = CLIENT */
    n += pb_uint(m + n, 9, 50);                       /* hw_model = T_DECK */
    send_fromradio(13, m, n);
}

/* Config (FromRadio.config=5): device (empty = defaults) + lora (US / LongFast) */
static void send_configs(void)
{
    uint8_t cfg[8]; int n = pb_bytes(cfg, 1, NULL, 0);   /* Config.device = {} */
    send_fromradio(5, cfg, n);

    uint8_t lc[16]; int ln = 0;
    ln += pb_uint(lc + ln, 1, 1);                     /* use_preset = true */
    ln += pb_uint(lc + ln, 2, 0);                     /* modem_preset = LONG_FAST */
    ln += pb_uint(lc + ln, 7, 1);                     /* region = US */
    uint8_t c2[24]; int cn = pb_bytes(c2, 6, lc, ln); /* Config.lora */
    send_fromradio(5, c2, cn);
}

static void send_config_complete(uint32_t nonce)
{
    uint8_t m[8]; int n = pb_uint(m, 7, nonce);       /* FromRadio.config_complete_id */
    send_frame(m, n);
}

/* push a received mesh packet up to the app */
extern "C" void lz_mtc_forward(uint32_t from, uint32_t to, uint32_t id, uint32_t chan,
                    uint8_t portnum, const uint8_t *payload, int plen,
                    float snr, uint32_t rxtime, int hop_limit)
{
    if(!g_companion) return;
    /* Data sub-message: portnum=1(varint), payload=2(bytes) */
    uint8_t data[256]; int dn = 0;
    dn += pb_uint(data + dn, 1, portnum);
    if(plen > 0) dn += pb_bytes(data + dn, 2, payload, plen);
    /* MeshPacket */
    uint8_t mp[300]; int n = 0;
    n += pb_fixed32(mp + n, 1, from);
    n += pb_fixed32(mp + n, 2, to);
    n += pb_uint(mp + n, 3, chan);
    n += pb_bytes(mp + n, 4, data, dn);               /* decoded */
    n += pb_fixed32(mp + n, 6, id);
    n += pb_fixed32(mp + n, 7, rxtime);
    n += pb_float(mp + n, 8, snr);
    n += pb_uint(mp + n, 9, hop_limit);
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

/* decode a ToRadio: extract a varint field, a packet, or want_config */
static void handle_toradio(const uint8_t *b, int len)
{
    int pos = 0;
    while(pos < len) {
        uint64_t key = 0; int sh = 0;
        while(pos < len) { uint8_t x = b[pos++]; key |= (uint64_t)(x & 0x7F) << sh; if(!(x & 0x80)) break; sh += 7; }
        int field = key >> 3, wire = key & 7;
        if(wire == 0) {                               /* varint */
            uint64_t v = 0; sh = 0;
            while(pos < len) { uint8_t x = b[pos++]; v |= (uint64_t)(x & 0x7F) << sh; if(!(x & 0x80)) break; sh += 7; }
            if(field == 3) do_want_config((uint32_t)v);   /* want_config_id */
        } else if(wire == 2) {                        /* length-delimited */
            uint64_t l = 0; sh = 0;
            while(pos < len) { uint8_t x = b[pos++]; l |= (uint64_t)(x & 0x7F) << sh; if(!(x & 0x80)) break; sh += 7; }
            if((uint64_t)pos + l > (uint64_t)len) break;
            if(field == 1) {                          /* ToRadio.packet -> transmit */
                /* find decoded Data (field 4) + to (field 2) inside the MeshPacket */
                const uint8_t *mp = b + pos; int mlen = (int)l;
                uint32_t to = LZ_BROADCAST; const uint8_t *data = NULL; int dlen = 0;
                int p = 0;
                while(p < mlen) {
                    uint64_t k2 = 0; int s2 = 0;
                    while(p < mlen) { uint8_t x = mp[p++]; k2 |= (uint64_t)(x & 0x7F) << s2; if(!(x & 0x80)) break; s2 += 7; }
                    int f2 = k2 >> 3, w2 = k2 & 7;
                    if(w2 == 5) { if(f2 == 2) to = mp[p] | (mp[p+1]<<8) | (mp[p+2]<<16) | ((uint32_t)mp[p+3]<<24); p += 4; }
                    else if(w2 == 0) { while(p < mlen && (mp[p++] & 0x80)); }
                    else if(w2 == 2) {
                        uint64_t l2 = 0; int s3 = 0;
                        while(p < mlen) { uint8_t x = mp[p++]; l2 |= (uint64_t)(x & 0x7F) << s3; if(!(x & 0x80)) break; s3 += 7; }
                        if(f2 == 4) { data = mp + p; dlen = (int)l2; }
                        p += (int)l2;
                    } else break;
                }
                if(data) {
                    /* pull portnum (1) + payload (2) out of Data, send via radio */
                    uint8_t portnum = 1; const uint8_t *pl = NULL; int pll = 0;
                    int q = 0;
                    while(q < dlen) {
                        uint64_t k3 = 0; int s4 = 0;
                        while(q < dlen) { uint8_t x = data[q++]; k3 |= (uint64_t)(x & 0x7F) << s4; if(!(x & 0x80)) break; s4 += 7; }
                        int f3 = k3 >> 3, w3 = k3 & 7;
                        if(w3 == 0) { uint64_t v = 0; int s5 = 0; while(q < dlen){uint8_t x=data[q++]; v|=(uint64_t)(x&0x7F)<<s5; if(!(x&0x80))break; s5+=7;} if(f3==1) portnum=(uint8_t)v; }
                        else if(w3 == 2) { uint64_t l3=0; int s6=0; while(q<dlen){uint8_t x=data[q++]; l3|=(uint64_t)(x&0x7F)<<s6; if(!(x&0x80))break; s6+=7;} if(f3==2){pl=data+q; pll=(int)l3;} q+=(int)l3; }
                        else break;
                    }
                    if(pl && pll > 0) {
                        lz_mt_packet_t p2; memset(&p2, 0, sizeof p2);
                        p2.to = to; p2.from = lz_svc_identity()->num;
                        p2.id = (uint32_t)(esp_random() | 1);
                        p2.hop_limit = 3; p2.hop_start = 3;
                        p2.portnum = portnum;
                        int c = pll < (int)sizeof p2.payload ? pll : (int)sizeof p2.payload;
                        memcpy(p2.payload, pl, c); p2.plen = (uint8_t)c;
                        lz_backend_send(&p2);
                    }
                }
            }
            pos += (int)l;
        } else if(wire == 5) pos += 4;
        else if(wire == 1) pos += 8;
        else break;
    }
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
                    if(idx >= want) { handle_toradio(buf, want); state = 0; }
                    break;
        }
    }
}

/* ---------- mode control + self-test ---------- */
extern "C" void lz_mtc_set_active(bool on)
{
    g_companion = on;
    if(on) {
        uint8_t reb[2] = { 0x40, 0x01 };   /* FromRadio{rebooted=true} (field 8 varint) */
        send_frame(reb, sizeof reb);
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
        int field = f[0] >> 3;             /* FromRadio payload_variant field */
        if(field == 3)  my_info = true;
        if(field == 13) meta = true;
        if(field == 5)  cfg = true;
        if(field == 10) chan = true;
        if(field == 7) { complete = true;
            uint64_t v = 0; int sh = 0, q = 1;
            while(q < len) { uint8_t x = f[q++]; v |= (uint64_t)(x & 0x7F) << sh; if(!(x & 0x80)) break; sh += 7; }
            nonce = (uint32_t)v;
        }
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
