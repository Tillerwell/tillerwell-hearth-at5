// Host test for components/at_access: the user table, nonces, claim, redeem and
// the signed-request check, driven by docs/auth-vectors.json (via auth_vectors.h).
//   make -C host_tests access
#include <stdio.h>
#include <string.h>

#include "at_access.h"
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

// --- fake platform -----------------------------------------------------------
static struct { char key[16]; uint8_t data[4096]; size_t len; bool used; } store[8];
static uint32_t fake_uptime;
static bool fake_clock_ok;
static int64_t fake_now;
static uint8_t next_random[16];
static bool have_next_random;
static uint8_t counter;

static bool io_load(const char *key, void *out, size_t len)
{
    for (size_t i = 0; i < N(store); i++)
        if (store[i].used && !strcmp(store[i].key, key) && store[i].len == len) { memcpy(out, store[i].data, len); return true; }
    return false;
}
static int io_save(const char *key, const void *in, size_t len)
{
    size_t slot = N(store);
    for (size_t i = 0; i < N(store); i++) {
        if (store[i].used && !strcmp(store[i].key, key)) { slot = i; break; }
        if (!store[i].used && slot == N(store)) slot = i;
    }
    if (slot == N(store) || len > sizeof(store[0].data)) return -1;
    snprintf(store[slot].key, sizeof(store[slot].key), "%s", key);
    memcpy(store[slot].data, in, len);
    store[slot].len = len;
    store[slot].used = true;
    return 0;
}
static void io_random(void *buf, size_t len)
{
    if (have_next_random) { memcpy(buf, next_random, len); have_next_random = false; return; }
    counter++;
    for (size_t i = 0; i < len; i++) ((uint8_t *)buf)[i] = (uint8_t)(counter * 37 + i * 11);
}
static uint32_t io_uptime(void) { return fake_uptime; }
static bool io_wall(int64_t *now) { if (!fake_clock_ok) return false; *now = fake_now; return true; }

static const at_access_io_t IO = { io_load, io_save, io_random, io_uptime, io_wall };

static void boot(void)
{
    fake_uptime = 1000;
    at_access_init(&IO, AV_DEVICE_ID);
}

static const av_key_t *key_named(const char *name)
{
    for (size_t i = 0; i < N(AV_KEYS); i++) if (!strcmp(AV_KEYS[i].name, name)) return &AV_KEYS[i];
    return NULL;
}
static void pub_of(const char *name, uint8_t pub[AT_AUTH_PUBKEY_LEN])
{
    const av_key_t *k = key_named(name);
    CHECK(at_b64url_decode(k->pub_b64url, strlen(k->pub_b64url), pub, AT_AUTH_PUBKEY_LEN) == AT_AUTH_PUBKEY_LEN, "pub %s", name);
}

static void claim_admin(void)
{
    uint8_t pub[AT_AUTH_PUBKEY_LEN];
    pub_of("admin", pub);
    CHECK(at_access_claim_start(pub, "Chris's iPhone") == 202, "claim starts");
}

static void test_claim(void)
{
    memset(store, 0, sizeof(store));
    boot();
    uint8_t admin[AT_AUTH_PUBKEY_LEN], outsider[AT_AUTH_PUBKEY_LEN];
    pub_of("admin", admin);
    pub_of("outsider", outsider);
    CHECK(!at_access_has_admin(), "fresh box has no admin");
    CHECK(at_access_claim_state() == AT_CLAIM_IDLE, "idle");

    CHECK(at_access_claim_start(admin, "me") == 202, "claim accepted");
    CHECK(at_access_has_admin() && at_access_claim_state() == AT_CLAIM_CLAIMED, "the claim makes the admin at once");
    at_access_button_pressed();
    CHECK(at_access_claim_start(outsider, "x") == 409, "no claim once there is an admin");

    boot();   // survives a restart
    at_user_t u;
    CHECK(at_access_user_find(key_named("admin")->key_id, &u) && u.perm == AT_PERM_ADMIN, "admin persisted");
    CHECK(at_access_user_remove(u.kid) == 2, "the last admin cannot be removed");
}

