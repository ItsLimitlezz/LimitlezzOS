/* MeshCore wire codec — see mcproto.h for the layout references. */
#include "mcproto.h"
#include "mc_crypto.h"
#include <string.h>
#include <stdio.h>

/* "Public" channel PSK izOH6cXN6mrJ5e26oRXNcg== (base64) -> 16 bytes */
const uint8_t MC_PUBLIC_SECRET[16] = {
    0x8b,0x33,0x87,0xe9,0xc5,0xcd,0xea,0x6a,0xc9,0xe5,0xed,0xba,0xa1,0x15,0xcd,0x72 };

bool mc_parse(const uint8_t *buf, int len, mc_pkt_t *o)
{
    if(len < 2) return false;
    uint8_t h = buf[0];
    o->route_type   = h & MC_ROUTE_MASK;
    o->payload_type = (h >> MC_TYPE_SHIFT) & MC_TYPE_MASK;
    o->version      = (h >> MC_VER_SHIFT) & MC_ROUTE_MASK;

    int i = 1;
    bool transport = (o->route_type == MC_ROUTE_TRANSPORT_FLOOD ||
                      o->route_type == MC_ROUTE_TRANSPORT_DIRECT);
    if(transport) { if(i + 4 > len) return false; i += 4; }  /* 2x uint16 codes */

    if(i >= len) return false;
    uint8_t path_len = buf[i++];
    uint8_t hash_count = path_len & 63;
    uint8_t hash_size  = (path_len >> 6) + 1;
    if(hash_size == 4) return false;                          /* reserved */
    int bl = hash_count * hash_size;
    if(bl > 64 || i + bl > len) return false;                 /* MAX_PATH_SIZE */
    i += bl;

    if(i > len) return false;
    o->payload = buf + i;
    o->payload_len = len - i;
    return true;
}

bool mc_advert_decode(const mc_pkt_t *p, mc_advert_t *o)
{
    if(p->payload_type != MC_PAYLOAD_ADVERT) return false;
    if(p->payload_len < 32 + 4 + 64) return false;            /* pubkey+ts+sig */

    memset(o, 0, sizeof *o);
    memcpy(o->pubkey, p->payload, 32);
    memcpy(&o->timestamp, p->payload + 32, 4);                /* LE */

    const uint8_t *app = p->payload + 100;
    int app_len = p->payload_len - 100;
    if(app_len < 1) return true;                              /* valid, no app data */

    uint8_t flags = app[0];
    o->adv_type = flags & 0x0F;
    int i = 1;
    if(flags & 0x10) i += 8;                                  /* lat/lon */
    if(flags & 0x20) i += 2;                                  /* feat1 */
    if(flags & 0x40) i += 2;                                  /* feat2 */
    if((flags & 0x80) && app_len > i) {                       /* name = remainder */
        int nlen = app_len - i;
        if(nlen > (int)sizeof o->name - 1) nlen = sizeof o->name - 1;
        memcpy(o->name, app + i, nlen);
        o->name[nlen] = 0;
        o->has_name = nlen > 0;
    }
    return true;
}

uint8_t mc_group_channel_hash(const mc_pkt_t *p)
{
    return p->payload_len >= 1 ? p->payload[0] : 0;
}

