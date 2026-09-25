#include "rd_ws.h"

#include <ctype.h>
#include <string.h>

#include "rd_crypto.h"

void rd_ws_accept_key(const char *client_key, char out[29]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char buf[256];
    size_t kl = strlen(client_key);
    if (kl > sizeof buf - sizeof guid) kl = sizeof buf - sizeof guid;
    memcpy(buf, client_key, kl);
    memcpy(buf + kl, guid, sizeof guid - 1);
    uint8_t digest[20];
    rd_sha1(buf, kl + sizeof guid - 1, digest);
    rd_base64(digest, 20, out);
}

void rd_ws_write_frame(rd_buf *out, int opcode, const void *payload, size_t len, int mask) {
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = (uint8_t)(0x80 | (opcode & 0x0F));
    uint8_t mbit = mask ? 0x80 : 0;
    if (len < 126) {
        hdr[h++] = (uint8_t)(mbit | len);
    } else if (len <= 0xFFFF) {
        hdr[h++] = (uint8_t)(mbit | 126);
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)len;
    } else {
        hdr[h++] = (uint8_t)(mbit | 127);
        for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    uint8_t key[4] = {0, 0, 0, 0};
    if (mask) {
        rd_random(key, 4);
        memcpy(hdr + h, key, 4);
        h += 4;
    }
    if (rd_buf_reserve(out, h + len) != 0) return;
    rd_buf_put(out, hdr, h);
    uint8_t *dst = rd_buf_grow(out, len);
    if (!dst || !len) return;
    memcpy(dst, payload, len);
    if (mask)
        for (size_t i = 0; i < len; i++) dst[i] ^= key[i & 3];
}

void rd_ws_reader_init(rd_ws_reader *r, size_t max_message, int expect_masked) {
    rd_buf_init(&r->assembly);
    r->assembly_opcode = -1;
    r->max_message = max_message;
    r->expect_masked = expect_masked;
}

void rd_ws_reader_free(rd_ws_reader *r) { rd_buf_free(&r->assembly); }

int rd_ws_next(rd_ws_reader *r, rd_buf *in, rd_ws_msg *msg) {
    for (;;) {
        if (in->len < 2) return 0;
        const uint8_t *p = in->data;
        int fin = p[0] & 0x80;
        int rsv = p[0] & 0x70;
        int opcode = p[0] & 0x0F;
        int masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7F;
        size_t h = 2;
        if (rsv) return -1; /* no extensions negotiated */
        if (masked != (r->expect_masked ? 0x80 : 0)) return -1;
        if (len == 126) {
            if (in->len < 4) return 0;
            len = ((uint64_t)p[2] << 8) | p[3];
            h = 4;
        } else if (len == 127) {
            if (in->len < 10) return 0;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | p[2 + i];
            h = 10;
        }
        if (len > r->max_message) return -1;
        uint8_t key[4] = {0, 0, 0, 0};
        if (masked) {
            if (in->len < h + 4) return 0;
            memcpy(key, p + h, 4);
            h += 4;
        }
        if (in->len < h + len) return 0;

        uint8_t *payload = in->data + h;
        if (masked)
            for (size_t i = 0; i < len; i++) payload[i] ^= key[i & 3];

        if (opcode >= 0x8) {
            /* Control frames: never fragmented, <= 125 bytes. They may arrive
             * between fragments, so they get their own buffer. */
            if (!fin || len > 125) return -1;
            memcpy(r->ctrl, payload, (size_t)len);
            rd_buf_consume(in, h + (size_t)len);
            msg->opcode = opcode;
            msg->data = r->ctrl;
            msg->len = (size_t)len;
            return 1;
        }

        if (opcode == RD_WS_CONT) {
            if (r->assembly_opcode < 0) return -1;
        } else {
            if (opcode != RD_WS_BINARY && opcode != RD_WS_TEXT) return -1;
            if (r->assembly_opcode >= 0) return -1; /* new message mid-fragment */
            r->assembly_opcode = opcode;
            rd_buf_clear(&r->assembly);
        }
        if (r->assembly.len + len > r->max_message) return -1;
        rd_buf_put(&r->assembly, payload, (size_t)len);
        rd_buf_consume(in, h + (size_t)len);
        if (!fin) continue;

        msg->opcode = r->assembly_opcode;
        msg->data = r->assembly.data;
        msg->len = r->assembly.len;
        r->assembly_opcode = -1;
        return 1;
    }
}

int rd_http_header(const char *headers, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    const char *line = headers;
    while (line && *line) {
        const char *eol = strstr(line, "\r\n");
        size_t ll = eol ? (size_t)(eol - line) : strlen(line);
        if (ll > nl && line[nl] == ':') {
            size_t i = 0;
            while (i < nl && tolower((unsigned char)line[i]) == tolower((unsigned char)name[i])) i++;
            if (i == nl) {
                const char *v = line + nl + 1;
                const char *e = line + ll;
                while (v < e && (*v == ' ' || *v == '\t')) v++;
                while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
                size_t vl = (size_t)(e - v);
                if (vl >= cap) vl = cap - 1;
                memcpy(out, v, vl);
                out[vl] = 0;
                return 1;
            }
        }
        line = eol ? eol + 2 : NULL;
    }
    return 0;
}
