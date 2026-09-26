/*
 * rd_font.h - the embedded UI font (Aileron, CC0) at many exact pixel sizes.
 * Text is drawn with the face whose size matches the wanted pixel size, so
 * glyphs are never scaled (scaling is what makes small text look blurry).
 */
#ifndef RD_FONT_H
#define RD_FONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rd_glyph {
    uint16_t x, w, advance;
} rd_glyph;

typedef struct rd_font_face {
    int size, ascent, height, atlas_w;
    const rd_glyph *glyphs;   /* ASCII 32..126 */
    const uint8_t *lz_alpha;  /* LZ-compressed atlas_w x height alpha */
    uint32_t lz_len;
} rd_font_face;

int rd_font_count(void);
const rd_font_face *rd_font_face_at(int index);
/* Index of the face closest to `pixel_size`. */
int rd_font_best(float pixel_size);
/* The face's alpha atlas, decompressed on first use and cached (not thread-safe). */
const uint8_t *rd_font_alpha(int index);
/* Face index of the 14 px "body text" size (used for 1x layout metrics). */
int rd_font_body(void);

#ifdef __cplusplus
}
#endif
#endif
