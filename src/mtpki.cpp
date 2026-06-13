/**
 * Meshtastic PKI (public-key) direct-message crypto.
 *
 * Format reverse-engineered from meshtastic/firmware CryptoEngine + protobufs
 * (byte-exact):
 *   keypair  : X25519 (Curve25519), private clamped per RFC 7748, public = 32B u-coord,
 *              advertised in NodeInfo User.public_key (field 8).
 *   shared   : k = X25519(our_priv, peer_pub); aes_key = SHA256(k)   -> AES-256
 *   nonce[16]: memset 0; [0..7]=packetId(LE u64); [8..11]=fromNode(LE u32);
 *              if(extraNonce) [4..7]=extraNonce(LE u32)  (overwrites packetId hi bytes!)
 *              CCM uses the first 13 bytes (L=2).
 *   AEAD     : AES-256-CCM, L=2, M=8 (8-byte tag), no AAD.
 *   blob     : [ciphertext (=plaintext len)] [tag 8] [extraNonce 4 LE]   (+12 overhead)
 *   on air   : header.channel byte == 0 marks a PKI DM; sender's key is NOT in the
 *              packet — the receiver looks it up by `from` in its node DB.
 *
 * AES-CCM via mbedTLS (exact tag/nonce lengths); X25519 + SHA256 via rweather/Crypto.
 */
#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <Curve25519.h>
#include <SHA256.h>
#include "mbedtls/ccm.h"
#include "services/mesh.h"
#include "mtpki.h"

extern "C" {
    void lz_store_save_mt_key(const uint8_t *prv32);
    bool lz_store_load_mt_key(uint8_t *prv32);
}

#define MT_PKC_OVERHEAD 12      /* 8-byte tag + 4-byte extraNonce */

static uint8_t g_mt_prv[32], g_mt_pub[32];
static bool    g_mt_id_ok;

/* derive the AES-256 key shared with `peer_pub`: SHA256(X25519(our_priv, peer_pub)) */
static bool shared_key(const uint8_t *peer_pub, uint8_t out_key[32])
{
    if(!g_mt_id_ok) return false;
    uint8_t shared[32];
    memcpy(shared, peer_pub, 32);
    if(!Curve25519::eval(shared, g_mt_prv, peer_pub)) return false;   /* X25519, weak-key checked */
    SHA256 sha; sha.reset(); sha.update(shared, 32); sha.finalize(out_key, 32);
    return true;
}

/* build the 16-byte nonce buffer exactly as firmware initNonce() does */
static void build_nonce(uint32_t from, uint32_t id, uint32_t extra, uint8_t nonce[16])
{
    memset(nonce, 0, 16);
    uint64_t pid = id;                 /* packetId widened to u64 LE */
    memcpy(nonce + 0, &pid, 8);
    memcpy(nonce + 8, &from, 4);
    if(extra) memcpy(nonce + 4, &extra, 4);   /* overwrites packetId hi bytes (firmware quirk) */
}

void lz_mtpki_init(void)
{
    if(!lz_store_load_mt_key(g_mt_prv)) {
        for(int i = 0; i < 32; i++) g_mt_prv[i] = (uint8_t)(esp_random() & 0xFF);
        g_mt_prv[0]  &= 0xF8;                  /* clamp (RFC 7748) */
        g_mt_prv[31]  = (g_mt_prv[31] & 0x7F) | 0x40;
        lz_store_save_mt_key(g_mt_prv);
        Curve25519::eval(g_mt_pub, g_mt_prv, 0);
        Serial.printf("[ok] Meshtastic PKI key generated: %02x%02x%02x%02x...\n",
                      g_mt_pub[0], g_mt_pub[1], g_mt_pub[2], g_mt_pub[3]);
    } else {
        Curve25519::eval(g_mt_pub, g_mt_prv, 0);   /* derive public from stored private */
    }
    g_mt_id_ok = true;
}

bool lz_mtpki_ready(void) { return g_mt_id_ok; }
const uint8_t *lz_mtpki_pubkey(void) { return g_mt_pub; }

