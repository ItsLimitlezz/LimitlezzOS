/**
 * Meshtastic wire protocol codec.
 *
 * Values below mirror the Meshtastic firmware (github.com/meshtastic/firmware)
 * so LimitlezzOS speaks the default LongFast channel with stock devices.
 * Each constant cites the firmware source it came from; verify against the
 * pinned firmware version before a release.
 *
 * Crypto note: this uses the well-known DEFAULT channel PSK, which is public.
 * The default Meshtastic channel is "encrypted" only against casual sniffing,
 * exactly as on stock firmware — not a security boundary. Private channels
 * (user-supplied PSK) are set via mt_set_channel().
 */
#include "mtproto.h"
#include <string.h>
#include <stdio.h>

#if defined(LZ_TARGET_TDECK)
  #include "mbedtls/aes.h"   /* ESP-IDF / Arluino-ESP32 bundled mbedTLS */
  #define MT_HAVE_AES 1
#else
  #include "aes_min.h"       /* tiny portable AES for the simulator */
  #define MT_HAVE_AES 1
#endif

/* ---- channel config ----
 * Default primary channel: name "LongFast" with the 1-byte default key (0x01),
 * which the firmware expands to this fixed 16-byte AES-128 key
 * (Channels.cpp / defaultpsk: the "AQ==" default). */
static const uint8_t DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

static char    g_chan_name[16] = "LongFast";
static uint8_t g_psk[32]       = {0};
static int     g_psk_len       = 16;

void mt_set_channel(const char *name, const uint8_t *psk, int psk_len)
{
    snprintf(g_chan_name, sizeof g_chan_name, "%s", name ? name : "");
    if(psk && (psk_len == 16 || psk_len == 32)) {
        memcpy(g_psk, psk, psk_len);
        g_psk_len = psk_len;
    } else {
        memcpy(g_psk, DEFAULT_PSK, 16);
        g_psk_len = 16;
    }
}

static void ensure_channel(void)
{
    static bool init;
    if(!init) { memcpy(g_psk, DEFAULT_PSK, 16); g_psk_len = 16; init = true; }
}

/* Channels::generateHash (Channels.cpp): xor-fold the channel name, xor-fold
 * the expanded PSK, then xor the two. May legitimately be 0. The firmware
 * hashes the *effective* name ("LongFast" for the default preset), not "".
 * Verified: xorHash("LongFast")=0x0A ^ xorHash(defaultPSK)=0x02 = 0x08. */
uint8_t mt_channel_hash(void)
{
    ensure_channel();
    uint8_t h = 0;
    for(const char *p = g_chan_name; *p; p++) h ^= (uint8_t)*p;
    for(int i = 0; i < g_psk_len; i++) h ^= g_psk[i];
    return h;
}

/* ---- header ----
 * Meshtastic PacketHeader (RadioInterface.h), little-endian on the wire:
 *   [0..3]  to (u32)      [4..7]  from (u32)     [8..11] id (u32)
 *   [12]    flags         [13]    channel hash
 *   [14]    next_hop      [15]    relay_node
 * flags bits: 0-2 hop_limit, 3 want_ack, 4 via_mqtt, 5-7 hop_start. */
static void put_u32(uint8_t *b, uint32_t v)
{
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; b[2] = (v >> 16) & 0xFF; b[3] = (v >> 24) & 0xFF;
}
static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

int mt_header_write(uint8_t *buf, const mt_frame_t *f)
{
    put_u32(buf + 0, f->to);
    put_u32(buf + 4, f->from);
    put_u32(buf + 8, f->id);
    uint8_t flags = (f->hop_limit & 0x07);
    if(f->want_ack) flags |= 0x08;
    if(f->via_mqtt) flags |= 0x10;
    flags |= (f->hop_start & 0x07) << 5;
    buf[12] = flags;
    buf[13] = f->channel_hash;
    buf[14] = f->next_hop;
    buf[15] = f->relay_node;
    return MT_HEADER_LEN;
}

