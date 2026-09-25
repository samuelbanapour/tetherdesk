/*
 * rd_tls.h - non-blocking TLS client (mbedTLS) for talking to the internet
 * relay over HTTPS/WSS. The relay's certificate is verified against the
 * operating system's trusted root certificates.
 *
 * TLS here protects the hop to the relay (e.g. the host's ID-ownership key);
 * the remote-desktop session itself is separately end-to-end encrypted
 * between viewer and host (rd_secure.h) and never readable by the relay.
 */
#ifndef RD_TLS_H
#define RD_TLS_H

#include <stddef.h>

#include "rd_net.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rd_tls rd_tls;

/* Wraps an already-connected non-blocking socket. Returns NULL on failure. */
rd_tls *rd_tls_new(rd_socket s, const char *server_name);
void rd_tls_free(rd_tls *t);
/* 0 = handshake complete, 1 = in progress (call again later), -1 = failed. */
int rd_tls_handshake(rd_tls *t);
/* >0 bytes, 0 = would block, -1 = closed/error. */
long rd_tls_send(rd_tls *t, const void *p, size_t n);
long rd_tls_recv(rd_tls *t, void *p, size_t n);
/* Human-readable description of the last failure. */
const char *rd_tls_error(const rd_tls *t);

#ifdef __cplusplus
}
#endif
#endif
