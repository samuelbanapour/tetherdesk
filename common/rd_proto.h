/*
 * rd_proto.h - the TetherDesk wire protocol.
 *
 * Transport: WebSocket (RFC 6455) binary messages, so browsers and native
 * clients speak exactly the same protocol to the host. Each WebSocket message
 * carries one TetherDesk message: a u8 type followed by its payload.
 * Integers are little-endian. "str" = u16 length + UTF-8, "blob" = u32
 * length + bytes.
 *
 * Handshake (see rd_secure.h for the cryptography)
 *   C->S HELLO        u16 version, str viewer_name, u8[32] viewer_ephemeral_key
 *   S->C HELLO        u16 version, u8 auth_method(2 = X25519/ChaCha20-Poly1305),
 *                     u8[16] nonce, str host_name, str host_os,
 *                     u8[32] host_static_key, u8[32] host_ephemeral_key
 *   ---- every later message, both directions, is AEAD-encrypted ----
 *   C->S AUTH         u8[32] HMAC-SHA256(password, "tetherdesk-auth" | transcript)
 *   S->C AUTH_RESULT  u8 ok, u8 view_only, str message
 *   (on success the host follows with DISPLAY_INFO, VIEWERS and a full FRAME)
 *
 * Screen
 *   S->C DISPLAY_INFO u16 width, u16 height, u8 current, u8 count,
 *                     count x { str name, u16 width, u16 height },
 *                     u8 flags (bit0: can switch resolution, bit1: full resolution on)
 *   S->C FRAME        u32 frame_id, u8 flags, u16 tile_count,
 *                     tile_count x { u16 x, u16 y, u16 w, u16 h, u8 encoding,
 *                                    u32 length, u8[length] payload }
 *   C->S FRAME_ACK    u32 frame_id  (flow control: host keeps <= 2 unacked)
 *   C->S REFRESH      (empty)  request a full-screen update
 *   C->S SETTINGS     u8 quality(0..3, 255 auto), u8 max_fps(1..60), u8 display,
 *                     u8 resolution (0 full/Retina, 1 half/fast, 255 unchanged)
 *
 * Input (ignored for view-only viewers)
 *   C->S POINTER      u16 x, u16 y, u8 buttons, i16 wheel_x, i16 wheel_y
 *   C->S KEY          u16 usb_hid_usage, u8 down
 *
 * Collaboration
 *   C->S CLIPBOARD / S->C CLIPBOARD   blob utf8_text
 *   C->S CHAT         str text
 *   S->C CHAT         str from, str text
 *   S->C VIEWERS      u8 count, count x { str name, str address, u8 view_only }
 *   S->C NOTICE       str text
 *   C->S PING         u64 client_time_us      S->C PONG  u64 (echoed)
 *
 * File transfer (viewer -> host, saved into the host's Downloads folder)
 *   C->S FILE_BEGIN   u32 id, u64 size, str file_name
 *   C->S FILE_CHUNK   u32 id, u8[...] data (rest of message)
 *   C->S FILE_END     u32 id
 *   S->C FILE_RESULT  u32 id, u8 ok, str message
 */
#ifndef RD_PROTO_H
#define RD_PROTO_H

#define RD_PROTO_VERSION 2
#define RD_DEFAULT_PORT 5980
#define RD_NONCE_LEN 16
#define RD_MAC_LEN 32

/* Largest message a host accepts from a viewer (file chunks are 64 KiB). */
#define RD_MAX_CLIENT_MSG (2u * 1024u * 1024u)
/* Largest message a viewer accepts from a host (a full 8K frame, lossless). */
#define RD_MAX_SERVER_MSG (256u * 1024u * 1024u)
#define RD_MAX_CLIPBOARD (1u * 1024u * 1024u)
#define RD_FILE_CHUNK (64u * 1024u)
#define RD_MAX_FILE_SIZE (4ull * 1024ull * 1024ull * 1024ull)

enum {
    /* viewer -> host */
    RD_C_HELLO = 1,
    RD_C_AUTH = 2,
    RD_C_FRAME_ACK = 3,
    RD_C_POINTER = 4,
    RD_C_KEY = 5,
    RD_C_CLIPBOARD = 6,
    RD_C_SETTINGS = 7,
    RD_C_CHAT = 8,
    RD_C_PING = 9,
    RD_C_FILE_BEGIN = 10,
    RD_C_FILE_CHUNK = 11,
    RD_C_FILE_END = 12,
    RD_C_REFRESH = 13,

    /* host -> viewer */
    RD_S_HELLO = 64,
    RD_S_AUTH_RESULT = 65,
    RD_S_DISPLAY_INFO = 66,
    RD_S_FRAME = 67,
    RD_S_CLIPBOARD = 68,
    RD_S_CHAT = 69,
    RD_S_PONG = 70,
    RD_S_VIEWERS = 71,
    RD_S_NOTICE = 72,
    RD_S_FILE_RESULT = 73
};

enum { RD_AUTH_X25519_CHACHA = 2 };

enum {
    RD_BTN_LEFT = 1,
    RD_BTN_MIDDLE = 2,
    RD_BTN_RIGHT = 4,
    RD_BTN_X1 = 8,
    RD_BTN_X2 = 16
};

enum { RD_FRAME_FULL = 1 };

/* Quality levels: 0 = lossless, 1..3 progressively drop low colour bits. */
enum { RD_QUALITY_LOSSLESS = 0, RD_QUALITY_HIGH = 1, RD_QUALITY_MEDIUM = 2, RD_QUALITY_LOW = 3 };

/* USB HID usages (identical to SDL scancodes) the host treats specially. */
enum {
    RD_HID_DELETE = 76,
    RD_HID_LCTRL = 224,
    RD_HID_LSHIFT = 225,
    RD_HID_LALT = 226,
    RD_HID_LGUI = 227,
    RD_HID_RCTRL = 228,
    RD_HID_RSHIFT = 229,
    RD_HID_RALT = 230,
    RD_HID_RGUI = 231,
    RD_HID_MAX = 256
};

#endif
