#include "rd_bytes.h"

#include <stdlib.h>
#include <string.h>

void rd_buf_init(rd_buf *b) { b->data = NULL; b->len = b->cap = 0; }

void rd_buf_free(rd_buf *b) {
    free(b->data);
    rd_buf_init(b);
}

void rd_buf_clear(rd_buf *b) { b->len = 0; }

int rd_buf_reserve(rd_buf *b, size_t extra) {
    size_t need = b->len + extra;
    if (need <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < need) ncap *= 2;
    uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
    if (!nd) return -1;
    b->data = nd;
    b->cap = ncap;
    return 0;
}

uint8_t *rd_buf_grow(rd_buf *b, size_t n) {
    if (rd_buf_reserve(b, n) != 0) return NULL;
    uint8_t *p = b->data + b->len;
    b->len += n;
    return p;
}

void rd_buf_put(rd_buf *b, const void *p, size_t n) {
    if (!n) return;
    uint8_t *d = rd_buf_grow(b, n);
    if (d) memcpy(d, p, n);
}

void rd_buf_put_u8(rd_buf *b, uint8_t v) { rd_buf_put(b, &v, 1); }

void rd_buf_put_u16(rd_buf *b, uint16_t v) {
    uint8_t t[2];
    rd_wr16(t, v);
    rd_buf_put(b, t, 2);
}

void rd_buf_put_u32(rd_buf *b, uint32_t v) {
    uint8_t t[4];
    rd_wr32(t, v);
    rd_buf_put(b, t, 4);
}

void rd_buf_put_u64(rd_buf *b, uint64_t v) {
    rd_buf_put_u32(b, (uint32_t)v);
    rd_buf_put_u32(b, (uint32_t)(v >> 32));
}

void rd_buf_put_str(rd_buf *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 0xFFFF) n = 0xFFFF;
    rd_buf_put_u16(b, (uint16_t)n);
    rd_buf_put(b, s, n);
}

void rd_buf_put_blob(rd_buf *b, const void *p, uint32_t n) {
    rd_buf_put_u32(b, n);
    rd_buf_put(b, p, n);
}

void rd_buf_consume(rd_buf *b, size_t n) {
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

void rd_buf_patch_u32(rd_buf *b, size_t off, uint32_t v) {
    if (off + 4 <= b->len) rd_wr32(b->data + off, v);
}

void rd_buf_patch_u16(rd_buf *b, size_t off, uint16_t v) {
    if (off + 2 <= b->len) rd_wr16(b->data + off, v);
}

void rd_reader_init(rd_reader *r, const void *p, size_t len) {
    r->p = (const uint8_t *)p;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

size_t rd_remaining(const rd_reader *r) { return r->err ? 0 : r->len - r->pos; }

const uint8_t *rd_get_bytes(rd_reader *r, size_t n) {
    if (r->err || r->len - r->pos < n) {
        r->err = 1;
        return NULL;
    }
    const uint8_t *p = r->p + r->pos;
    r->pos += n;
    return p;
}

uint8_t rd_get_u8(rd_reader *r) {
    const uint8_t *p = rd_get_bytes(r, 1);
    return p ? p[0] : 0;
}

uint16_t rd_get_u16(rd_reader *r) {
    const uint8_t *p = rd_get_bytes(r, 2);
    return p ? rd_rd16(p) : 0;
}

int16_t rd_get_i16(rd_reader *r) { return (int16_t)rd_get_u16(r); }

uint32_t rd_get_u32(rd_reader *r) {
    const uint8_t *p = rd_get_bytes(r, 4);
    return p ? rd_rd32(p) : 0;
}

uint64_t rd_get_u64(rd_reader *r) {
    uint64_t lo = rd_get_u32(r);
    uint64_t hi = rd_get_u32(r);
    return lo | (hi << 32);
}

size_t rd_get_str(rd_reader *r, char *out, size_t cap) {
    uint16_t n = rd_get_u16(r);
    const uint8_t *p = rd_get_bytes(r, n);
    if (cap) {
        size_t c = 0;
        if (p) {
            c = n < cap - 1 ? n : cap - 1;
            memcpy(out, p, c);
        }
        out[c] = 0;
    }
    return p ? n : 0;
}

const uint8_t *rd_get_blob(rd_reader *r, uint32_t *n) {
    *n = rd_get_u32(r);
    const uint8_t *p = rd_get_bytes(r, *n);
    if (!p) *n = 0;
    return p;
}