bool mt_header_read(const uint8_t *buf, int len, mt_frame_t *f)
{
    if(len < MT_HEADER_LEN) return false;
    memset(f, 0, sizeof *f);
    f->to   = get_u32(buf + 0);
    f->from = get_u32(buf + 4);
    f->id   = get_u32(buf + 8);
    uint8_t flags = buf[12];
    f->flags = flags;
    f->hop_limit = flags & 0x07;
    f->want_ack  = (flags & 0x08) != 0;
    f->via_mqtt  = (flags & 0x10) != 0;
    f->hop_start = (flags >> 5) & 0x07;
    f->channel_hash = buf[13];
    f->next_hop  = buf[14];
    f->relay_node = buf[15];
    int plen = len - MT_HEADER_LEN;
    if(plen < 0) plen = 0;
    if(plen > (int)sizeof f->payload) plen = sizeof f->payload;
    memcpy(f->payload, buf + MT_HEADER_LEN, plen);
    f->plen = (uint8_t)plen;
    return true;
}

/* ---- AES-CTR payload crypto ----
 * CryptoEngine::initNonce: 16-byte block = packet id as uint64 LE in [0..7]
 * (our ids are 32-bit, so [4..7] are 0), sender node id as uint32 LE in
 * [8..11], CTR counter in [12..15] starting at 0. Meshtastic sets a 4-byte
 * counter; mbedTLS increments the full 128-bit block, which is identical for
 * any payload under 2^32 blocks (ours are <=16 blocks). */
void mt_crypt(uint8_t *data, int len, uint32_t from, uint32_t packet_id)
{
    ensure_channel();
    uint8_t nonce[16];
    memset(nonce, 0, sizeof nonce);
    put_u32(nonce + 0, packet_id);   /* [0..3]; [4..7] stay 0 (u64 high word) */
    put_u32(nonce + 8, from);        /* [8..11]; [12..15] counter = 0 */

#if MT_HAVE_AES
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    /* CTR uses the encrypt key schedule for both directions */
    mbedtls_aes_setkey_enc(&ctx, g_psk, g_psk_len * 8);
    size_t nc_off = 0;
    uint8_t stream_block[16];
    memset(stream_block, 0, sizeof stream_block);
    mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce, stream_block,
                          data, data);
    mbedtls_aes_free(&ctx);
#else
    (void)data; (void)len;
#endif
}

/* ---- protobuf (Data submessage, mesh.proto) ----
 * field 1 portnum (varint), field 2 payload (bytes), field 3 want_response
 * (varint bool), field 6 request_id (FIXED32, wire type 5 — not varint; the
 * same is true of dest/source/reply_id/emoji). Standard protobuf otherwise. */
static void put_u32le(uint8_t *b, uint32_t v)
{
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; b[2] = (v >> 16) & 0xFF; b[3] = (v >> 24) & 0xFF;
}
static int pb_varint(uint8_t *b, uint64_t v)
{
    int i = 0;
    do { uint8_t byte = v & 0x7F; v >>= 7; if(v) byte |= 0x80; b[i++] = byte; } while(v);
    return i;
}

