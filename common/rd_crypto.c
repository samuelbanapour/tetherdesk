#include "rd_crypto.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* ---------------------------------------------------------------- SHA-1 */

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void rd_sha1(const void *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint8_t *p = (const uint8_t *)data;
    size_t n = len;
    while (n >= 64) { sha1_block(h, p); p += 64; n -= 64; }
    uint8_t tail[128] = {0};
    memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t tl = (n + 9 <= 64) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha1_block(h, tail);
    if (tl == 128) sha1_block(h, tail + 64);
    for (int i = 0; i < 5; i++) put_be32(out + 4 * i, h[i]);
}

/* -------------------------------------------------------------- SHA-256 */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void rd_sha256_init(rd_sha256_ctx *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->total = 0;
    c->fill = 0;
}

void rd_sha256_update(rd_sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    while (len) {
        size_t take = 64 - c->fill;
        if (take > len) take = len;
        memcpy(c->block + c->fill, p, take);
        c->fill += take;
        p += take;
        len -= take;
        if (c->fill == 64) {
            sha256_block(c->h, c->block);
            c->fill = 0;
        }
    }
}

void rd_sha256_final(rd_sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    rd_sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->fill != 56) rd_sha256_update(c, &zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
    rd_sha256_update(c, len_be, 8);
    for (int i = 0; i < 8; i++) put_be32(out + 4 * i, c->h[i]);
}

void rd_sha256(const void *data, size_t len, uint8_t out[32]) {
    rd_sha256_ctx c;
    rd_sha256_init(&c);
    rd_sha256_update(&c, data, len);
    rd_sha256_final(&c, out);
}

void rd_hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len, uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (key_len > 64) rd_sha256(key, key_len, k);
    else memcpy(k, key, key_len);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    uint8_t inner[32];
    rd_sha256_ctx c;
    rd_sha256_init(&c);
    rd_sha256_update(&c, ipad, 64);
    rd_sha256_update(&c, msg, msg_len);
    rd_sha256_final(&c, inner);
    rd_sha256_init(&c);
    rd_sha256_update(&c, opad, 64);
    rd_sha256_update(&c, inner, 32);
    rd_sha256_final(&c, out);
}

/* --------------------------------------------------------------- Base64 */

void rd_base64(const uint8_t *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[o] = 0;
}

/* --------------------------------------------------------------- Random */

int rd_random(void *buf, size_t len) {
#if defined(_WIN32)
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
#elif defined(__EMSCRIPTEN__)
    /* Emscripten's getentropy() is backed by crypto.getRandomValues. */
    extern int getentropy(void *, size_t);
    uint8_t *p = (uint8_t *)buf;
    while (len) {
        size_t n = len > 256 ? 256 : len;
        if (getentropy(p, n) != 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
#elif defined(__APPLE__)
    arc4random_buf(buf, len);
    return 0;
#else
    uint8_t *p = (uint8_t *)buf;
    while (len) {
        ssize_t n = getrandom(p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
#endif
}

int rd_ct_equal(const void *a, const void *b, size_t len) {
    const volatile uint8_t *x = (const volatile uint8_t *)a;
    const volatile uint8_t *y = (const volatile uint8_t *)b;
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) d |= x[i] ^ y[i];
    return d == 0;
}
