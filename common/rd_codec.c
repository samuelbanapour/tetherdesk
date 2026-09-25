#include "rd_codec.h"

#include <string.h>

/* ------------------------------------------------------------------ LZ */

#define LZ_HASH_LOG 13
#define LZ_MIN_MATCH 4
#define LZ_LAST_LITERALS 5
#define LZ_MF_LIMIT 12

static uint32_t read32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

size_t rd_lz_bound(size_t n) { return n + n / 255 + 16; }

static uint8_t *put_len(uint8_t *op, size_t len) {
    while (len >= 255) {
        *op++ = 255;
        len -= 255;
    }
    *op++ = (uint8_t)len;
    return op;
}

static uint8_t *emit_literals(uint8_t *op, uint8_t *token, const uint8_t *lit, size_t n) {
    *token = (uint8_t)((n >= 15 ? 15 : n) << 4);
    if (n >= 15) op = put_len(op, n - 15);
    memcpy(op, lit, n);
    return op + n;
}

size_t rd_lz_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    if (cap < rd_lz_bound(n)) return 0;
    uint32_t table[1u << LZ_HASH_LOG];
    memset(table, 0, sizeof table);
    uint8_t *op = dst;
    size_t ip = 0, anchor = 0;

    if (n >= LZ_MF_LIMIT) {
        const size_t limit = n - LZ_MF_LIMIT;
        while (ip <= limit) {
            uint32_t seq = read32(src + ip);
            uint32_t h = (seq * 2654435761u) >> (32 - LZ_HASH_LOG);
            size_t cand = table[h];
            table[h] = (uint32_t)ip + 1;
            if (!cand || ip - (cand - 1) > 65535 || read32(src + cand - 1) != seq) {
                /* Skip faster through incompressible data. */
                ip += 1 + ((ip - anchor) >> 6);
                continue;
            }
            size_t ref = cand - 1;
            size_t m = LZ_MIN_MATCH;
            const size_t maxm = n - LZ_LAST_LITERALS - ip;
            while (m < maxm && src[ref + m] == src[ip + m]) m++;
            while (ip > anchor && ref > 0 && src[ip - 1] == src[ref - 1]) {
                ip--;
                ref--;
                m++;
            }
            uint8_t *token = op++;
            op = emit_literals(op, token, src + anchor, ip - anchor);
            size_t off = ip - ref;
            *op++ = (uint8_t)off;
            *op++ = (uint8_t)(off >> 8);
            size_t ml = m - LZ_MIN_MATCH;
            *token |= (uint8_t)(ml >= 15 ? 15 : ml);
            if (ml >= 15) op = put_len(op, ml - 15);
            ip += m;
            anchor = ip;
            if (ip - 2 <= limit) {
                uint32_t s2 = read32(src + ip - 2);
                table[(s2 * 2654435761u) >> (32 - LZ_HASH_LOG)] = (uint32_t)(ip - 2) + 1;
            }
        }
    }
    uint8_t *token = op++;
    op = emit_literals(op, token, src + anchor, n - anchor);
    return (size_t)(op - dst);
}

static int read_len(const uint8_t *src, size_t n, size_t *ip, size_t *len) {
    uint8_t b;
    do {
        if (*ip >= n) return -1;
        b = src[(*ip)++];
        *len += b;
    } while (b == 255);
    return 0;
}

long rd_lz_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    size_t ip = 0, op = 0;
    while (ip < n) {
        uint8_t t = src[ip++];
        size_t lit = t >> 4;
        if (lit == 15 && read_len(src, n, &ip, &lit)) return -1;
        if (lit > n - ip || lit > cap - op) return -1;
        memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip == n) break; /* final literal-only sequence */
        if (n - ip < 2) return -1;
        size_t off = (size_t)src[ip] | ((size_t)src[ip + 1] << 8);
        ip += 2;
        if (off == 0 || off > op) return -1;
        size_t ml = t & 15;
        if (ml == 15 && read_len(src, n, &ip, &ml)) return -1;
        ml += LZ_MIN_MATCH;
        if (ml > cap - op) return -1;
        uint8_t *d = dst + op;
        const uint8_t *s = d - off;
        if (off >= ml) memcpy(d, s, ml);
        else for (size_t i = 0; i < ml; i++) d[i] = s[i]; /* overlapping run */
        op += ml;
    }
    return (long)op;
}

