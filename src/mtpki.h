/* Meshtastic PKI direct-message crypto (X25519 + SHA256 + AES-256-CCM). */
#ifndef LZ_MTPKI_H
#define LZ_MTPKI_H
#include <stdint.h>
#include <stdbool.h>

void           lz_mtpki_init(void);
bool           lz_mtpki_ready(void);
const uint8_t *lz_mtpki_pubkey(void);                 /* our 32-byte X25519 public key */
/* encrypt plaintext -> blob (ciphertext+tag8+extraNonce4); returns blob len or -1 */
int  lz_mtpki_encrypt(const uint8_t *peer_pub, uint32_t from, uint32_t id,
                      const uint8_t *plain, int plen, uint8_t *out, int cap);
/* decrypt blob -> plaintext; returns plaintext len or -1 (tag mismatch / not ours) */
int  lz_mtpki_decrypt(const uint8_t *peer_pub, uint32_t from, uint32_t id,
                      const uint8_t *blob, int blen, uint8_t *out, int cap);

#endif
