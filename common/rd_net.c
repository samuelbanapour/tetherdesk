#include "rd_net.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#define WOULD_BLOCK(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
static int last_err(void) { return WSAGetLastError(); }
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define WOULD_BLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINPROGRESS)
static int last_err(void) { return errno; }
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int rd_net_init(void) {
#ifdef _WIN32
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void rd_net_close(rd_socket s) {
    if (s == RD_INVALID_SOCKET) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static void set_nonblocking(rd_socket s) {
#ifdef _WIN32
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void tune(rd_socket s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof one);
#endif
    int sz = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&sz, sizeof sz);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&sz, sizeof sz);
}

rd_socket rd_net_listen(const char *bind_addr, int port, char *err, size_t err_cap) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
    char ports[16];
    snprintf(ports, sizeof ports, "%d", port);
    int rc = getaddrinfo(bind_addr, ports, &hints, &res);
    if (rc != 0 || !res) {
        snprintf(err, err_cap, "invalid bind address '%s'", bind_addr);
        return RD_INVALID_SOCKET;
    }
    rd_socket s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == RD_INVALID_SOCKET) {
        snprintf(err, err_cap, "socket() failed (%d)", last_err());
        freeaddrinfo(res);
        return RD_INVALID_SOCKET;
    }
    int one = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#endif
    if (res->ai_family == AF_INET6) {
        int zero = 0; /* accept IPv4-mapped connections too */
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof zero);
    }
    if (bind(s, res->ai_addr, (int)res->ai_addrlen) != 0 || listen(s, 16) != 0) {
        snprintf(err, err_cap, "cannot listen on %s:%d (error %d) - is the port in use?", bind_addr, port,
                 last_err());
        rd_net_close(s);
        freeaddrinfo(res);
        return RD_INVALID_SOCKET;
    }
    freeaddrinfo(res);
    set_nonblocking(s);
    return s;
}

rd_socket rd_net_accept(rd_socket listener, char *addr, size_t addr_cap) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    rd_socket s = accept(listener, (struct sockaddr *)&ss, &sl);
    if (s == RD_INVALID_SOCKET) return s;
    char host[INET6_ADDRSTRLEN] = "?";
    if (ss.ss_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, host, sizeof host);
    else if (ss.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, host, sizeof host);
    /* Present IPv4-mapped addresses in their familiar dotted form. */
    const char *h = strncmp(host, "::ffff:", 7) == 0 && strchr(host + 7, '.') ? host + 7 : host;
    snprintf(addr, addr_cap, "%s", h);
    set_nonblocking(s);
    tune(s);
    return s;
}

rd_socket rd_net_connect_start(const char *host, int port, char *err, size_t err_cap) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ports[16];
    snprintf(ports, sizeof ports, "%d", port);
    if (getaddrinfo(host, ports, &hints, &res) != 0 || !res) {
        snprintf(err, err_cap, "cannot resolve '%s'", host);
        return RD_INVALID_SOCKET;
    }
    rd_socket s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == RD_INVALID_SOCKET) {
        snprintf(err, err_cap, "socket() failed");
        freeaddrinfo(res);
        return s;
    }
    set_nonblocking(s);
    tune(s);
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0 && !WOULD_BLOCK(last_err())) {
        snprintf(err, err_cap, "connect to %s:%d failed (%d)", host, port, last_err());
        rd_net_close(s);
        s = RD_INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

int rd_net_connect_result(rd_socket s) {
    int e = 0;
    socklen_t l = sizeof e;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&e, &l) != 0) return -1;
    return e;
}

long rd_net_send(rd_socket s, const void *p, size_t n) {
#ifdef _WIN32
    int r = send(s, (const char *)p, (int)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n), 0);
#else
    ssize_t r = send(s, p, n, MSG_NOSIGNAL);
#endif
    if (r >= 0) return (long)r;
    return WOULD_BLOCK(last_err()) ? 0 : -1;
}

long rd_net_recv(rd_socket s, void *p, size_t n) {
#ifdef _WIN32
    int r = recv(s, (char *)p, (int)n, 0);
#else
    ssize_t r = recv(s, p, n, 0);
#endif
    if (r > 0) return (long)r;
    if (r == 0) return -1; /* orderly shutdown */
    return WOULD_BLOCK(last_err()) ? 0 : -1;
}

int rd_poll(rd_pollfd *fds, size_t n, int timeout_ms) {
#ifdef _WIN32
    return WSAPoll(fds, (ULONG)n, timeout_ms);
#else
    return poll(fds, (nfds_t)n, timeout_ms);
#endif
}

uint64_t rd_now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / freq.QuadPart * 1000000 + c.QuadPart % freq.QuadPart * 1000000 / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}
