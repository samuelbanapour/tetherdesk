/*
 * rd_crypto.h - the small amount of cryptography TetherDesk needs, with no
 * external dependencies: SHA-1 (WebSocket accept keys), SHA-256 + HMAC
 * (challenge/response authentication), Base64, a CSPRNG wrapper and a
 * constant-time comparison.
 */
#ifndef RD_CRYPTO_H
#define RD_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rd_sha1(const void *data, size_t len, uint8_t out[20]);

typedef struct rd_sha256_ctx {
    uint32_t h[8];
    uint64_t total;
    uint8_t block[64];
    size_t fill;
} rd_sha256_ctx;

void rd_sha256_init(rd_sha256_ctx *c);
void rd_sha256_update(rd_sha256_ctx *c, const void *data, size_t len);
void rd_sha256_final(rd_sha256_ctx *c, uint8_t out[32]);
void rd_sha256(const void *data, size_t len, uint8_t out[32]);
void rd_hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len, uint8_t out[32]);

/* Writes NUL-terminated Base64 into out (needs 4*ceil(len/3)+1 bytes). */
void rd_base64(const uint8_t *in, size_t len, char *out);

/* Fills buf from the OS CSPRNG. Returns 0 on success. */
int rd_random(void *buf, size_t len);

/* Returns 1 if equal. Runs in time independent of where they differ. */
int rd_ct_equal(const void *a, const void *b, size_t len);

#ifdef __cplusplus
}
#endif
#endif