static void test_nonces(void)
{
    memset(store, 0, sizeof(store));
    boot();
    char a[AT_NONCE_LEN + 1], b[AT_NONCE_LEN + 1];
    at_access_nonce_new(a);
    at_access_nonce_new(b);
    CHECK(strlen(a) == AT_NONCE_LEN && strcmp(a, b) != 0, "nonces are 22 characters and differ");
    CHECK(at_access_nonce_spend(a), "first use works");
    CHECK(!at_access_nonce_spend(a), "second use fails");
    fake_uptime += AT_NONCE_TTL_S + 1;
    CHECK(!at_access_nonce_spend(b), "expired nonce fails");
    CHECK(!at_access_nonce_spend(b), "and stays spent");
    char first[AT_NONCE_LEN + 1], n[AT_NONCE_LEN + 1];
    at_access_nonce_new(first);
    for (int i = 0; i < AT_NONCES_MAX; i++) at_access_nonce_new(n);
    CHECK(!at_access_nonce_spend(first), "the oldest of more than 16 is dropped");
    CHECK(at_access_nonce_spend(n), "the newest is kept");
}

static void test_requests(void)
{
    memset(store, 0, sizeof(store));
    boot();
    claim_admin();
    uint8_t guest[AT_AUTH_PUBKEY_LEN];
    pub_of("guest", guest);
    at_user_t gu = { 0 };
    at_auth_key_id(guest, gu.kid);
    memcpy(gu.pub, guest, sizeof(guest));
    gu.perm = AT_PERM_CONTROL;
    CHECK(at_access_user_add(&gu), "guest added");

    for (size_t i = 0; i < N(AV_REQUESTS); i++) {
        const av_request_t *r = &AV_REQUESTS[i];
        at_auth_header_t h0;
        CHECK(at_auth_parse_header(r->header, &h0), "[%s] header parses", r->name);
        // Make the box issue exactly the nonce the vector was signed with.
        CHECK(at_b64url_decode(h0.nonce, strlen(h0.nonce), next_random, 16) == 16, "[%s] nonce is 16 bytes", r->name);
        have_next_random = true;
        char issued[AT_NONCE_LEN + 1];
        at_access_nonce_new(issued);
        CHECK(!strcmp(issued, h0.nonce), "[%s] nonce reproduced", r->name);

        at_auth_header_t h;
        const char *err = at_access_open(r->header, &h);
        CHECK(err == NULL, "[%s] open: %s", r->name, err ? err : "");
        at_user_t u;
        CHECK(at_access_user_find(h.kid, &u), "[%s] signer in table", r->name);
        char hex[65];
        at_auth_sha256_hex((const uint8_t *)r->body, strlen(r->body), hex);
        err = at_access_verify(&h, u.pub, r->method, r->path, hex);
        bool valid = !strcmp(r->expect, "valid");
        CHECK(valid ? err == NULL : (err && !strcmp(err, r->expect)), "[%s] verify gave %s", r->name, err ? err : "ok");
        CHECK(at_access_open(r->header, &h) != NULL && !strcmp(at_access_open(r->header, &h), "nonce_invalid"), "[%s] replay refused", r->name);
    }
    at_auth_header_t h;
    CHECK(!strcmp(at_access_open(NULL, &h), "auth_required"), "no header");
    CHECK(!strcmp(at_access_open("v1 kid=short nonce=x sig=AA", &h), "auth_required"), "malformed header");
    CHECK(!strcmp(at_access_open("v1 kid=87etahFmchg nonce=neverissued sig=AAAA", &h), "nonce_invalid") ||
          !strcmp(at_access_open("v1 kid=87etahFmchg nonce=neverissued sig=AAAA", &h), "auth_required"), "unknown nonce");

    // a user whose grant has lapsed is not found, with a trustworthy clock
    at_user_t lapsing = gu;
    lapsing.exp = AV_NOW + 10;
    at_access_user_add(&lapsing);
    fake_clock_ok = true;
    fake_now = AV_NOW;
    CHECK(at_access_user_find(gu.kid, NULL), "before expiry");
    fake_now = AV_NOW + 11;
    CHECK(!at_access_user_find(gu.kid, NULL), "after expiry");
    fake_clock_ok = false;
    CHECK(at_access_user_find(gu.kid, NULL), "no clock: expiry is not enforced (best effort)");
}

