/*
 * rd_ws.h - a minimal RFC 6455 WebSocket implementation for both roles.
 *
 * The host is a WebSocket server (browsers connect to it directly); the
 * native viewer is a WebSocket client. Frames are parsed from a receive
 * buffer, fragmented messages are reassembled, and control frames
 * (ping/pong/close) are surfaced to the caller.
 */
#ifndef RD_WS_H
#define RD_WS_H

#include <stddef.h>
#include <stdint.h>

#include "rd_bytes.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RD_WS_CONT = 0x0,
    RD_WS_TEXT = 0x1,
    RD_WS_BINARY = 0x2,
    RD_WS_CLOSE = 0x8,
    RD_WS_PING = 0x9,
    RD_WS_PONG = 0xA
};

/* Computes Sec-WebSocket-Accept for a client key (out: 29 bytes incl. NUL). */
void rd_ws_accept_key(const char *client_key, char out[29]);

/* Appends one frame. Clients must set `mask` (RFC 6455 section 5.3). */
void rd_ws_write_frame(rd_buf *out, int opcode, const void *payload, size_t len, int mask);

typedef struct rd_ws_msg {
    int opcode;          /* RD_WS_BINARY/TEXT for data, or a control opcode */
    const uint8_t *data; /* valid until the next rd_ws_next() call */
    size_t len;
} rd_ws_msg;

typedef struct rd_ws_reader {
    rd_buf assembly;     /* fragments of an in-progress data message */
    int assembly_opcode;
    size_t max_message;  /* larger messages are a protocol error */
    int expect_masked;   /* servers require masked frames, clients unmasked */
    uint8_t ctrl[125];   /* payload of the last control frame */
} rd_ws_reader;

void rd_ws_reader_init(rd_ws_reader *r, size_t max_message, int expect_masked);
void rd_ws_reader_free(rd_ws_reader *r);

/* Extracts the next complete message from `in` (consuming its bytes).
 * Returns 1 if *msg was filled, 0 if more data is needed, -1 on protocol
 * error (caller should close the connection). */
int rd_ws_next(rd_ws_reader *r, rd_buf *in, rd_ws_msg *msg);

/* HTTP header helper: finds `name` (case-insensitive) in a NUL-terminated
 * header block and copies its trimmed value. Returns 1 if found. */
int rd_http_header(const char *headers, const char *name, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
