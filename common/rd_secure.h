/*
 * rd_secure.h - TetherDesk's encrypted channel (dependency-free C).
 *
 * Primitives: X25519 (RFC 7748), ChaCha20-Poly1305 AEAD (RFC 8439),
 * HKDF-SHA256 (RFC 5869).
 *
 * Handshake (a Noise-"NK"-like pattern, plus a password proof):
 *   viewer  -> host   HELLO  ce            (viewer ephemeral public key)
 *   host    -> viewer HELLO  ss, se, nonce (host static + ephemeral keys)
 *   both:  th   = SHA-256("TetherDesk v2" | ce | ss | se | nonce)
 *          ikm  = X25519(ce, se) | X25519(ce, ss)
 *          keys = HKDF(ikm, salt = th) -> viewer->host key, host->viewer key
 *   viewer -> host    AUTH   HMAC-SHA256(password, "tetherdesk-auth" | th)   [encrypted]
 *
 * Every message after the two HELLOs is sealed with ChaCha20-Poly1305 using
 * a per-direction key and a message counter as nonce, so traffic is
 * confidential, tamper-evident and replay-proof. Mixing in the host's static
 * key means only the real host can derive the keys; viewers pin its
 * fingerprint on first use (like SSH) to detect man-in-the-middle attacks.
 */
#ifndef RD_SECURE_H
#define RD_SECURE_H

#include <stddef.h>
#include <stdint.h>

#include "rd_bytes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_KEY_LEN 32
#define RD_TAG_LEN 16

/* X25519: out = scalar * point. Returns -1 if the result is all zeros
 * (a low-order point was supplied), which callers must treat as failure. */
int rd_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void rd_x25519_base(uint8_t pub[32], const uint8_t scalar[32]);
/* New random private key + matching public key. */
int rd_x25519_keypair(uint8_t priv[32], uint8_t pub[32]);

void rd_chacha20(uint8_t *out, const uint8_t *in, size_t len, const uint8_t key[32], const uint8_t nonce[12],
                 uint32_t counter);
void rd_poly1305(uint8_t tag[16], const uint8_t *msg, size_t len, const uint8_t key[32]);
void rd_aead_seal(uint8_t *out /* len + 16 */, const uint8_t *msg, size_t len, const uint8_t *ad, size_t ad_len,
                  const uint8_t key[32], const uint8_t nonce[12]);
/* Returns 0 and writes len-16 bytes of plaintext, or -1 if forged. */
int rd_aead_open(uint8_t *out, const uint8_t *in, size_t len, const uint8_t *ad, size_t ad_len,
                 const uint8_t key[32], const uint8_t nonce[12]);

void rd_hkdf_sha256(uint8_t *out, size_t out_len, const uint8_t *ikm, size_t ikm_len, const uint8_t *salt,
                    size_t salt_len, const uint8_t *info, size_t info_len);

/* One direction-pair of an established session. */
typedef struct rd_channel {
    uint8_t send_key[32], recv_key[32];
    uint64_t send_ctr, recv_ctr;
    int active;
} rd_channel;

/* Handshake helpers shared by host and viewers. */
void rd_hs_transcript(uint8_t th[32], const uint8_t ce[32], const uint8_t ss[32], const uint8_t se[32],
                      const uint8_t nonce[16]);
/* is_host selects which derived key is for sending. dh1 = X25519(e,e), dh2 = X25519(ce,ss). */
void rd_hs_keys(rd_channel *ch, const uint8_t dh1[32], const uint8_t dh2[32], const uint8_t th[32], int is_host);
void rd_hs_auth_mac(uint8_t out[32], const char *password, size_t pw_len, const uint8_t th[32]);
/* Human-readable fingerprint of a host public key: "ab12-cd34-...", 8 groups. */
void rd_fingerprint(const uint8_t pub[32], char out[40]);

/* Seal msg into out (appends len + 16 bytes). */
void rd_channel_seal(rd_channel *ch, const uint8_t *msg, size_t len, rd_buf *out);
/* Opens in place-safe: plaintext (len - 16 bytes) is written to out.
 * Returns plaintext length or -1 on forgery/replay. */
long rd_channel_open(rd_channel *ch, const uint8_t *in, size_t len, uint8_t *out);

/* Best-effort wipe that the compiler won't optimise away. */
void rd_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif
#endif
