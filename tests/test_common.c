/* Self-tests for the shared C core: crypto vectors, LZ, tile codec, WebSocket. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rd_codec.h"
#include "rd_crypto.h"
#include "rd_secure.h"
#include "rd_ws.h"

static int failures = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);          \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

static void hex(const uint8_t *p, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", p[i]);
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void test_crypto(void) {
    uint8_t d[32];
    char h[65];
    rd_sha1("abc", 3, d);
    hex(d, 20, h);
    CHECK(!strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d"), "sha1 %s", h);
    rd_sha256("abc", 3, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 %s", h);
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    rd_sha256(m, strlen(m), d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256-2 %s", h);
    rd_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "hmac %s", h);
    char acc[29];
    rd_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc);
    CHECK(!strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "ws accept %s", acc);
}

static void test_lz(void) {
    for (int t = 0; t < 400; t++) {
        size_t n = rnd() % 20000;
        uint8_t *src = malloc(n + 1), *dst = malloc(rd_lz_bound(n)), *back = malloc(n + 1);
        int mode = t % 4;
        for (size_t i = 0; i < n; i++) {
            if (mode == 0) src[i] = (uint8_t)rnd();
            else if (mode == 1) src[i] = (uint8_t)(i % 7);
            else if (mode == 2) src[i] = (rnd() % 10) ? (i ? src[i - 1] : 0) : (uint8_t)rnd();
            else src[i] = "hello world "[i % 12];
        }
        size_t zl = rd_lz_compress(src, n, dst, rd_lz_bound(n));
        long bl = rd_lz_decompress(dst, zl, back, n);
        CHECK(bl == (long)n && !memcmp(src, back, n), "lz roundtrip mode %d n %zu", mode, n);
        /* Corrupted input must never crash or overrun. */
        for (int k = 0; k < 20 && zl; k++) {
            dst[rnd() % zl] ^= (uint8_t)(1 + rnd() % 255);
            rd_lz_decompress(dst, zl, back, n);
        }
        free(src); free(dst); free(back);
    }
}

static void test_tiles(void) {
    uint8_t bgra[RD_TILE * RD_TILE * 4];
    uint32_t out[RD_TILE * RD_TILE];
    const char *names[] = {"solid", "palette", "zrgb", "raw"};
    int seen[4] = {0};
    for (int t = 0; t < 600; t++) {
        int w = 1 + rnd() % RD_TILE, h = 1 + rnd() % RD_TILE;
        int kind = t % 5, ncol = 1 + rnd() % 300;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t *p = bgra + (y * w + x) * 4;
                uint32_t c;
                if (kind == 0) c = 0x336699;
                else if (kind == 1) c = (rnd() % 3) * 0x404040;
                else if (kind == 2) c = (rnd() % ncol) * 0x010203;
                else if (kind == 3) c = (uint32_t)((x * 4) << 16 | (y * 4) << 8 | ((x + y) * 2));
                else c = rnd();
                p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)(c >> 16); p[3] = 255;
            }
        for (int q = 0; q < 4; q++) {
            rd_buf b;
            rd_buf_init(&b);
            int enc = rd_encode_tile(bgra, (size_t)w * 4, w, h, q, &b);
            seen[enc]++;
            memset(out, 0, sizeof out);
            int rc = rd_decode_tile(enc, b.data, b.len, w, h, out, (size_t)w);
            CHECK(rc == 0, "decode %s failed w%d h%d", names[enc], w, h);
            static const uint8_t mask[4] = {0xFF, 0xFC, 0xF8, 0xF0};
            const int half = (uint8_t)~mask[q] >> 1;
#define QV(v) ((uint32_t)(((v) + half > 255 ? 255 : (v) + half) & mask[q]))
            for (int i = 0; i < w * h && rc == 0; i++) {
                const uint8_t *p = bgra + i * 4;
                uint32_t want = 0xFF000000u | QV(p[2]) << 16 | QV(p[1]) << 8 | QV(p[0]);
                if (out[i] != want) {
                    CHECK(0, "pixel mismatch enc %s q%d i%d %08x != %08x", names[enc], q, i, out[i], want);
                    break;
                }
            }
            rd_buf_free(&b);
        }
    }
    printf("  tile encodings used: solid %d, palette %d, zrgb %d, raw %d\n", seen[0], seen[1], seen[2], seen[3]);
    CHECK(seen[0] && seen[1] && seen[2] && seen[3], "every encoding should be exercised");
}

static void test_ws(void) {
    rd_buf wire;
    rd_buf_init(&wire);
    uint8_t big[70000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)i;
    rd_ws_write_frame(&wire, RD_WS_BINARY, "hi", 2, 1);
    rd_ws_write_frame(&wire, RD_WS_BINARY, big, sizeof big, 1);
    rd_ws_write_frame(&wire, RD_WS_PING, "p", 1, 1);
    rd_ws_reader r;
    rd_ws_reader_init(&r, 1 << 20, 1);
    rd_ws_msg m;
    /* Feed one byte at a time to exercise partial-frame handling. */
    rd_buf in;
    rd_buf_init(&in);
    int got = 0;
    for (size_t i = 0; i < wire.len; i++) {
        rd_buf_put(&in, wire.data + i, 1);
        int rc;
        while ((rc = rd_ws_next(&r, &in, &m)) == 1) {
            if (got == 0) CHECK(m.len == 2 && !memcmp(m.data, "hi", 2), "msg0");
            if (got == 1) CHECK(m.len == sizeof big && !memcmp(m.data, big, sizeof big), "msg1");
            if (got == 2) CHECK(m.opcode == RD_WS_PING && m.len == 1, "ping");
            got++;
        }
        CHECK(rc == 0, "ws parse error");
    }
    CHECK(got == 3, "got %d messages", got);
    /* An unmasked frame to a server is a protocol error. */
    rd_buf_clear(&wire);
    rd_ws_write_frame(&wire, RD_WS_BINARY, "x", 1, 0);
    CHECK(rd_ws_next(&r, &wire, &m) == -1, "unmasked should fail");
    char v[64];
    CHECK(rd_http_header("GET / HTTP/1.1\r\nHost: a\r\nsec-websocket-key:  abc \r\n\r\n", "Sec-WebSocket-Key",
                         v, sizeof v) && !strcmp(v, "abc"), "header parse");
    rd_ws_reader_free(&r);
    rd_buf_free(&wire);
    rd_buf_free(&in);
}


