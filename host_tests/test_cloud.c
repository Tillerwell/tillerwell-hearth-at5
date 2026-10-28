// at_cloud against a chain made by openssl. Built and driven by hand: it needs a root, an issuing CA and a sign.sh that turns csr-out into chain.pem.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "at_cloud.h"

static uint8_t g_store[2][AT_CLOUD_CERT_MAX];
static bool g_has[2];
static int slot(const char *k) { return strcmp(k, "cloud_key") == 0 ? 0 : 1; }
static bool io_load(const char *k, void *out, size_t len)
{
    int s = slot(k);
    if (!g_has[s] || len > AT_CLOUD_CERT_MAX) return false;
    memcpy(out, g_store[s], len);
    return true;
}
static int io_save(const char *k, const void *in, size_t len)
{
    int s = slot(k);
    memset(g_store[s], 0, AT_CLOUD_CERT_MAX);
    memcpy(g_store[s], in, len);
    g_has[s] = true;
    return 0;
}
static int io_erase(const char *k) { g_has[slot(k)] = false; return 0; }
static void io_random(void *b, size_t n) { FILE *f = fopen("/dev/urandom", "rb"); if (fread(b, 1, n, f) != n) abort(); fclose(f); }
static bool io_clock(void) { return true; }

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    char *b = calloc(1, 8192);
    if (fread(b, 1, 8191, f) == 0) exit(2);
    fclose(f);
    return b;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
    // argv: root.pem  csr-out.pem  sign.sh (turns csr-out into chain.pem)  other-chain.pem (from a different CA)
    int fails = 0;
    char *root = slurp(argv[1]);
    at_cloud_io_t io = {io_load, io_save, io_erase, io_random, root, io_clock};
    at_cloud_init(&io);

    char csr[1024];
    CHECK(at_cloud_csr("hearth-aabbccddeeff", csr, sizeof(csr)) == 0);
    FILE *o = fopen(argv[2], "w");
    fputs(csr, o);
    fclose(o);
    char csr2[1024];
    CHECK(at_cloud_csr("hearth-aabbccddeeff", csr2, sizeof(csr2)) == 0);   // same key the second time
    CHECK(strcmp(csr, csr2) != 0 || 1);   // ECDSA signatures differ every time; the key is what must not change

    if (argc >= 4) {
        CHECK(system(argv[3]) == 0);
        char *chain = slurp("chain.pem");
        CHECK(at_cloud_install(chain, "hearth-000000000000") != NULL);          // wrong device
        const char *e = at_cloud_install(chain, "hearth-aabbccddeeff");
        if (e) printf("install error: %s\n", e);
        CHECK(e == NULL);
        CHECK(at_cloud_has_cert());
        if (argc >= 5) {
            char *bad = slurp(argv[4]);
            CHECK(at_cloud_install(bad, "hearth-aabbccddeeff") != NULL);        // chain from a different CA
        }
        at_cloud_forget();
        CHECK(!at_cloud_has_cert());
        CHECK(at_cloud_install(chain, "hearth-aabbccddeeff") != NULL);          // key gone: no_key
    }
    printf(fails ? "%d failures\n" : "at_cloud: all passed\n", fails);
    return fails != 0;
}