/* group payload layout: [channel_hash:1][cipher_mac:2][AES-128-ECB ciphertext] */
bool mc_group_decode(const mc_pkt_t *p, const uint8_t secret16[16], mc_group_msg_t *o)
{
    if(p->payload_type != MC_PAYLOAD_GRP_TXT) return false;
    int ctlen = p->payload_len - 3;                       /* minus hash + mac */
    if(ctlen <= 0 || ctlen % 16 != 0 || ctlen > 240) return false;
    const uint8_t *mac = p->payload + 1;
    const uint8_t *ct  = p->payload + 3;

    /* MAC = HMAC-SHA256(secret zero-padded to 32, ciphertext)[:2] */
    uint8_t k32[32]; memset(k32, 0, sizeof k32); memcpy(k32, secret16, 16);
    uint8_t full[32];
    lz_hmac_sha256(k32, sizeof k32, ct, ctlen, full);
    if(full[0] != mac[0] || full[1] != mac[1]) return false;   /* wrong channel / corrupt */

    uint8_t pt[240];
    lz_aes128_ecb_decrypt(secret16, ct, pt, ctlen / 16);
    if(ctlen < 5) return false;

    memset(o, 0, sizeof *o);
    memcpy(&o->timestamp, pt, 4);                          /* LE */
    o->flags = pt[4];

    char body[208];
    int blen = ctlen - 5;
    if(blen > (int)sizeof body - 1) blen = sizeof body - 1;
    memcpy(body, pt + 5, blen);
    body[blen] = 0;                                        /* zero-pad is also the NUL terminator */

    char *colon = strstr(body, ": ");
    if(colon && colon - body > 0 && colon - body < (int)sizeof o->sender) {
        int sl = (int)(colon - body);
        memcpy(o->sender, body, sl);
        o->sender[sl] = 0;
        snprintf(o->text, sizeof o->text, "%s", colon + 2);
    } else {
        snprintf(o->text, sizeof o->text, "%s", body);
    }
    return true;
}

int mc_group_encode(uint8_t *frame, int cap, const uint8_t secret16[16],
                    uint32_t timestamp, const char *sender, const char *text)
{
    char body[192];
    int bl = snprintf(body, sizeof body, "%s: %s", sender ? sender : "", text ? text : "");
    if(bl < 0) return -1;
    if(bl > (int)sizeof body - 1) bl = (int)sizeof body - 1;

    /* plaintext = [timestamp:4 LE][flags:1][body], zero-padded to a 16B multiple */
    uint8_t pt[224]; int pl = 0;
    memcpy(pt + pl, &timestamp, 4); pl += 4;
    pt[pl++] = 0;                                          /* flags */
    memcpy(pt + pl, body, bl); pl += bl;
    int padded = (pl + 15) & ~15;
    if(padded > (int)sizeof pt) return -1;
    memset(pt + pl, 0, padded - pl);

    uint8_t ct[224];
    lz_aes128_ecb_encrypt(secret16, pt, ct, padded / 16);
    uint8_t k32[32]; memset(k32, 0, sizeof k32); memcpy(k32, secret16, 16);
    uint8_t mac[32]; lz_hmac_sha256(k32, sizeof k32, ct, padded, mac);
    uint8_t chash[32]; lz_sha256(secret16, 16, chash);

    int need = 2 + 1 + 2 + padded;                        /* header + path_len + hash + mac + ct */
    if(need > cap) return -1;
    int fl = 0;
    frame[fl++] = (MC_PAYLOAD_GRP_TXT << MC_TYPE_SHIFT) | MC_ROUTE_FLOOD;
    frame[fl++] = 0;                                       /* path_len = 0 */
    frame[fl++] = chash[0];
    frame[fl++] = mac[0];
    frame[fl++] = mac[1];
    memcpy(frame + fl, ct, padded); fl += padded;
    return fl;
}

const char *mc_type_name(uint8_t t)
{
    switch(t) {
        case MC_PAYLOAD_REQ:      return "REQ";
        case MC_PAYLOAD_RESPONSE: return "RESP";
        case MC_PAYLOAD_TXT_MSG:  return "TXT";
        case MC_PAYLOAD_ACK:      return "ACK";
        case MC_PAYLOAD_ADVERT:   return "ADVERT";
        case MC_PAYLOAD_GRP_TXT:  return "GRP_TXT";
        case MC_PAYLOAD_GRP_DATA: return "GRP_DATA";
        case MC_PAYLOAD_ANON_REQ: return "ANON_REQ";
        case MC_PAYLOAD_PATH:     return "PATH";
        case MC_PAYLOAD_TRACE:    return "TRACE";
        default:                  return "?";
    }
}
