/* Self-tests for the shared C core: crypto vectors, LZ, tile codec, WebSocket. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rd_codec.h"
#include "rd_crypto.h"
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

int main(void) {
    test_crypto();
    test_lz();
    test_tiles();
    test_ws();
    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all common tests passed\n");
    return 0;
}
