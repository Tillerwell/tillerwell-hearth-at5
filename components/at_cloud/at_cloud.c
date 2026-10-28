#include "at_cloud.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/ecp.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"

#define KEY_NAME  "cloud_key"
#define CERT_NAME "cloud_cert"
#define KEY_DER_MAX 160   // SEC1 ECPrivateKey for P-256 is 121 bytes

static const at_cloud_io_t *s_io;

void at_cloud_init(const at_cloud_io_t *io) { s_io = io; }

static int rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    s_io->random(buf, len);
    return 0;
}

/* The stored key is the DER, padded with zeros to a fixed size (the settings store only reads exact lengths). */
static bool key_load(mbedtls_pk_context *pk)
{
    uint8_t der[KEY_DER_MAX];
    if (!s_io->load(KEY_NAME, der, sizeof(der))) return false;
    size_t n = KEY_DER_MAX;
    while (n > 0 && der[n - 1] == 0) n--;   // DER ends in a non-zero byte, so trailing zeros are all padding
    int rc = mbedtls_pk_parse_key(pk, der, n, NULL, 0, rng, NULL);
    memset(der, 0, sizeof(der));
    return rc == 0;
}

static bool key_make(mbedtls_pk_context *pk)
{
    if (mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) return false;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk), rng, NULL) != 0) return false;
    uint8_t buf[KEY_DER_MAX];
    int n = mbedtls_pk_write_key_der(pk, buf, sizeof(buf));   // written at the end of buf
    if (n <= 0) return false;
    uint8_t der[KEY_DER_MAX] = {0};
    memcpy(der, buf + sizeof(buf) - n, (size_t)n);
    int rc = s_io->save(KEY_NAME, der, sizeof(der));
    memset(buf, 0, sizeof(buf));
    memset(der, 0, sizeof(der));
    return rc == 0;
}

int at_cloud_csr(const char *device_id, char *out, size_t cap)
{
    mbedtls_pk_context pk;
    mbedtls_x509write_csr csr;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_csr_init(&csr);
    char subject[48];
    int rc = -1;
    if (snprintf(subject, sizeof(subject), "CN=%s", device_id) >= (int)sizeof(subject)) goto done;
    if (!key_load(&pk)) {
        mbedtls_pk_free(&pk);
        mbedtls_pk_init(&pk);
        if (!key_make(&pk)) goto done;
    }
    if (mbedtls_x509write_csr_set_subject_name(&csr, subject) != 0) goto done;
    mbedtls_x509write_csr_set_key(&csr, &pk);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    if (mbedtls_x509write_csr_pem(&csr, (unsigned char *)out, cap, rng, NULL) != 0) goto done;
    rc = 0;
done:
    mbedtls_x509write_csr_free(&csr);
    mbedtls_pk_free(&pk);
    return rc;
}

static bool same_public_key(mbedtls_pk_context *a, mbedtls_pk_context *b)
{
    uint8_t x[100], y[100];
    int nx = mbedtls_pk_write_pubkey_der(a, x, sizeof(x));
    int ny = mbedtls_pk_write_pubkey_der(b, y, sizeof(y));
    return nx > 0 && nx == ny && memcmp(x + sizeof(x) - nx, y + sizeof(y) - ny, (size_t)nx) == 0;
}

static bool subject_is(const mbedtls_x509_crt *crt, const char *cn)
{
    for (const mbedtls_x509_name *n = &crt->subject; n; n = n->next) {
        if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid) == 0) return n->val.len == strlen(cn) && memcmp(n->val.p, cn, n->val.len) == 0;
    }
    return false;
}

/* With no trustworthy clock a fresh box can't tell an expired certificate from a valid one; the TLS client checks
 * the dates properly once it has network time. */
static int vrfy(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)crt; (void)depth;
    if (!*(const bool *)ctx) *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    return 0;
}

const char *at_cloud_install(const char *chain_pem, const char *device_id)
{
    size_t len = strlen(chain_pem);
    if (len + 1 > AT_CLOUD_CERT_MAX) return "too_large";

    mbedtls_pk_context pk;
    mbedtls_x509_crt chain, root;
    mbedtls_pk_init(&pk);
    mbedtls_x509_crt_init(&chain);
    mbedtls_x509_crt_init(&root);
    const char *err = NULL;
    if (!key_load(&pk)) { err = "no_key"; goto done; }
    if (mbedtls_x509_crt_parse(&chain, (const unsigned char *)chain_pem, len + 1) != 0 ||
        mbedtls_x509_crt_parse(&root, (const unsigned char *)s_io->root_pem, strlen(s_io->root_pem) + 1) != 0) {
        err = "bad_cert";
        goto done;
    }
    bool dates = s_io->clock_ok();
    uint32_t flags = 0;
    if (mbedtls_x509_crt_verify(&chain, &root, NULL, NULL, &flags, vrfy, &dates) != 0) { err = "bad_chain"; goto done; }
    if (!subject_is(&chain, device_id)) { err = "wrong_device"; goto done; }
    if (!same_public_key(&pk, &chain.pk)) { err = "wrong_key"; goto done; }

    uint8_t stored[AT_CLOUD_CERT_MAX] = {0};
    memcpy(stored, chain_pem, len);
    if (s_io->save(CERT_NAME, stored, sizeof(stored)) != 0) err = "storage";
done:
    mbedtls_x509_crt_free(&root);
    mbedtls_x509_crt_free(&chain);
    mbedtls_pk_free(&pk);
    return err;
}

bool at_cloud_has_cert(void)
{
    static uint8_t buf[AT_CLOUD_CERT_MAX];
    return s_io->load(CERT_NAME, buf, sizeof(buf)) && buf[0] != 0;
}

void at_cloud_forget(void)
{
    s_io->erase(CERT_NAME);
    s_io->erase(KEY_NAME);
}