/* --------------------------------------------------------------- Tiles */

#define TILE_PX (RD_TILE * RD_TILE)

static const uint8_t k_quality_mask[4] = {0xFF, 0xFC, 0xF8, 0xF0};

/* Median edge detector (LOCO-I / JPEG-LS) predictor. */
static int med(int a, int b, int c) {
    int mx = a > b ? a : b, mn = a < b ? a : b;
    if (c >= mx) return mn;
    if (c <= mn) return mx;
    return a + b - c;
}

static void put_rgb(rd_buf *out, uint32_t c) {
    uint8_t t[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    rd_buf_put(out, t, 3);
}

/* Palette lookup: open-addressed hash of colour -> index. */
#define PAL_HASH 1024
static uint32_t pal_hash(uint32_t c) { return ((c * 2654435761u) >> 22) & (PAL_HASH - 1); }

int rd_encode_tile(const uint8_t *bgra, size_t stride, int w, int h, int quality, rd_buf *out) {
    uint32_t px[TILE_PX];
    uint8_t idx[TILE_PX];
    const uint8_t mask = k_quality_mask[quality < 0 ? 0 : quality > 3 ? 3 : quality];
    const int n = w * h;

    /* Lossy levels round each channel to the nearest representable value
     * (not truncate), so reduced tiles don't come out darker than their
     * neighbours. */
    uint8_t q[256];
    const int half = (uint8_t)~mask >> 1;
    for (int v = 0; v < 256; v++) q[v] = (uint8_t)((v + half > 255 ? 255 : v + half) & mask);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = bgra + (size_t)y * stride;
        uint32_t *o = px + y * w;
        for (int x = 0; x < w; x++, row += 4)
            o[x] = ((uint32_t)q[row[2]] << 16) | ((uint32_t)q[row[1]] << 8) | q[row[0]];
    }

    /* Try to build a palette of up to 256 colours. */
    uint32_t pal[256];
    int16_t slot_idx[PAL_HASH];
    uint32_t slot_col[PAL_HASH];
    memset(slot_idx, -1, sizeof slot_idx);
    int np = 0, is_pal = 1;
    uint32_t last = ~px[0];
    uint8_t last_i = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = px[i];
        if (c == last) {
            idx[i] = last_i;
            continue;
        }
        uint32_t s = pal_hash(c);
        while (slot_idx[s] >= 0 && slot_col[s] != c) s = (s + 1) & (PAL_HASH - 1);
        if (slot_idx[s] < 0) {
            if (np == 256) {
                is_pal = 0;
                break;
            }
            slot_idx[s] = (int16_t)np;
            slot_col[s] = c;
            pal[np++] = c;
        }
        last = c;
        last_i = (uint8_t)slot_idx[s];
        idx[i] = last_i;
    }

    if (is_pal && np == 1) {
        put_rgb(out, pal[0]);
        return RD_ENC_SOLID;
    }

    const size_t raw_size = (size_t)n * 3;
    uint8_t work[TILE_PX * 3];
    uint8_t lz[TILE_PX * 3 + TILE_PX * 3 / 255 + 16];

    if (is_pal) {
        const int bpp = np <= 2 ? 1 : np <= 4 ? 2 : np <= 16 ? 4 : 8;
        const int row_bytes = (w * bpp + 7) / 8;
        const int per_byte = 8 / bpp;
        memset(work, 0, (size_t)row_bytes * h);
        for (int y = 0; y < h; y++) {
            uint8_t *dr = work + y * row_bytes;
            const uint8_t *sr = idx + y * w;
            for (int x = 0; x < w; x++) {
                int shift = 8 - bpp * (x % per_byte + 1);
                dr[x / per_byte] |= (uint8_t)(sr[x] << shift);
            }
        }
        size_t zl = rd_lz_compress(work, (size_t)row_bytes * h, lz, sizeof lz);
        if (1 + (size_t)np * 3 + zl < raw_size) {
            rd_buf_put_u8(out, (uint8_t)(np - 1));
            for (int i = 0; i < np; i++) put_rgb(out, pal[i]);
            rd_buf_put(out, lz, zl);
            return RD_ENC_PALETTE;
        }
    } else {
        /* Decorrelate (G, R-G, B-G), then predict each channel with MED. */
        uint8_t *t = work;
        for (int i = 0; i < n; i++) {
            uint32_t c = px[i];
            uint8_t r = (uint8_t)(c >> 16), g = (uint8_t)(c >> 8), b = (uint8_t)c;
            t[i * 3 + 0] = g;
            t[i * 3 + 1] = (uint8_t)(r - g);
            t[i * 3 + 2] = (uint8_t)(b - g);
        }
        uint8_t res[TILE_PX * 3];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int ch = 0; ch < 3; ch++) {
                    int i = (y * w + x) * 3 + ch;
                    int a = x ? t[i - 3] : (y ? t[i - w * 3] : 0);
                    int b = y ? t[i - w * 3] : a;
                    int c = (x && y) ? t[i - w * 3 - 3] : b;
                    res[i] = (uint8_t)(t[i] - med(a, b, c));
                }
        size_t zl = rd_lz_compress(res, raw_size, lz, sizeof lz);
        if (zl < raw_size) {
            rd_buf_put(out, lz, zl);
            return RD_ENC_ZRGB;
        }
    }

    uint8_t *raw = rd_buf_grow(out, raw_size);
    if (raw)
        for (int i = 0; i < n; i++) {
            raw[i * 3 + 0] = (uint8_t)(px[i] >> 16);
            raw[i * 3 + 1] = (uint8_t)(px[i] >> 8);
            raw[i * 3 + 2] = (uint8_t)px[i];
        }
    return RD_ENC_RAW;
}