/* encrypt plaintext -> blob (ciphertext+tag+extraNonce). returns blob length or -1 */
int lz_mtpki_encrypt(const uint8_t *peer_pub, uint32_t from, uint32_t id,
                     const uint8_t *plain, int plen, uint8_t *out, int cap)
{
    if(!g_mt_id_ok || plen < 0 || plen + MT_PKC_OVERHEAD > cap) return -1;
    uint8_t key[32];
    if(!shared_key(peer_pub, key)) return -1;

    uint32_t extra = esp_random(); if(!extra) extra = 1;   /* nonzero so enc/dec agree */
    uint8_t nonce[16];
    build_nonce(from, id, extra, nonce);

    uint8_t tag[8];
    mbedtls_ccm_context ctx; mbedtls_ccm_init(&ctx);
    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if(rc == 0)
        rc = mbedtls_ccm_encrypt_and_tag(&ctx, plen, nonce, 13, NULL, 0,
                                         plain, out, tag, 8);
    mbedtls_ccm_free(&ctx);
    if(rc != 0) return -1;

    memcpy(out + plen, tag, 8);
    memcpy(out + plen + 8, &extra, 4);     /* extraNonce, LE */
    return plen + MT_PKC_OVERHEAD;
}

/* decrypt blob (ciphertext+tag+extraNonce) -> plaintext. returns plaintext length or -1 */
int lz_mtpki_decrypt(const uint8_t *peer_pub, uint32_t from, uint32_t id,
                     const uint8_t *blob, int blen, uint8_t *out, int cap)
{
    if(!g_mt_id_ok || blen <= MT_PKC_OVERHEAD) return -1;
    int clen = blen - MT_PKC_OVERHEAD;
    if(clen > cap) return -1;
    const uint8_t *tag = blob + clen;
    uint32_t extra; memcpy(&extra, blob + clen + 8, 4);

    uint8_t key[32];
    if(!shared_key(peer_pub, key)) return -1;
    uint8_t nonce[16];
    build_nonce(from, id, extra, nonce);

    mbedtls_ccm_context ctx; mbedtls_ccm_init(&ctx);
    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if(rc == 0)
        rc = mbedtls_ccm_auth_decrypt(&ctx, clen, nonce, 13, NULL, 0,
                                      blob, out, tag, 8);
    mbedtls_ccm_free(&ctx);
    return rc == 0 ? clen : -1;        /* rc != 0 -> tag mismatch (not for us / corrupt) */
}

/* on-device self-test: A=us, B=ephemeral; round-trip + tamper check */
extern "C" int lz_mtpki_selftest(char *out, int n)
{
    if(!g_mt_id_ok) return snprintf(out, n, "no PKI identity");
    uint8_t b_prv[32], b_pub[32];
    for(int i = 0; i < 32; i++) b_prv[i] = (uint8_t)(esp_random() & 0xFF);
    b_prv[0] &= 0xF8; b_prv[31] = (b_prv[31] & 0x7F) | 0x40;
    Curve25519::eval(b_pub, b_prv, 0);

    const char *msg = "hello pki";
    uint8_t blob[64];
    int bl = lz_mtpki_encrypt(b_pub, 0x11223344, 0xa1b2c3d4,
                              (const uint8_t *)msg, strlen(msg), blob, sizeof blob);
    if(bl < 0) return snprintf(out, n, "encrypt failed");

    /* B decrypts using A's (our) public key — DH is symmetric */
    uint8_t save_prv[32]; memcpy(save_prv, g_mt_prv, 32);   /* temporarily become B */
    memcpy(g_mt_prv, b_prv, 32);
    uint8_t plain[64];
    int pl = lz_mtpki_decrypt(g_mt_pub /* A pub */, 0x11223344, 0xa1b2c3d4, blob, bl, plain, sizeof plain);
    bool match = (pl == (int)strlen(msg) && memcmp(plain, msg, pl) == 0);   /* check BEFORE tamper */

    /* flip a ciphertext bit -> tag must fail (mbedtls zeroes `tmp` on failure) */
    uint8_t tmp[64];
    blob[0] ^= 0x01;
    bool tamper_ok = (lz_mtpki_decrypt(g_mt_pub, 0x11223344, 0xa1b2c3d4, blob, bl, tmp, sizeof tmp) < 0);
    blob[0] ^= 0x01;
    memcpy(g_mt_prv, save_prv, 32);   /* restore A */
    return snprintf(out, n, "blob=%dB roundtrip=%s tamper-rejected=%s -> %s",
                    bl, match ? "ok" : "FAIL", tamper_ok ? "ok" : "FAIL",
                    (match && tamper_ok) ? "PASS" : "FAIL");
}

#endif /* LZ_TARGET_TDECK */
