/*
 * rd_codec.h - TetherDesk's screen codec.
 *
 * The screen is cut into RD_TILE x RD_TILE tiles; only tiles that changed
 * since the viewer's last acknowledged state are sent. Each tile is encoded
 * with whichever of these is smallest:
 *
 *   SOLID    one colour                       payload: r, g, b
 *   PALETTE  <= 256 colours (UI, text)        payload: u8 n-1, n*RGB, LZ(packed indices)
 *                                             indices use 1/2/4/8 bits, rows byte-aligned
 *   ZRGB     photographic / gradients         payload: LZ(MED-predicted, green-decorrelated RGB)
 *   RAW      incompressible fallback          payload: w*h*RGB
 *
 * "LZ" is a small LZ77 byte compressor using the LZ4 block layout (tokens,
 * 255-run lengths, 16-bit offsets). The decoder is fully bounds-checked,
 * since it runs on untrusted network input.
 *
 * Lossy quality levels simply clear low colour bits before encoding, which
 * makes more tiles fit a palette and gives LZ longer matches. The decoder
 * doesn't need to know the quality level.
 *
 * Pixels: the encoder reads 32-bit BGRA bytes (what every OS capture API
 * produces); the decoder writes 0xFFRRGGBB uint32s (SDL ARGB8888).
 */
#ifndef RD_CODEC_H
#define RD_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "rd_bytes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_TILE 64

enum { RD_ENC_SOLID = 0, RD_ENC_PALETTE = 1, RD_ENC_ZRGB = 2, RD_ENC_RAW = 3 };

size_t rd_lz_bound(size_t n);
/* Returns compressed size, or 0 if cap < rd_lz_bound(n). */
size_t rd_lz_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);
/* Returns decompressed size, or -1 on malformed input / overflow. */
long rd_lz_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);

/* Encodes one tile (w,h <= RD_TILE), appending its payload to `out`.
 * Returns the RD_ENC_* id used. Thread-safe (no shared state). */
int rd_encode_tile(const uint8_t *bgra, size_t stride_bytes, int w, int h, int quality, rd_buf *out);

/* Decodes a tile payload into dst (stride in pixels). Returns 0 or -1. */
int rd_decode_tile(int encoding, const uint8_t *payload, size_t len, int w, int h,
                   uint32_t *dst, size_t dst_stride_px);

#ifdef __cplusplus
}
#endif
#endif
