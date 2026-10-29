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

static void add_user(const char *name, at_perm_t perm)
{
    at_user_t u = { 0 };
    pub_of(name, u.pub);
    at_auth_key_id(u.pub, u.kid);
    u.perm = (uint8_t)perm;
    CHECK(at_access_user_add(&u), "add %s", name);
}

static const char *cloud(const av_command_t *c, int64_t now, bool clock, char *id)
{
    at_cloud_cmd_t out;
    return at_access_cloud_cmd(c->message, now, clock, &out, id, 40);
}

static void test_cloud_cmds(void)
{
    char id[40];
    const av_command_t *valid = &AV_COMMANDS[0], *tampered = &AV_COMMANDS[1], *expired = &AV_COMMANDS[2], *other = &AV_COMMANDS[3];
    const char *r;

    memset(store, 0, sizeof(store));
    boot();
    claim_admin();
    add_user("guest", AT_PERM_CONTROL);
    r = cloud(valid, AV_NOW, true, id);
    CHECK(!r, "valid command accepted: %s", r ? r : "");
    CHECK(!strcmp(id, "cmd-dab7893a271a"), "ack id is the command's: %s", id);
    r = cloud(valid, AV_NOW, true, id);
    CHECK(r && !strcmp(r, "replay"), "same id twice is a replay: %s", r ? r : "ok");
    CHECK(!strcmp(id, "cmd-dab7893a271a"), "a replay still names its id");
    boot();
    r = cloud(valid, AV_NOW, true, id);
    CHECK(!r, "the replay list is RAM only: gone after a restart (the window and the clock are what then protect)");

    r = cloud(tampered, AV_NOW, true, id);
    CHECK(r && !strcmp(r, "bad_signature"), "changed after signing: %s", r ? r : "ok");
    CHECK(id[0], "a refusal still names the id it read (untrusted)");
    r = cloud(expired, AV_NOW, true, id);
    CHECK(r && !strcmp(r, "expired"), "expired: %s", r ? r : "ok");
    r = cloud(other, AV_NOW, true, id);
    CHECK(r && !strcmp(r, "wrong_device"), "another box: %s", r ? r : "ok");
    r = cloud(&AV_COMMANDS[0], AV_NOW, false, id);
    CHECK(r && !strcmp(r, "no_clock") , "no network time: %s", r ? r : "ok");
    r = at_access_cloud_cmd("{\"kid\":1}", AV_NOW, true, &(at_cloud_cmd_t){ 0 }, id, sizeof(id));
    CHECK(r && !strcmp(r, "malformed") && !id[0], "malformed: %s", r ? r : "ok");
    r = at_access_cloud_cmd(NULL, AV_NOW, true, &(at_cloud_cmd_t){ 0 }, id, sizeof(id));
    CHECK(r && !strcmp(r, "malformed"), "no message at all");

    // The lifetime edges: iat = AV_NOW, exp = AV_NOW + 30, 60 s of slack before iat.
    memset(store, 0, sizeof(store));
    boot();
    claim_admin();
    add_user("guest", AT_PERM_CONTROL);
    CHECK(!cloud(valid, AV_NOW + 30, true, id), "good at exp");
    boot();
    r = cloud(valid, AV_NOW + 31, true, id);
    CHECK(r && !strcmp(r, "expired"), "refused one second after exp");
    CHECK(!cloud(valid, AV_NOW - 60, true, id), "good 60 s before iat");
    boot();
    r = cloud(valid, AV_NOW - 61, true, id);
    CHECK(r && !strcmp(r, "expired"), "refused 61 s before iat");

    // Who may do it.
    memset(store, 0, sizeof(store));
    boot();
    claim_admin();
    r = cloud(valid, AV_NOW, true, id);
    CHECK(r && !strcmp(r, "unknown_signer"), "a key that is not in the table: %s", r ? r : "ok");
    add_user("guest", AT_PERM_VIEW);
    r = cloud(valid, AV_NOW, true, id);
    CHECK(r && !strcmp(r, "forbidden"), "a viewer cannot start the heater: %s", r ? r : "ok");
    CHECK(at_auth_cmd_json_perm("{\"cmd\":\"stop\"}") == AT_PERM_CONTROL, "stop needs control");
    CHECK(at_auth_cmd_json_perm("{\"cmd\":\"fuel_price\",\"value\":1}") == AT_PERM_MANAGER, "fuel_price needs manager");
    CHECK(at_auth_cmd_json_perm("{\"cmd\":\"factory_reset\"}") == AT_PERM_ADMIN, "factory_reset needs admin");
    CHECK(at_auth_cmd_json_perm("{\"cmd\":\"never_heard_of_it\"}") == AT_PERM_ADMIN, "unknown command needs admin");
    CHECK(at_auth_cmd_json_perm("{\"cmd\":\"something_new\"}") == AT_PERM_ADMIN, "unlisted commands fail closed");
    CHECK(at_auth_cmd_json_perm("nonsense") == AT_PERM_ADMIN && at_auth_cmd_json_perm(NULL) == AT_PERM_ADMIN, "unparseable: admin");

    // An expired user is not found, so its commands stop with the grant.
    memset(store, 0, sizeof(store));
    boot();
    claim_admin();
    fake_clock_ok = true;
    fake_now = AV_NOW;
    at_user_t g = { 0 };
    pub_of("guest", g.pub);
    at_auth_key_id(g.pub, g.kid);
    g.perm = AT_PERM_CONTROL;
    g.exp = AV_NOW - 1;
    CHECK(at_access_user_add(&g), "add expiring guest");
    r = cloud(valid, AV_NOW, true, id);
    CHECK(r && !strcmp(r, "unknown_signer"), "expired grant: %s", r ? r : "ok");
    fake_clock_ok = false;
}

int main(void)
{
    test_claim();
    test_nonces();
    test_requests();
    test_redeem();
    test_users_and_legacy();
    test_cloud_cmds();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures ? 1 : 0;
}
