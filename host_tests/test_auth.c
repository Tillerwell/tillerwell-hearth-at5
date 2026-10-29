// Host test for components/at_auth against docs/auth-vectors.json (via
// auth_vectors.h). Built with ESP-IDF's own mbedTLS and cJSON sources, so the
// signature checks run on the same library the box uses.
//   make -C host_tests auth
#include <stdio.h>
#include <string.h>

#include "at_auth.h"
#include "auth_vectors.h"

static int failures, checks;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        checks++;                                                               \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

#define N(a) (sizeof(a) / sizeof((a)[0]))

static const av_key_t *key_named(const char *name)
{
    for (size_t i = 0; i < N(AV_KEYS); i++)
        if (strcmp(AV_KEYS[i].name, name) == 0) return &AV_KEYS[i];
    return NULL;
}

static void pub_of(const av_key_t *k, uint8_t pub[AT_AUTH_PUBKEY_LEN])
{
    int n = at_b64url_decode(k->pub_b64url, strlen(k->pub_b64url), pub, AT_AUTH_PUBKEY_LEN);
    CHECK(n == AT_AUTH_PUBKEY_LEN, "public key %s decodes to %d bytes", k->name, n);
}

// The box's user table in these tests: the admin and the invited guest. The
// "outsider" key is deliberately unknown to it.
static bool lookup_user(const char *kid, uint8_t pub[AT_AUTH_PUBKEY_LEN], at_perm_t *perm)
{
    static const char *const known[] = { "admin", "guest" };
    for (size_t i = 0; i < N(known); i++) {
        const av_key_t *k = key_named(known[i]);
        if (strcmp(k->key_id, kid) == 0) {
            pub_of(k, pub);
            *perm = strcmp(known[i], "admin") == 0 ? AT_PERM_ADMIN : AT_PERM_CONTROL;
            return true;
        }
    }
    return false;
}

static void test_base64url(void)
{
    const uint8_t data[] = { 0xfb, 0xff, 0x00, 0x10, 0x83 };
    char enc[16];
    uint8_t dec[8];
    for (size_t len = 0; len <= sizeof(data); len++) {
        size_t n = at_b64url_encode(data, len, enc, sizeof(enc));
        CHECK(strchr(enc, '=') == NULL && strchr(enc, '+') == NULL && strchr(enc, '/') == NULL, "no padding or +/");
        int m = at_b64url_decode(enc, n, dec, sizeof(dec));
        CHECK(m == (int)len && memcmp(dec, data, len) == 0, "round trip of %zu bytes", len);
    }
    CHECK(at_b64url_decode("ab=c", 4, dec, sizeof(dec)) < 0, "padding rejected");
    CHECK(at_b64url_decode("ab+c", 4, dec, sizeof(dec)) < 0, "standard alphabet rejected");
    CHECK(at_b64url_decode("A", 1, dec, sizeof(dec)) < 0, "impossible length rejected");
    CHECK(at_b64url_decode("AB", 2, dec, sizeof(dec)) < 0, "non-zero leftover bits rejected");
}

static void test_keys(void)
{
    for (size_t i = 0; i < N(AV_KEYS); i++) {
        uint8_t pub[AT_AUTH_PUBKEY_LEN];
        char kid[AT_AUTH_KID_LEN + 1];
        pub_of(&AV_KEYS[i], pub);
        at_auth_key_id(pub, kid);
        CHECK(strcmp(kid, AV_KEYS[i].key_id) == 0, "key id of %s: got %s want %s", AV_KEYS[i].name, kid, AV_KEYS[i].key_id);
    }
}

static void test_requests(void)
{
    for (size_t i = 0; i < N(AV_REQUESTS); i++) {
        const av_request_t *r = &AV_REQUESTS[i];
        at_auth_header_t h;
        CHECK(at_auth_parse_header(r->header, &h), "[%s] header parses", r->name);
        CHECK(strcmp(h.kid, key_named(r->signer)->key_id) == 0, "[%s] header kid", r->name);

        // The box rebuilds what it expects to have been signed from what it received.
        char sts[512];
        size_t len = at_auth_string_to_sign(sts, sizeof(sts), AV_DEVICE_ID, r->method, r->path, h.nonce,
                                            (const uint8_t *)r->body, strlen(r->body));
        CHECK(len > 0, "[%s] string to sign fits", r->name);
        bool valid = strcmp(r->expect, "valid") == 0;
        if (valid) CHECK(strcmp(sts, r->string_to_sign) == 0, "[%s] string to sign matches the vector", r->name);

        uint8_t pub[AT_AUTH_PUBKEY_LEN];
        at_perm_t perm;
        CHECK(lookup_user(h.kid, pub, &perm), "[%s] signer known", r->name);
        bool ok = at_auth_verify(pub, (const uint8_t *)sts, len, h.sig);
        CHECK(ok == valid, "[%s] verify gave %s, want %s", r->name, ok ? "valid" : "invalid", r->expect);
    }
    at_auth_header_t h;
    CHECK(!at_auth_parse_header("v2 kid=87etahFmchg nonce=x sig=AAAA", &h), "unknown version rejected");
    CHECK(!at_auth_parse_header("v1 kid=short nonce=x sig=AAAA", &h), "short kid rejected");
}

