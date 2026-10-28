// The box's cloud identity (docs/API_SPEC.md §8): a P-256 key made once, the certificate request for it, and the
// certificate chain the Tillerwell issuing CA returns. Pure logic over mbedTLS; the platform (NVS, random numbers,
// the pinned root, the clock) comes in through at_cloud_io_t so it can be built on the host.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_CLOUD_CERT_MAX 3072   // the stored chain, PEM, NUL-padded

typedef struct {
    bool (*load)(const char *key, void *out, size_t len);
    int (*save)(const char *key, const void *in, size_t len);   // 0 = ok
    int (*erase)(const char *key);                              // 0 = ok (or already gone)
    void (*random)(void *buf, size_t len);
    const char *root_pem;       // the pinned Tillerwell Hearth Root CA, NUL-terminated
    bool (*clock_ok)(void);     // false while the time is not trustworthy: validity dates are then not checked
} at_cloud_io_t;

void at_cloud_init(const at_cloud_io_t *io);
// ESP-IDF only: wires it to NVS, the RNG, the pinned root and the clock. Call once after at_settings_init.
void at_cloud_start(void);

// Makes the key on first use, then writes a PEM certificate request with subject exactly CN=<device_id>.
// 0 = ok, else -1.
int at_cloud_csr(const char *device_id, char *out, size_t cap);

// Checks a PEM chain (leaf first) ends at the pinned root, that the leaf is CN=<device_id> and carries this box's
// key, and stores it. NULL = installed, else the error code ("no_key", "bad_cert", "bad_chain", "wrong_device",
// "wrong_key", "too_large", "storage").
const char *at_cloud_install(const char *chain_pem, const char *device_id);

bool at_cloud_has_cert(void);
// Forgets the certificate and the key (the next enrolment makes a new one).
void at_cloud_forget(void);

#ifdef __cplusplus
}
#endif