static void test_redeem(void)
{
    uint8_t guest[AT_AUTH_PUBKEY_LEN], outsider[AT_AUTH_PUBKEY_LEN];
    pub_of("guest", guest);
    pub_of("outsider", outsider);
    for (size_t i = 0; i < N(AV_GRANTS); i++) {
        const av_grant_t *g = &AV_GRANTS[i];
        memset(store, 0, sizeof(store));
        boot();
        claim_admin();
        const char *err = at_access_redeem(g->token, guest);
        bool valid = !strcmp(g->expect, "valid");
        CHECK(valid ? err == NULL : (err && !strcmp(err, g->expect)), "[%s] redeem gave %s", g->name, err ? err : "ok");
        if (!valid) { CHECK(at_access_user_count() == 1, "[%s] nobody added", g->name); continue; }
        at_user_t u;
        CHECK(at_access_user_find(key_named("guest")->key_id, &u) && u.perm == AT_PERM_CONTROL, "[%s] guest can control", g->name);
        err = at_access_redeem(g->token, guest);
        CHECK(err && !strcmp(err, "already_used"), "[%s] one use only: %s", g->name, err ? err : "ok");
        boot();
        err = at_access_redeem(g->token, guest);
        CHECK(err && !strcmp(err, "already_used"), "[%s] still used after a restart", g->name);

        // wrong key: someone else presents the token
        memset(store, 0, sizeof(store));
        boot();
        claim_admin();
        err = at_access_redeem(g->token, outsider);
        CHECK(err && !strcmp(err, "wrong_key"), "[%s] a different key cannot redeem: %s", g->name, err ? err : "ok");
        // too late
        fake_clock_ok = true;
        fake_now = AV_NOW + 901;
        err = at_access_redeem(g->token, guest);
        CHECK(err && !strcmp(err, "expired"), "[%s] late redeem: %s", g->name, err ? err : "ok");
        fake_clock_ok = false;
        // burned before use
        at_access_gid_burn("4CiMk3JUelHSYvj7");
        err = at_access_redeem(g->token, guest);
        CHECK(err && !strcmp(err, "already_used"), "[%s] revoked invite: %s", g->name, err ? err : "ok");
    }
    // nobody to sign: a box with no admin refuses
    memset(store, 0, sizeof(store));
    boot();
    const char *err = at_access_redeem(AV_GRANTS[0].token, guest);
    CHECK(err && !strcmp(err, "unknown_signer"), "no admin, no invites: %s", err ? err : "ok");
}

static void test_users_and_legacy(void)
{
    memset(store, 0, sizeof(store));
    boot();
    CHECK(at_access_legacy_enabled(), "old API on by default");
    CHECK(at_access_legacy_set(false) != 0 && at_access_legacy_enabled(), "cannot switch it off with no admin");
    claim_admin();
    CHECK(at_access_legacy_set(false) == 0 && !at_access_legacy_enabled(), "off once there is an admin");
    boot();
    CHECK(!at_access_legacy_enabled(), "stays off after a restart");

    at_user_t u;
    memset(&u, 0, sizeof(u));
    for (int i = 0; i < AT_USERS_MAX; i++) {
        snprintf(u.kid, sizeof(u.kid), "kid%08d", i);
        u.perm = AT_PERM_VIEW;
        if (i < AT_USERS_MAX - 1) CHECK(at_access_user_add(&u), "add %d", i);
        else CHECK(!at_access_user_add(&u), "17th user refused (admin plus 15)");
    }
    CHECK(at_access_user_remove("kid00000000") == 0, "remove a viewer");
    CHECK(at_access_user_remove("nobody") == 1, "unknown kid");
    at_access_user_remove(key_named("admin")->key_id);
    CHECK(at_access_has_admin(), "last admin kept");
}

int main(void)
{
    test_claim();
    test_nonces();
    test_requests();
    test_redeem();
    test_users_and_legacy();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures ? 1 : 0;
}
