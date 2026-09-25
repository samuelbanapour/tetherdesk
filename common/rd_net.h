/*
 * rd_net.h - thin portability layer over BSD sockets / Winsock.
 * All sockets are non-blocking; callers drive them with rd_poll().
 */
#ifndef RD_NET_H
#define RD_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET rd_socket;
#define RD_INVALID_SOCKET INVALID_SOCKET
typedef WSAPOLLFD rd_pollfd;
#else
#include <poll.h>
typedef int rd_socket;
#define RD_INVALID_SOCKET (-1)
typedef struct pollfd rd_pollfd;
#endif

#ifdef __cplusplus
extern "C" {
#endif

int rd_net_init(void);
void rd_net_close(rd_socket s);

/* Listen on bind_addr ("0.0.0.0", "::", "127.0.0.1"...). Returns socket or invalid. */
rd_socket rd_net_listen(const char *bind_addr, int port, char *err, size_t err_cap);
/* Accepts a pending connection; fills a printable peer address. */
rd_socket rd_net_accept(rd_socket listener, char *addr, size_t addr_cap);
/* Starts a non-blocking connect (resolving host). Completion: poll for POLLOUT
 * then rd_net_connect_result(). */
rd_socket rd_net_connect_start(const char *host, int port, char *err, size_t err_cap);
/* 0 = connected, otherwise the socket error code. */
int rd_net_connect_result(rd_socket s);

/* >0 bytes, 0 = would block, -1 = closed/error. */
long rd_net_send(rd_socket s, const void *p, size_t n);
long rd_net_recv(rd_socket s, void *p, size_t n);

int rd_poll(rd_pollfd *fds, size_t n, int timeout_ms);

/* Monotonic clock in microseconds. */
uint64_t rd_now_us(void);

#ifdef __cplusplus
}
#endif
#endif