static uint32_t argb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

int rd_decode_tile(int enc, const uint8_t *p, size_t len, int w, int h, uint32_t *dst, size_t ds) {
    if (w <= 0 || h <= 0 || w > RD_TILE || h > RD_TILE) return -1;
    const size_t n = (size_t)w * h;

    switch (enc) {
    case RD_ENC_SOLID: {
        if (len != 3) return -1;
        uint32_t c = argb(p[0], p[1], p[2]);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) dst[y * ds + x] = c;
        return 0;
    }
    case RD_ENC_RAW: {
        if (len != n * 3) return -1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++, p += 3) dst[y * ds + x] = argb(p[0], p[1], p[2]);
        return 0;
    }
    case RD_ENC_PALETTE: {
        if (len < 1) return -1;
        const int np = p[0] + 1;
        if (len < 1 + (size_t)np * 3) return -1;
        uint32_t pal[256];
        for (int i = 0; i < np; i++) pal[i] = argb(p[1 + i * 3], p[2 + i * 3], p[3 + i * 3]);
        const int bpp = np <= 2 ? 1 : np <= 4 ? 2 : np <= 16 ? 4 : 8;
        const int row_bytes = (w * bpp + 7) / 8;
        const int per_byte = 8 / bpp;
        const uint8_t bmask = (uint8_t)((1u << bpp) - 1);
        uint8_t packed[TILE_PX];
        const size_t want = (size_t)row_bytes * h;
        const size_t hdr = 1 + (size_t)np * 3;
        if (rd_lz_decompress(p + hdr, len - hdr, packed, want) != (long)want) return -1;
        for (int y = 0; y < h; y++) {
            const uint8_t *sr = packed + y * row_bytes;
            for (int x = 0; x < w; x++) {
                int shift = 8 - bpp * (x % per_byte + 1);
                int i = (sr[x / per_byte] >> shift) & bmask;
                if (i >= np) return -1;
                dst[y * ds + x] = pal[i];
            }
        }
        return 0;
    }
    case RD_ENC_ZRGB: {
        uint8_t t[TILE_PX * 3];
        if (rd_lz_decompress(p, len, t, n * 3) != (long)(n * 3)) return -1;
        /* Undo prediction in place: each sample depends only on earlier ones. */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int ch = 0; ch < 3; ch++) {
                    int i = (y * w + x) * 3 + ch;
                    int a = x ? t[i - 3] : (y ? t[i - w * 3] : 0);
                    int b = y ? t[i - w * 3] : a;
                    int c = (x && y) ? t[i - w * 3 - 3] : b;
                    t[i] = (uint8_t)(t[i] + med(a, b, c));
                }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t *s = t + (y * w + x) * 3;
                uint8_t g = s[0];
                dst[y * ds + x] = argb((uint8_t)(s[1] + g), g, (uint8_t)(s[2] + g));
            }
        return 0;
    }
    default:
        return -1;
    }
}