static void unhex(const char *h, uint8_t *out) {
    for (size_t i = 0; h[2 * i]; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_secure(void) {
    char h[200];
    uint8_t k[32], u[32], o[32];
    /* RFC 7748 5.2 */
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u);
    rd_x25519(o, k, u);
    hex(o, 32, h);
    CHECK(!strcmp(h, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"), "x25519 %s", h);
    /* RFC 7748 6.1 */
    uint8_t apriv[32], apub[32], bpriv[32], bpub[32], s1[32], s2[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", apriv);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bpriv);
    rd_x25519_base(apub, apriv);
    rd_x25519_base(bpub, bpriv);
    hex(apub, 32, h);
    CHECK(!strcmp(h, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"), "x25519 pub %s", h);
    rd_x25519(s1, apriv, bpub);
    rd_x25519(s2, bpriv, apub);
    hex(s1, 32, h);
    CHECK(!strcmp(h, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742") && !memcmp(s1, s2, 32),
          "x25519 shared %s", h);
    uint8_t zero[32] = {0};
    CHECK(rd_x25519(o, apriv, zero) == -1, "low-order point must be rejected");

    /* RFC 8439 2.5.2 Poly1305 */
    uint8_t pk[32], tag[16];
    unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", pk);
    const char *pm = "Cryptographic Forum Research Group";
    rd_poly1305(tag, (const uint8_t *)pm, strlen(pm), pk);
    hex(tag, 16, h);
    CHECK(!strcmp(h, "a8061dc1305136c6c22b8baf0c0127a9"), "poly1305 %s", h);

    /* RFC 8439 2.8.2 AEAD */
    const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
                     "sunscreen would be it.";
    uint8_t key[32], nonce[12], aad[12], ct[200], back[200];
    unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key);
    unhex("070000004041424344454647", nonce);
    unhex("50515253c0c1c2c3c4c5c6c7", aad);
    size_t n = strlen(pt);
    rd_aead_seal(ct, (const uint8_t *)pt, n, aad, 12, key, nonce);
    hex(ct, 16, h);
    CHECK(!strcmp(h, "d31a8d34648e60db7b86afbc53ef7ec2"), "aead ct %s", h);
    hex(ct + n, 16, h);
    CHECK(!strcmp(h, "1ae10b594f09e26a7e902ecbd0600691"), "aead tag %s", h);
    CHECK(rd_aead_open(back, ct, n + 16, aad, 12, key, nonce) == 0 && !memcmp(back, pt, n), "aead open");
    ct[5] ^= 1;
    CHECK(rd_aead_open(back, ct, n + 16, aad, 12, key, nonce) == -1, "aead must reject tampering");

    /* RFC 5869 A.1 HKDF */
    uint8_t ikm[22], salt[13], info[10], okm[42];
    memset(ikm, 0x0b, sizeof ikm);
    unhex("000102030405060708090a0b0c", salt);
    unhex("f0f1f2f3f4f5f6f7f8f9", info);
    rd_hkdf_sha256(okm, 42, ikm, 22, salt, 13, info, 10);
    hex(okm, 42, h);
    CHECK(!strcmp(h, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
          "hkdf %s", h);

    /* Full handshake + channel round trip, including replay rejection. */
    uint8_t ce_priv[32], ce[32], ss_priv[32], ss[32], se_priv[32], se[32], hn[16] = {1, 2, 3}, th[32];
    rd_x25519_keypair(ce_priv, ce);
    rd_x25519_keypair(ss_priv, ss);
    rd_x25519_keypair(se_priv, se);
    rd_hs_transcript(th, ce, ss, se, hn);
    uint8_t cd1[32], cd2[32], hd1[32], hd2[32];
    rd_x25519(cd1, ce_priv, se);
    rd_x25519(cd2, ce_priv, ss);
    rd_x25519(hd1, se_priv, ce);
    rd_x25519(hd2, ss_priv, ce);
    rd_channel cv, ch;
    rd_hs_keys(&cv, cd1, cd2, th, 0);
    rd_hs_keys(&ch, hd1, hd2, th, 1);
    rd_buf wire;
    rd_buf_init(&wire);
    for (int i = 0; i < 3; i++) {
        rd_buf_clear(&wire);
        char msg[32];
        snprintf(msg, sizeof msg, "message %d", i);
        rd_channel_seal(&cv, (const uint8_t *)msg, strlen(msg), &wire);
        uint8_t plain[64];
        long pl = rd_channel_open(&ch, wire.data, wire.len, plain);
        CHECK(pl == (long)strlen(msg) && !memcmp(plain, msg, (size_t)pl), "channel msg %d", i);
    }
    uint8_t plain[64];
    CHECK(rd_channel_open(&ch, wire.data, wire.len, plain) == -1, "replayed message must be rejected");
    rd_buf_free(&wire);
    char fp[40];
    rd_fingerprint(ss, fp);
    CHECK(strlen(fp) == 39, "fingerprint format %s", fp);
}

int main(void) {
    test_crypto();
    test_lz();
    test_tiles();
    test_ws();
    test_secure();
    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all common tests passed\n");
    return 0;
}