static bool pb_read_varint(const uint8_t *b, int len, int *pos, uint64_t *out)
{
    uint64_t v = 0; int shift = 0;
    while(*pos < len && shift < 64) {
        uint8_t byte = b[(*pos)++];
        v |= (uint64_t)(byte & 0x7F) << shift;
        if(!(byte & 0x80)) {
            if(out) *out = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool pb_skip(const uint8_t *b, int len, int *pos, int wire)
{
    if(wire == 0) {
        uint64_t ignored;
        return pb_read_varint(b, len, pos, &ignored);
    } else if(wire == 2) {
        uint64_t l;
        if(!pb_read_varint(b, len, pos, &l)) return false;
        if((uint64_t)*pos + l > (uint64_t)len) return false;
        *pos += (int)l;
        return true;
    } else if(wire == 5) {
        if(*pos + 4 > len) return false;
        *pos += 4;
        return true;
    } else if(wire == 1) {
        if(*pos + 8 > len) return false;
        *pos += 8;
        return true;
    }
    return false;
}

static uint32_t pb_read_fixed32(const uint8_t *b, int len, int *pos, bool *ok)
{
    if(*pos + 4 > len) { *ok = false; return 0; }
    uint32_t v = (uint32_t)b[*pos] | ((uint32_t)b[*pos + 1] << 8) |
                 ((uint32_t)b[*pos + 2] << 16) | ((uint32_t)b[*pos + 3] << 24);
    *pos += 4;
    return v;
}

static float pb_read_float(const uint8_t *b, int len, int *pos, bool *ok)
{
    uint32_t raw = pb_read_fixed32(b, len, pos, ok);
    float f = 0.0f;
    memcpy(&f, &raw, sizeof f);
    return f;
}

int mt_data_encode(uint8_t *buf, int cap, const mt_data_t *d)
{
    int n = 0;
    /* field 1: portnum, wire type 0 (varint) */
    if(n + 2 > cap) return -1;
    buf[n++] = (1 << 3) | 0;
    n += pb_varint(buf + n, d->portnum);
    /* field 2: payload, wire type 2 (length-delimited) */
    if(d->plen) {
        if(n + 2 + d->plen > cap) return -1;
        buf[n++] = (2 << 3) | 2;
        n += pb_varint(buf + n, d->plen);
        memcpy(buf + n, d->payload, d->plen);
        n += d->plen;
    }
    if(d->want_response) {
        if(n + 2 > cap) return -1;
        buf[n++] = (3 << 3) | 0;
        buf[n++] = 1;
    }
    if(d->request_id) {
        if(n + 5 > cap) return -1;
        buf[n++] = (6 << 3) | 5;           /* fixed32 */
        put_u32le(buf + n, d->request_id);
        n += 4;
    }
    return n;
}

bool mt_data_decode(const uint8_t *buf, int len, mt_data_t *d)
{
    memset(d, 0, sizeof *d);
    int pos = 0;
    while(pos < len) {
        uint64_t tag;
        if(!pb_read_varint(buf, len, &pos, &tag)) return false;
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if(field == 1 && wire == 0) {
            uint64_t v;
            if(!pb_read_varint(buf, len, &pos, &v)) return false;
            d->portnum = (uint8_t)v;
        } else if(field == 2 && wire == 2) {
            uint64_t l;
            if(!pb_read_varint(buf, len, &pos, &l)) return false;
            /* compare as uint64: a huge attacker length must not cast to a
             * negative int and slip past the bound (OOB read from a packet) */
            if((uint64_t)pos + l > (uint64_t)len) return false;
            uint64_t copy = l > sizeof d->payload ? sizeof d->payload : l;
            memcpy(d->payload, buf + pos, copy);
            d->plen = (uint8_t)copy;
            pos += (int)l;   /* l <= len-pos here, so the cast is safe */
        } else if(field == 3 && wire == 0) {
            uint64_t v;
            if(!pb_read_varint(buf, len, &pos, &v)) return false;
            d->want_response = v != 0;
        } else if(field == 6 && wire == 5) {       /* request_id: fixed32 LE */
            if(pos + 4 > len) return false;
            d->request_id = (uint32_t)buf[pos] | ((uint32_t)buf[pos+1] << 8) |
                            ((uint32_t)buf[pos+2] << 16) | ((uint32_t)buf[pos+3] << 24);
            pos += 4;
        } else if(!pb_skip(buf, len, &pos, wire)) {
            return false;
        }
    }
    return true;
}

bool mt_position_decode(const uint8_t *buf, int len, mt_position_t *p)
{
    if(!p) return false;
    memset(p, 0, sizeof *p);
    int pos = 0;
    bool ok = true;
    while(pos < len) {
        uint64_t tag;
        if(!pb_read_varint(buf, len, &pos, &tag)) return false;
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if(field == 1 && wire == 5) {
            p->latitude_i = (int32_t)pb_read_fixed32(buf, len, &pos, &ok);
            p->has_lat = ok;
        } else if(field == 2 && wire == 5) {
            p->longitude_i = (int32_t)pb_read_fixed32(buf, len, &pos, &ok);
            p->has_lon = ok;
        } else if(field == 3 && wire == 0) {
            uint64_t v;
            if(!pb_read_varint(buf, len, &pos, &v)) return false;
            p->altitude_m = (int32_t)v;
            p->has_alt = true;
        } else if((field == 4 || field == 7) && wire == 5) {
            p->time = pb_read_fixed32(buf, len, &pos, &ok);
        } else if(field == 23 && wire == 0) {
            uint64_t v;
            if(!pb_read_varint(buf, len, &pos, &v)) return false;
            p->precision_bits = v > 255 ? 255 : (uint8_t)v;
        } else if(!pb_skip(buf, len, &pos, wire)) {
            return false;
        }
        if(!ok) return false;
    }
    return true;
}

static bool mt_device_metrics_decode(const uint8_t *buf, int len, mt_telemetry_t *t)
{
    int pos = 0;
    bool ok = true;
    while(pos < len) {
        uint64_t tag;
        if(!pb_read_varint(buf, len, &pos, &tag)) return false;
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if(field == 1 && wire == 0) {
            uint64_t v;
            if(!pb_read_varint(buf, len, &pos, &v)) return false;
            t->battery_level = v > 255 ? 255 : (uint8_t)v;
            t->has_battery = true;
        } else if(field == 2 && wire == 5) {
            t->voltage = pb_read_float(buf, len, &pos, &ok);
            t->has_voltage = ok;
        } else if(field == 5 && wire == 0) {
            uint64_t v;
            if(!pb_read_varint(buf, len, &pos, &v)) return false;
            t->uptime_s = (uint32_t)v;
            t->has_uptime = true;
        } else if(!pb_skip(buf, len, &pos, wire)) {
            return false;
        }
        if(!ok) return false;
    }
    return true;
}

static bool mt_environment_metrics_decode(const uint8_t *buf, int len, mt_telemetry_t *t)
{
    int pos = 0;
    bool ok = true;
    while(pos < len) {
        uint64_t tag;
        if(!pb_read_varint(buf, len, &pos, &tag)) return false;
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if(field == 1 && wire == 5) {
            t->temperature_c = pb_read_float(buf, len, &pos, &ok);
            t->has_temperature = ok;
        } else if(field == 2 && wire == 5) {
            t->humidity_pct = pb_read_float(buf, len, &pos, &ok);
            t->has_humidity = ok;
        } else if(field == 3 && wire == 5) {
            t->pressure_hpa = pb_read_float(buf, len, &pos, &ok);
            t->has_pressure = ok;
        } else if(!pb_skip(buf, len, &pos, wire)) {
            return false;
        }
        if(!ok) return false;
    }
    return true;
}

bool mt_telemetry_decode(const uint8_t *buf, int len, mt_telemetry_t *t)
{
    if(!t) return false;
    memset(t, 0, sizeof *t);
    int pos = 0;
    while(pos < len) {
        uint64_t tag;
        if(!pb_read_varint(buf, len, &pos, &tag)) return false;
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if((field == 2 || field == 3) && wire == 2) {
            uint64_t l;
            if(!pb_read_varint(buf, len, &pos, &l)) return false;
            if((uint64_t)pos + l > (uint64_t)len) return false;
            bool ok = field == 2
                    ? mt_device_metrics_decode(buf + pos, (int)l, t)
                    : mt_environment_metrics_decode(buf + pos, (int)l, t);
            if(!ok) return false;
            pos += (int)l;
        } else if(!pb_skip(buf, len, &pos, wire)) {
            return false;
        }
    }
    return true;
}

int mt_build_text(uint8_t *out, int cap, uint32_t from, uint32_t to,
                  uint32_t id, uint8_t hop_limit, bool want_ack, const char *text)
{
    mt_data_t d;
    memset(&d, 0, sizeof d);
    d.portnum = MT_PORT_TEXT;
    size_t tl = strlen(text);
    if(tl > sizeof d.payload) tl = sizeof d.payload;
    memcpy(d.payload, text, tl);
    d.plen = (uint8_t)tl;

    uint8_t plain[251];
    int pn = mt_data_encode(plain, sizeof plain, &d);
    if(pn < 0) return -1;

    mt_crypt(plain, pn, from, id);     /* encrypt in place */

    mt_frame_t f;
    memset(&f, 0, sizeof f);
    f.to = to; f.from = from; f.id = id;
    f.hop_limit = hop_limit; f.hop_start = hop_limit;
    f.want_ack = want_ack;
    f.channel_hash = mt_channel_hash();
    f.next_hop = 0; f.relay_node = 0;

    if(cap < MT_HEADER_LEN + pn) return -1;
    mt_header_write(out, &f);
    memcpy(out + MT_HEADER_LEN, plain, pn);
    return MT_HEADER_LEN + pn;
}
