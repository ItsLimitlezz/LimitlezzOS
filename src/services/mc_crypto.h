/**
 * Portable crypto for the MeshCore codec — SHA-256, HMAC-SHA256, and AES-128
 * ECB (both directions). Header-only so the exact same code runs in the desktop
 * simulator (selftest) and on the ESP32, rather than diverging from the
 * framework's mbedTLS / rweather-Crypto on hardware.
 *
 * MeshCore group channels use: AES-128-ECB with the 16-byte channel secret as
 * the key (NoPadding; plaintext is zero-padded to a 16-byte multiple) and an
 * Encrypt-then-MAC tag = HMAC-SHA256(secret-zero-padded-to-32, ciphertext)[:2].
 * Verified against meshcore-dev/MeshCore and michaelhart/meshcore-decoder.
 */
#ifndef LZ_MC_CRYPTO_H
#define LZ_MC_CRYPTO_H

#include "aes_min.h"          /* mbedtls_aes_context, lz_aes_setkey, lz_aes_encrypt_block, LZ_SBOX */
#include <stdint.h>
#include <string.h>

/* ---------------- SHA-256 ---------------- */

typedef struct { uint32_t h[8]; uint64_t total; uint8_t buf[64]; size_t n; } lz_sha256_ctx;

static const uint32_t LZ_SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

static inline uint32_t lz_ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static inline void lz_sha256_block(lz_sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    for(int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for(int i = 16; i < 64; i++) {
        uint32_t s0 = lz_ror32(w[i-15],7) ^ lz_ror32(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = lz_ror32(w[i-2],17) ^ lz_ror32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for(int i = 0; i < 64; i++) {
        uint32_t S1 = lz_ror32(e,6) ^ lz_ror32(e,11) ^ lz_ror32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + LZ_SHA256_K[i] + w[i];
        uint32_t S0 = lz_ror32(a,2) ^ lz_ror32(a,13) ^ lz_ror32(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static inline void lz_sha256_init(lz_sha256_ctx *c)
{
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->total = 0; c->n = 0;
}

static inline void lz_sha256_update(lz_sha256_ctx *c, const uint8_t *data, size_t len)
{
    c->total += len;
    while(len) {
        size_t take = 64 - c->n;
        if(take > len) take = len;
        memcpy(c->buf + c->n, data, take);
        c->n += take; data += take; len -= take;
        if(c->n == 64) { lz_sha256_block(c, c->buf); c->n = 0; }
    }
}

static inline void lz_sha256_final(lz_sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    lz_sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while(c->n != 56) lz_sha256_update(c, &zero, 1);
    uint8_t lenb[8];
    for(int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i*8));
    lz_sha256_update(c, lenb, 8);
    for(int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

static inline void lz_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    lz_sha256_ctx c; lz_sha256_init(&c); lz_sha256_update(&c, data, len); lz_sha256_final(&c, out);
}

/* ---------------- HMAC-SHA256 ---------------- */

static inline void lz_hmac_sha256(const uint8_t *key, size_t klen,
                                  const uint8_t *msg, size_t mlen, uint8_t out[32])
{
    uint8_t k[64], ipad[64], opad[64], inner[32];
    memset(k, 0, sizeof k);
    if(klen > 64) lz_sha256(key, klen, k);     /* keys >block get hashed first */
    else memcpy(k, key, klen);
    for(int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    lz_sha256_ctx c;
    lz_sha256_init(&c); lz_sha256_update(&c, ipad, 64); lz_sha256_update(&c, msg, mlen); lz_sha256_final(&c, inner);
    lz_sha256_init(&c); lz_sha256_update(&c, opad, 64); lz_sha256_update(&c, inner, 32); lz_sha256_final(&c, out);
}

/* ---------------- AES-128 ECB (decrypt block: inverse cipher) ---------------- */

static const uint8_t LZ_INV_SBOX[256] = {
0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d };

static inline uint8_t lz_gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for(int i = 0; i < 8; i++) {
        if(b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if(hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

static inline void lz_aes_decrypt_block(const mbedtls_aes_context *c,
                                        const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16]; memcpy(s, in, 16);
    const uint8_t *rk = c->round_key; int nr = c->nr;
    uint8_t t;
    for(int i = 0; i < 16; i++) s[i] ^= rk[16*nr + i];
    for(int round = nr - 1; round >= 1; round--) {
        /* InvShiftRows (rows are s[4*col+row]) */
        t=s[13]; s[13]=s[9]; s[9]=s[5]; s[5]=s[1]; s[1]=t;
        t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
        t=s[3]; s[3]=s[7]; s[7]=s[11]; s[11]=s[15]; s[15]=t;
        for(int i = 0; i < 16; i++) s[i] = LZ_INV_SBOX[s[i]];      /* InvSubBytes */
        for(int i = 0; i < 16; i++) s[i] ^= rk[16*round + i];      /* AddRoundKey */
        for(int col = 0; col < 4; col++) {                         /* InvMixColumns */
            uint8_t *p = s + 4*col, a0=p[0],a1=p[1],a2=p[2],a3=p[3];
            p[0] = lz_gmul(a0,14)^lz_gmul(a1,11)^lz_gmul(a2,13)^lz_gmul(a3,9);
            p[1] = lz_gmul(a0,9)^lz_gmul(a1,14)^lz_gmul(a2,11)^lz_gmul(a3,13);
            p[2] = lz_gmul(a0,13)^lz_gmul(a1,9)^lz_gmul(a2,14)^lz_gmul(a3,11);
            p[3] = lz_gmul(a0,11)^lz_gmul(a1,13)^lz_gmul(a2,9)^lz_gmul(a3,14);
        }
    }
    t=s[13]; s[13]=s[9]; s[9]=s[5]; s[5]=s[1]; s[1]=t;
    t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
    t=s[3]; s[3]=s[7]; s[7]=s[11]; s[11]=s[15]; s[15]=t;
    for(int i = 0; i < 16; i++) s[i] = LZ_INV_SBOX[s[i]];
    for(int i = 0; i < 16; i++) s[i] ^= rk[i];
    memcpy(out, s, 16);
}

/* ECB over whole 16-byte blocks (NoPadding). nblocks = byte_len / 16. */
static inline void lz_aes128_ecb_encrypt(const uint8_t key[16], const uint8_t *in,
                                         uint8_t *out, size_t nblocks)
{
    mbedtls_aes_context c; lz_aes_init(&c); lz_aes_setkey(&c, key, 128);
    for(size_t i = 0; i < nblocks; i++) lz_aes_encrypt_block(&c, in + 16*i, out + 16*i);
}

static inline void lz_aes128_ecb_decrypt(const uint8_t key[16], const uint8_t *in,
                                         uint8_t *out, size_t nblocks)
{
    mbedtls_aes_context c; lz_aes_init(&c); lz_aes_setkey(&c, key, 128);
    for(size_t i = 0; i < nblocks; i++) lz_aes_decrypt_block(&c, in + 16*i, out + 16*i);
}

#endif
