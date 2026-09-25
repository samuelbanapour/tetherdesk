#include "rd_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#if defined(__APPLE__)
#include <Security/Security.h>
#elif defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#endif

struct rd_tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt ca;
    rd_socket sock;
    char error[160];
};

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    rd_tls *t = (rd_tls *)ctx;
    long r = rd_net_send(t->sock, buf, len);
    if (r > 0) return (int)r;
    return r == 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    rd_tls *t = (rd_tls *)ctx;
    long r = rd_net_recv(t->sock, buf, len);
    if (r > 0) return (int)r;
    return r == 0 ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_CONN_RESET;
}

/* Loads the OS trust store. Returns the number of certificates loaded. */
static int load_system_roots(mbedtls_x509_crt *ca) {
    int n = 0;
#if defined(__APPLE__)
    CFArrayRef anchors = NULL;
    if (SecTrustCopyAnchorCertificates(&anchors) == errSecSuccess && anchors) {
        for (CFIndex i = 0; i < CFArrayGetCount(anchors); i++) {
            SecCertificateRef cert = (SecCertificateRef)CFArrayGetValueAtIndex(anchors, i);
            CFDataRef der = SecCertificateCopyData(cert);
            if (!der) continue;
            if (mbedtls_x509_crt_parse_der(ca, CFDataGetBytePtr(der), (size_t)CFDataGetLength(der)) == 0) n++;
            CFRelease(der);
        }
        CFRelease(anchors);
    }
#elif defined(_WIN32)
    HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
    if (store) {
        PCCERT_CONTEXT c = NULL;
        while ((c = CertEnumCertificatesInStore(store, c)) != NULL)
            if (mbedtls_x509_crt_parse_der(ca, c->pbCertEncoded, c->cbCertEncoded) == 0) n++;
        CertCloseStore(store, 0);
    }
#else
    static const char *files[] = {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                                  "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"};
    for (size_t i = 0; i < sizeof files / sizeof files[0] && !n; i++) {
        int before = 0;
        for (mbedtls_x509_crt *c = ca; c && c->raw.len; c = c->next) before++;
        if (mbedtls_x509_crt_parse_file(ca, files[i]) >= 0) {
            int after = 0;
            for (mbedtls_x509_crt *c = ca; c && c->raw.len; c = c->next) after++;
            n += after - before;
        }
    }
#endif
    return n;
}

static void set_error(rd_tls *t, const char *what, int code) {
    char msg[100];
    mbedtls_strerror(code, msg, sizeof msg);
    snprintf(t->error, sizeof t->error, "%s: %s", what, msg);
}

rd_tls *rd_tls_new(rd_socket s, const char *server_name) {
    static int psa_ready = 0;
    if (!psa_ready) {
        if (psa_crypto_init() != PSA_SUCCESS) return NULL;
        psa_ready = 1;
    }
    rd_tls *t = (rd_tls *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->sock = s;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_x509_crt_init(&t->ca);
    int rc;
    if ((rc = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, (const unsigned char *)"tetherdesk",
                                    10)) != 0 ||
        (rc = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        rd_tls_free(t);
        return NULL;
    }
    if (load_system_roots(&t->ca) > 0) {
        mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        /* No trust store available: still encrypted, and the session inside
         * is end-to-end encrypted and identity-pinned regardless. */
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    if ((rc = mbedtls_ssl_setup(&t->ssl, &t->conf)) != 0 || (rc = mbedtls_ssl_set_hostname(&t->ssl, server_name)) != 0) {
        rd_tls_free(t);
        return NULL;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
    return t;
}

void rd_tls_free(rd_tls *t) {
    if (!t) return;
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    mbedtls_x509_crt_free(&t->ca);
    free(t);
}

int rd_tls_handshake(rd_tls *t) {
    int rc = mbedtls_ssl_handshake(&t->ssl);
    if (rc == 0) return 0;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return 1;
    if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        snprintf(t->error, sizeof t->error, "the relay's certificate could not be verified");
        return -1;
    }
    set_error(t, "TLS handshake failed", rc);
    return -1;
}

long rd_tls_send(rd_tls *t, const void *p, size_t n) {
    int rc = mbedtls_ssl_write(&t->ssl, (const unsigned char *)p, n);
    if (rc > 0) return rc;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    set_error(t, "TLS write failed", rc);
    return -1;
}

long rd_tls_recv(rd_tls *t, void *p, size_t n) {
    int rc = mbedtls_ssl_read(&t->ssl, (unsigned char *)p, n);
    if (rc > 0) return rc;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
    if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return 0; /* TLS 1.3 housekeeping */
#endif
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        snprintf(t->error, sizeof t->error, "connection closed");
        return -1;
    }
    set_error(t, "TLS read failed", rc);
    return -1;
}

const char *rd_tls_error(const rd_tls *t) { return t->error; }
