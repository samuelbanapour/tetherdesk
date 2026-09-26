#include "rd_font.h"

#include <math.h>
#include <stdlib.h>

#include "rd_codec.h"
#include "rd_font_data.h"

static uint8_t *g_cache[RD_FONT_FACE_COUNT];

int rd_font_count(void) { return RD_FONT_FACE_COUNT; }

const rd_font_face *rd_font_face_at(int index) {
    if (index < 0) index = 0;
    if (index >= RD_FONT_FACE_COUNT) index = RD_FONT_FACE_COUNT - 1;
    return &rd_font_faces[index];
}

int rd_font_best(float px) {
    int best = 0;
    float diff = 1e9f;
    for (int i = 0; i < RD_FONT_FACE_COUNT; i++) {
        float d = fabsf((float)rd_font_faces[i].size - px);
        if (d < diff) {
            diff = d;
            best = i;
        }
    }
    return best;
}

int rd_font_body(void) { return rd_font_best(14.0f); }

const uint8_t *rd_font_alpha(int index) {
    const rd_font_face *f = rd_font_face_at(index);
    index = (int)(f - rd_font_faces);
    if (!g_cache[index]) {
        size_t n = (size_t)f->atlas_w * (size_t)f->height;
        uint8_t *buf = (uint8_t *)calloc(n, 1);
        if (!buf) return NULL;
        if (rd_lz_decompress(f->lz_alpha, f->lz_len, buf, n) != (long)n) {
            free(buf);
            return NULL;
        }
        g_cache[index] = buf;
    }
    return g_cache[index];
}