static void test_grants(void)
{
    for (size_t i = 0; i < N(AV_GRANTS); i++) {
        const av_grant_t *g = &AV_GRANTS[i];
        at_grant_t peek, grant;
        CHECK(at_auth_grant_peek(g->token, &peek) == AT_AUTH_OK, "[%s] grant parses", g->name);

        // The QR carries exactly this token, between the fingerprint and the guest's key.
        char want_prefix[64];
        snprintf(want_prefix, sizeof(want_prefix), "hearth-invite:1:%s:", AV_DEVICE_ID);
        CHECK(strncmp(g->qr, want_prefix, strlen(want_prefix)) == 0 && strstr(g->qr, g->token), "[%s] QR layout", g->name);

        uint8_t admin_pub[AT_AUTH_PUBKEY_LEN];
        at_perm_t signer_perm = AT_PERM_NONE;
        at_auth_result_t res;
        if (!lookup_user(peek.by, admin_pub, &signer_perm) || signer_perm != AT_PERM_ADMIN)
            res = AT_AUTH_UNKNOWN_SIGNER;
        else
            res = at_auth_grant_verify(g->token, admin_pub, &grant);
        CHECK(strcmp(at_auth_result_name(res), g->expect) == 0 || (res == AT_AUTH_OK && strcmp(g->expect, "valid") == 0),
              "[%s] got %s, want %s", g->name, at_auth_result_name(res), g->expect);

        if (res == AT_AUTH_OK) {
            const av_key_t *guest = key_named("guest");
            CHECK(strcmp(grant.dev, AV_DEVICE_ID) == 0, "[%s] grant device", g->name);
            CHECK(strcmp(grant.key_id, guest->key_id) == 0, "[%s] granted key is the guest's", g->name);
            CHECK(grant.perm == AT_PERM_CONTROL, "[%s] grant perm", g->name);
            CHECK(grant.redeem_by == AV_NOW + 900 && grant.exp == 0, "[%s] grant times", g->name);
            CHECK(strcmp(grant.label, "Sam's phone") == 0, "[%s] grant label", g->name);
        }
    }
}

static void test_commands(void)
{
    for (size_t i = 0; i < N(AV_COMMANDS); i++) {
        const av_command_t *c = &AV_COMMANDS[i];
        at_cloud_cmd_t peek, cmd;
        CHECK(at_auth_cloud_cmd_peek(c->message, &peek) == AT_AUTH_OK, "[%s] message parses", c->name);
        uint8_t pub[AT_AUTH_PUBKEY_LEN];
        at_perm_t perm;
        at_auth_result_t res = lookup_user(peek.kid, pub, &perm)
                                   ? at_auth_cloud_cmd_verify(c->message, pub, AV_DEVICE_ID, AV_NOW, true, &cmd)
                                   : AT_AUTH_UNKNOWN_SIGNER;
        bool want_ok = strcmp(c->expect, "valid") == 0;
        CHECK(want_ok ? res == AT_AUTH_OK : strcmp(at_auth_result_name(res), c->expect) == 0,
              "[%s] got %s, want %s", c->name, at_auth_result_name(res), c->expect);
        if (res == AT_AUTH_OK) {
            CHECK(strcmp(cmd.cmd_json, "{\"cmd\":\"start_preheat\",\"minutes\":20}") == 0, "[%s] cmd body: %s", c->name, cmd.cmd_json);
            CHECK(at_auth_cmd_perm("start_preheat") <= perm, "[%s] signer may start", c->name);
            // Same message, box without network time: refused, never guessed.
            CHECK(at_auth_cloud_cmd_verify(c->message, pub, AV_DEVICE_ID, AV_NOW, false, &cmd) == AT_AUTH_NO_CLOCK,
                  "[%s] no clock refuses", c->name);
            // Replayed after it lapsed.
            CHECK(at_auth_cloud_cmd_verify(c->message, pub, AV_DEVICE_ID, AV_NOW + 31, true, &cmd) == AT_AUTH_EXPIRED,
                  "[%s] late copy expires", c->name);
        }
    }
}

static void test_permissions(void)
{
    CHECK(at_auth_route_perm("GET", "/api/auth/nonce") == AT_PERM_NONE, "nonce is public");
    CHECK(at_auth_route_perm("GET", "/api/status") == AT_PERM_VIEW, "status needs view");
    CHECK(at_auth_route_perm("GET", "/api/status?x=1") == AT_PERM_VIEW, "query ignored for routing");
    CHECK(at_auth_route_perm("PUT", "/api/timers") == AT_PERM_CONTROL, "timers need control");
    CHECK(at_auth_route_perm("POST", "/api/wifi") == AT_PERM_MANAGER, "wifi needs manager");
    CHECK(at_auth_route_perm("DELETE", "/api/auth/users/87etahFmchg") == AT_PERM_ADMIN, "removing users needs admin");
    CHECK(at_auth_route_perm("GET", "/api/statusx") == AT_PERM_ADMIN, "near-miss route fails closed");
    CHECK(at_auth_route_perm("GET", "/api/secret") == AT_PERM_ADMIN, "unknown route fails closed");
    CHECK(at_auth_cmd_perm("stop") == AT_PERM_CONTROL, "stop needs control");
    CHECK(at_auth_cmd_perm("auto_range") == AT_PERM_CONTROL, "control may set the thermostat range");
    CHECK(at_auth_cmd_perm("preheat_duration") == AT_PERM_CONTROL, "control may change a running preheat's length");
    CHECK(at_auth_cmd_perm("hysteresis") == AT_PERM_MANAGER, "settings need manager");
    CHECK(at_auth_cmd_perm("factory_reset") == AT_PERM_ADMIN, "factory reset needs admin");
    CHECK(at_auth_cmd_perm("something_new") == AT_PERM_ADMIN, "unknown command fails closed");
}

int main(void)
{
    test_base64url();
    test_keys();
    test_requests();
    test_grants();
    test_commands();
    test_permissions();
    printf("%s: %d checks, %d failures (%zu requests, %zu grants, %zu commands)\n", failures ? "FAILED" : "OK", checks,
           failures, N(AV_REQUESTS), N(AV_GRANTS), N(AV_COMMANDS));
    return failures ? 1 : 0;
}
