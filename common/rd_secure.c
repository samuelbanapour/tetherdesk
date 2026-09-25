#include "rd_secure.h"

#include <string.h>

#include "rd_crypto.h"

void rd_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_le64(uint8_t *p, uint64_t v) {
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------ X25519
 * Field arithmetic mod 2^255-19 with 16 limbs of 16 bits (after TweetNaCl,
 * public domain). Constant-time: no secret-dependent branches or indices. */

typedef int64_t gf[16];
static const gf k121665 = {0xDB41, 1};

static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}

static void sel25519(gf p, gf q, int b) {
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fmul(gf o, const gf a, const gf b) {
    int64_t t[31];
    memset(t, 0, sizeof t);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void fsq(gf o, const gf a) { fmul(o, a, a); }

static void inv25519(gf o, const gf in) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) {
        fsq(c, c);
        if (a != 2 && a != 4) fmul(c, c, in);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

int rd_x25519(uint8_t q[32], const uint8_t n[32], const uint8_t p[32]) {
    uint8_t z[32];
    gf x, a, b, c, d, e, f;
    memcpy(z, n, 32);
    z[31] = (uint8_t)((n[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        fadd(e, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fsq(d, e);
        fsq(f, a);
        fmul(a, c, a);
        fmul(c, b, e);
        fadd(e, a, c);
        fsub(a, a, c);
        fsq(b, a);
        fsub(c, d, f);
        fmul(a, c, k121665);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fsq(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    inv25519(c, c);
    fmul(a, a, c);
    pack25519(q, a);
    rd_wipe(z, sizeof z);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= q[i];
    return acc ? 0 : -1;
}

void rd_x25519_base(uint8_t pub[32], const uint8_t scalar[32]) {
    static const uint8_t nine[32] = {9};
    rd_x25519(pub, scalar, nine);
}

int rd_x25519_keypair(uint8_t priv[32], uint8_t pub[32]) {
    if (rd_random(priv, 32) != 0) return -1;
    rd_x25519_base(pub, priv);
    return 0;
}

/* ------------------------------------------------------------ ChaCha20 */

#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d)                                                  \
    a += b; d ^= a; d = ROTL(d, 16);                                   \
    c += d; b ^= c; b = ROTL(b, 12);                                   \
    a += b; d ^= a; d = ROTL(d, 8);                                    \
    c += d; b ^= c; b = ROTL(b, 7)

static void chacha_block(uint8_t out[64], const uint32_t in[16]) {
    uint32_t x[16];
    memcpy(x, in, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) put_le32(out + 4 * i, x[i] + in[i]);
}

void rd_chacha20(uint8_t *out, const uint8_t *in, size_t len, const uint8_t key[32], const uint8_t nonce[12],
                 uint32_t counter) {
    uint32_t st[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) st[4 + i] = le32(key + 4 * i);
    st[12] = counter;
    st[13] = le32(nonce);
    st[14] = le32(nonce + 4);
    st[15] = le32(nonce + 8);
    uint8_t ks[64];
    while (len) {
        chacha_block(ks, st);
        st[12]++;
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        out += n;
        in += n;
        len -= n;
    }
    rd_wipe(ks, sizeof ks);
}

/* ------------------------------------------------------------ Poly1305
 * 26-bit limb implementation (after poly1305-donna, public domain). */

typedef struct {
    uint32_t r[5], h[5], pad[4];
} poly_state;

static void poly_blocks(poly_state *st, const uint8_t *m, size_t bytes, uint32_t hibit) {
    const uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    while (bytes >= 16) {
        h0 += (le32(m + 0)) & 0x3ffffff;
        h1 += (le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (le32(m + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += 16;
        bytes -= 16;
    }
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

static void poly_init(poly_state *st, const uint8_t key[32]) {
    st->r[0] = (le32(key + 0)) & 0x3ffffff;
    st->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    st->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    st->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    st->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    memset(st->h, 0, sizeof st->h);
    for (int i = 0; i < 4; i++) st->pad[i] = le32(key + 16 + 4 * i);
}

static void poly_finish(poly_state *st, uint8_t tag[16]) {
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1; /* all ones if h >= p */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    h0 = (h0 | (h1 << 26));
    h1 = ((h1 >> 6) | (h2 << 20));
    h2 = ((h2 >> 12) | (h3 << 14));
    h3 = ((h3 >> 18) | (h4 << 8));
    uint64_t f = (uint64_t)h0 + st->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + st->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + st->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + st->pad[3] + (f >> 32); h3 = (uint32_t)f;
    put_le32(tag, h0);
    put_le32(tag + 4, h1);
    put_le32(tag + 8, h2);
    put_le32(tag + 12, h3);
    rd_wipe(st, sizeof *st);
}

void rd_poly1305(uint8_t tag[16], const uint8_t *msg, size_t len, const uint8_t key[32]) {
    poly_state st;
    poly_init(&st, key);
    size_t full = len & ~(size_t)15;
    poly_blocks(&st, msg, full, 1u << 24);
    if (len > full) {
        uint8_t last[16] = {0};
        memcpy(last, msg + full, len - full);
        last[len - full] = 1;
        poly_blocks(&st, last, 16, 0);
    }
    poly_finish(&st, tag);
}

/* ------------------------------------------------------------ AEAD */

/* Feeds data zero-padded to a 16-byte boundary (every block is "full"). */
static void poly_padded(poly_state *st, const uint8_t *p, size_t len) {
    size_t full = len & ~(size_t)15;
    poly_blocks(st, p, full, 1u << 24);
    if (len > full) {
        uint8_t last[16] = {0};
        memcpy(last, p + full, len - full);
        poly_blocks(st, last, 16, 1u << 24);
    }
}

static void aead_tag(uint8_t tag[16], const uint8_t *ct, size_t len, const uint8_t *ad, size_t ad_len,
                     const uint8_t key[32], const uint8_t nonce[12]) {
    uint8_t otk[64] = {0};
    rd_chacha20(otk, otk, 64, key, nonce, 0);
    poly_state st;
    poly_init(&st, otk);
    poly_padded(&st, ad, ad_len);
    poly_padded(&st, ct, len);
    uint8_t lens[16];
    put_le64(lens, ad_len);
    put_le64(lens + 8, len);
    poly_blocks(&st, lens, 16, 1u << 24);
    poly_finish(&st, tag);
    rd_wipe(otk, sizeof otk);
}

void rd_aead_seal(uint8_t *out, const uint8_t *msg, size_t len, const uint8_t *ad, size_t ad_len,
                  const uint8_t key[32], const uint8_t nonce[12]) {
    rd_chacha20(out, msg, len, key, nonce, 1);
    aead_tag(out + len, out, len, ad, ad_len, key, nonce);
}

int rd_aead_open(uint8_t *out, const uint8_t *in, size_t len, const uint8_t *ad, size_t ad_len,
                 const uint8_t key[32], const uint8_t nonce[12]) {
    if (len < RD_TAG_LEN) return -1;
    size_t n = len - RD_TAG_LEN;
    uint8_t tag[16];
    aead_tag(tag, in, n, ad, ad_len, key, nonce);
    if (!rd_ct_equal(tag, in + n, 16)) return -1;
    rd_chacha20(out, in, n, key, nonce, 1);
    return 0;
}

/* ------------------------------------------------------------ HKDF */

void rd_hkdf_sha256(uint8_t *out, size_t out_len, const uint8_t *ikm, size_t ikm_len, const uint8_t *salt,
                    size_t salt_len, const uint8_t *info, size_t info_len) {
    uint8_t prk[32], t[32], block[32 + 256 + 1];
    static const uint8_t zero_salt[32] = {0};
    if (!salt) salt = zero_salt, salt_len = 32;
    rd_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    size_t t_len = 0;
    if (info_len > 256) info_len = 256;
    for (uint8_t i = 1; out_len; i++) {
        memcpy(block, t, t_len);
        memcpy(block + t_len, info, info_len);
        block[t_len + info_len] = i;
        rd_hmac_sha256(prk, 32, block, t_len + info_len + 1, t);
        t_len = 32;
        size_t n = out_len < 32 ? out_len : 32;
        memcpy(out, t, n);
        out += n;
        out_len -= n;
    }
    rd_wipe(prk, sizeof prk);
    rd_wipe(t, sizeof t);
}

/* ------------------------------------------------------------ Session */

void rd_hs_transcript(uint8_t th[32], const uint8_t ce[32], const uint8_t ss[32], const uint8_t se[32],
                      const uint8_t nonce[16]) {
    rd_sha256_ctx c;
    rd_sha256_init(&c);
    rd_sha256_update(&c, "TetherDesk v2", 13);
    rd_sha256_update(&c, ce, 32);
    rd_sha256_update(&c, ss, 32);
    rd_sha256_update(&c, se, 32);
    rd_sha256_update(&c, nonce, 16);
    rd_sha256_final(&c, th);
}

void rd_hs_keys(rd_channel *ch, const uint8_t dh1[32], const uint8_t dh2[32], const uint8_t th[32], int is_host) {
    uint8_t ikm[64], okm[64];
    memcpy(ikm, dh1, 32);
    memcpy(ikm + 32, dh2, 32);
    rd_hkdf_sha256(okm, 64, ikm, 64, th, 32, (const uint8_t *)"tetherdesk keys", 15);
    const uint8_t *c2s = okm, *s2c = okm + 32;
    memcpy(ch->send_key, is_host ? s2c : c2s, 32);
    memcpy(ch->recv_key, is_host ? c2s : s2c, 32);
    ch->send_ctr = ch->recv_ctr = 0;
    ch->active = 1;
    rd_wipe(ikm, sizeof ikm);
    rd_wipe(okm, sizeof okm);
}

void rd_hs_auth_mac(uint8_t out[32], const char *password, size_t pw_len, const uint8_t th[32]) {
    uint8_t msg[15 + 32];
    memcpy(msg, "tetherdesk-auth", 15);
    memcpy(msg + 15, th, 32);
    rd_hmac_sha256(password, pw_len, msg, sizeof msg, out);
}

void rd_fingerprint(const uint8_t pub[32], char out[40]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t d[32];
    rd_sha256(pub, 32, d);
    size_t o = 0;
    for (int i = 0; i < 16; i++) {
        if (i && i % 2 == 0) out[o++] = '-';
        out[o++] = hex[d[i] >> 4];
        out[o++] = hex[d[i] & 15];
    }
    out[o] = 0;
}

static void ctr_nonce(uint8_t nonce[12], uint64_t ctr) {
    memset(nonce, 0, 4);
    put_le64(nonce + 4, ctr);
}

void rd_channel_seal(rd_channel *ch, const uint8_t *msg, size_t len, rd_buf *out) {
    uint8_t nonce[12];
    ctr_nonce(nonce, ch->send_ctr++);
    uint8_t *dst = rd_buf_grow(out, len + RD_TAG_LEN);
    if (dst) rd_aead_seal(dst, msg, len, NULL, 0, ch->send_key, nonce);
}

long rd_channel_open(rd_channel *ch, const uint8_t *in, size_t len, uint8_t *out) {
    uint8_t nonce[12];
    if (len < RD_TAG_LEN) return -1;
    ctr_nonce(nonce, ch->recv_ctr);
    if (rd_aead_open(out, in, len, NULL, 0, ch->recv_key, nonce) != 0) return -1;
    ch->recv_ctr++;
    return (long)(len - RD_TAG_LEN);
}
