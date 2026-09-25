/*
 * rd_bytes.h - growable byte buffers and bounds-checked little-endian readers.
 *
 * Every multi-byte integer on the TetherDesk wire is little-endian. Readers
 * never read past the end of their input: on underflow they set `err` and
 * return zeros, so message handlers can parse optimistically and check
 * `r.err` once at the end.
 */
#ifndef RD_BYTES_H
#define RD_BYTES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rd_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} rd_buf;

void rd_buf_init(rd_buf *b);
void rd_buf_free(rd_buf *b);
void rd_buf_clear(rd_buf *b);
/* Ensure room for `extra` more bytes. Returns 0 on success, -1 on OOM. */
int rd_buf_reserve(rd_buf *b, size_t extra);
/* Append `n` uninitialised bytes and return a pointer to them (NULL on OOM). */
uint8_t *rd_buf_grow(rd_buf *b, size_t n);
void rd_buf_put(rd_buf *b, const void *p, size_t n);
void rd_buf_put_u8(rd_buf *b, uint8_t v);
void rd_buf_put_u16(rd_buf *b, uint16_t v);
void rd_buf_put_u32(rd_buf *b, uint32_t v);
void rd_buf_put_u64(rd_buf *b, uint64_t v);
/* Short string: u16 length prefix + UTF-8 bytes (truncated to 65535). */
void rd_buf_put_str(rd_buf *b, const char *s);
/* Long blob: u32 length prefix + bytes. */
void rd_buf_put_blob(rd_buf *b, const void *p, uint32_t n);
/* Drop the first `n` bytes (used for socket send/receive queues). */
void rd_buf_consume(rd_buf *b, size_t n);
/* Overwrite a u32 at an absolute offset (for back-patching counts). */
void rd_buf_patch_u32(rd_buf *b, size_t off, uint32_t v);
void rd_buf_patch_u16(rd_buf *b, size_t off, uint16_t v);

typedef struct rd_reader {
    const uint8_t *p;
    size_t len;
    size_t pos;
    int err;
} rd_reader;

void rd_reader_init(rd_reader *r, const void *p, size_t len);
uint8_t rd_get_u8(rd_reader *r);
uint16_t rd_get_u16(rd_reader *r);
int16_t rd_get_i16(rd_reader *r);
uint32_t rd_get_u32(rd_reader *r);
uint64_t rd_get_u64(rd_reader *r);
/* Returns a pointer into the input and advances; NULL (and err) on underflow. */
const uint8_t *rd_get_bytes(rd_reader *r, size_t n);
/* Reads a u16-prefixed string into `out` (always NUL-terminated, truncated
 * to cap-1). Returns the full wire length. */
size_t rd_get_str(rd_reader *r, char *out, size_t cap);
/* Reads a u32-prefixed blob, returning a pointer into the input. */
const uint8_t *rd_get_blob(rd_reader *r, uint32_t *n);
size_t rd_remaining(const rd_reader *r);

static inline void rd_wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void rd_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t rd_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#ifdef __cplusplus
}
#endif
#endif
