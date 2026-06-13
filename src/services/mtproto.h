/**
 * Meshtastic wire protocol codec — header framing, channel hashing, AES-CTR
 * payload crypto, and the minimal protobuf needed for text + node info.
 *
 * Constants (header layout, default PSK, modem preset, channel hash) follow
 * the Meshtastic firmware so LimitlezzOS interoperates with stock devices on
 * the default LongFast channel. See mtproto.c for the authoritative values
 * and the firmware source lines they came from.
 */
#ifndef LZ_MTPROTO_H
#define LZ_MTPROTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Meshtastic PortNum (portnums.proto) */
enum {
    MT_PORT_TEXT      = 1,    /* TEXT_MESSAGE_APP   */
    MT_PORT_POSITION  = 3,    /* POSITION_APP       */
    MT_PORT_NODEINFO  = 4,    /* NODEINFO_APP       */
    MT_PORT_ROUTING   = 5,    /* ROUTING_APP (acks) */
    MT_PORT_TELEMETRY = 67,   /* TELEMETRY_APP      */
};

#define MT_BROADCAST   0xFFFFFFFFu
#define MT_HEADER_LEN  16

/* Decoded radio frame (header + still-encrypted or already-plain payload) */
typedef struct {
    uint32_t to, from, id;
    uint8_t  flags;          /* raw flags byte */
    uint8_t  channel_hash;
    uint8_t  next_hop, relay_node;
    uint8_t  hop_limit, hop_start;
    bool     want_ack, via_mqtt;
    uint8_t  payload[251];
    uint8_t  plen;
} mt_frame_t;

/* Decoded Data submessage (after decrypt) */
typedef struct {
    uint8_t  portnum;
    uint8_t  payload[237];
    uint8_t  plen;
    bool     want_response;
    uint32_t request_id;
} mt_data_t;

/* ---- channel ---- */
void    mt_set_channel(const char *name, const uint8_t *psk, int psk_len);
uint8_t mt_channel_hash(void);           /* hash of the configured channel */

/* ---- header (de)serialize ---- */
int  mt_header_write(uint8_t *buf, const mt_frame_t *f);   /* returns bytes written */
bool mt_header_read(const uint8_t *buf, int len, mt_frame_t *f);

/* ---- payload crypto (AES-CTR over the Data protobuf) ---- */
void mt_crypt(uint8_t *data, int len, uint32_t from, uint32_t packet_id);

/* ---- protobuf Data ---- */
int  mt_data_encode(uint8_t *buf, int cap, const mt_data_t *d);
bool mt_data_decode(const uint8_t *buf, int len, mt_data_t *d);

/* convenience: build a full TX frame for a text message */
int  mt_build_text(uint8_t *out, int cap, uint32_t from, uint32_t to,
                   uint32_t id, uint8_t hop_limit, bool want_ack, const char *text);

#ifdef __cplusplus
}
#endif

#endif
