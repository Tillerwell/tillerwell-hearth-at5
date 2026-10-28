#include "at_access.h"

#include <stdio.h>
#include <string.h>


#define KEY_USERS "acc_users"
#define KEY_GIDS "acc_gids"
#define KEY_LEGACY "acc_legacy"

typedef struct {
    uint8_t n;
    at_user_t u[AT_USERS_MAX];
} users_blob_t;

typedef struct {
    uint8_t next;
    uint8_t n;
    char gid[AT_GIDS_MAX][24];
} gids_blob_t;

static const at_access_io_t *s_io;
static char s_device_id[32];
static users_blob_t s_users;
static gids_blob_t s_gids;
static bool s_legacy = true;

static struct {
    char nonce[AT_NONCE_LEN + 1];
    uint32_t born;
    bool used;
} s_nonces[AT_NONCES_MAX];
static uint8_t s_nonce_next;

static struct {
    at_claim_state_t state;
    uint32_t started;
    at_user_t user;
} s_claim;

static struct {
    char id[40];
} s_cmd_ids[AT_CMD_IDS_MAX];
static uint8_t s_cmd_next;

static void save_users(void) { s_io->save(KEY_USERS, &s_users, sizeof(s_users)); }
static void save_gids(void) { s_io->save(KEY_GIDS, &s_gids, sizeof(s_gids)); }

void at_access_init(const at_access_io_t *io, const char *device_id)
{
    s_io = io;
    snprintf(s_device_id, sizeof(s_device_id), "%s", device_id);
    memset(&s_users, 0, sizeof(s_users));
    memset(&s_gids, 0, sizeof(s_gids));
    memset(s_nonces, 0, sizeof(s_nonces));
    memset(&s_claim, 0, sizeof(s_claim));
    memset(s_cmd_ids, 0, sizeof(s_cmd_ids));
    s_cmd_next = 0;
    s_nonce_next = 0;
    s_legacy = true;
    if (!io->load(KEY_USERS, &s_users, sizeof(s_users)) || s_users.n > AT_USERS_MAX) memset(&s_users, 0, sizeof(s_users));
    if (!io->load(KEY_GIDS, &s_gids, sizeof(s_gids)) || s_gids.n > AT_GIDS_MAX || s_gids.next >= AT_GIDS_MAX)
        memset(&s_gids, 0, sizeof(s_gids));
    uint8_t l = 1;
    if (io->load(KEY_LEGACY, &l, 1)) s_legacy = l != 0;
    // Never leave a box with the old API off and nobody able to sign in: if every admin is gone, it comes back.
    if (!s_legacy && !at_access_has_admin()) s_legacy = true;
}

const char *at_access_device_id(void) { return s_device_id; }

// --- users ----------------------------------------------------------------

int at_access_user_count(void) { return s_users.n; }

bool at_access_user_at(int i, at_user_t *out)
{
    if (i < 0 || i >= s_users.n) return false;
    *out = s_users.u[i];
    return true;
}

static bool expired(const at_user_t *u)
{
    int64_t now;
    return u->exp != 0 && s_io->wall_time && s_io->wall_time(&now) && now > u->exp;
}

bool at_access_user_find(const char *kid, at_user_t *out)
{
    for (int i = 0; kid && i < s_users.n; i++) {
        if (strcmp(s_users.u[i].kid, kid) == 0) {
            if (expired(&s_users.u[i])) return false;
            if (out) *out = s_users.u[i];
            return true;
        }
    }
    return false;
}

bool at_access_has_admin(void)
{
    for (int i = 0; i < s_users.n; i++)
        if (s_users.u[i].perm == AT_PERM_ADMIN && !expired(&s_users.u[i])) return true;
    return false;
}

bool at_access_user_add(const at_user_t *u)
{
    for (int i = 0; i < s_users.n; i++) {
        if (strcmp(s_users.u[i].kid, u->kid) == 0) {
            s_users.u[i] = *u;
            save_users();
            return true;
        }
    }
    if (s_users.n >= AT_USERS_MAX) return false;
    s_users.u[s_users.n++] = *u;
    save_users();
    return true;
}

int at_access_user_remove(const char *kid)
{
    int idx = -1, admins = 0;
    for (int i = 0; i < s_users.n; i++) {
        if (s_users.u[i].perm == AT_PERM_ADMIN) admins++;
        if (kid && strcmp(s_users.u[i].kid, kid) == 0) idx = i;
    }
    if (idx < 0) return 1;
    if (s_users.u[idx].perm == AT_PERM_ADMIN && admins <= 1) return 2;
    memmove(&s_users.u[idx], &s_users.u[idx + 1], (size_t)(s_users.n - idx - 1) * sizeof(at_user_t));
    s_users.n--;
    memset(&s_users.u[s_users.n], 0, sizeof(at_user_t));
    save_users();
    if (!at_access_has_admin() && !s_legacy) at_access_legacy_set(true);
    return 0;
}

// --- nonces -----------------------------------------------------------------

void at_access_nonce_new(char out[AT_NONCE_LEN + 1])
{
    uint8_t r[16];
    s_io->random(r, sizeof(r));
    at_b64url_encode(r, sizeof(r), out, AT_NONCE_LEN + 1);
    // Keep the 16 newest; the oldest slot is overwritten first.
    memcpy(s_nonces[s_nonce_next].nonce, out, AT_NONCE_LEN + 1);
    s_nonces[s_nonce_next].born = s_io->uptime_s();
    s_nonces[s_nonce_next].used = true;
    s_nonce_next = (uint8_t)((s_nonce_next + 1) % AT_NONCES_MAX);
}

bool at_access_nonce_spend(const char *nonce)
{
    if (!nonce) return false;
    for (int i = 0; i < AT_NONCES_MAX; i++) {
        if (s_nonces[i].used && strcmp(s_nonces[i].nonce, nonce) == 0) {
            s_nonces[i].used = false;   // spent, whatever happens next
            return s_io->uptime_s() - s_nonces[i].born <= AT_NONCE_TTL_S;
        }
    }
    return false;
}

// --- signed requests ------------------------------------------------------------

const char *at_access_open(const char *hdr_value, at_auth_header_t *h)
{
    if (!hdr_value || !at_auth_parse_header(hdr_value, h)) return "auth_required";
    if (!at_access_nonce_spend(h->nonce)) return "nonce_invalid";
    return NULL;
}

const char *at_access_verify(const at_auth_header_t *h, const uint8_t pub[AT_AUTH_PUBKEY_LEN], const char *method,
                             const char *path, const char *body_sha256_hex)
{
    char sts[512];
    size_t n = at_auth_string_to_sign_hash(sts, sizeof(sts), s_device_id, method, path, h->nonce, body_sha256_hex);
    if (!n || !at_auth_verify(pub, (const uint8_t *)sts, n, h->sig)) return "bad_signature";
    return NULL;
}

// --- invitations ------------------------------------------------------------------

bool at_access_gid_used(const char *gid)
{
    for (int i = 0; i < s_gids.n; i++)
        if (strcmp(s_gids.gid[i], gid) == 0) return true;
    return false;
}

void at_access_gid_burn(const char *gid)
{
    if (at_access_gid_used(gid)) return;
    snprintf(s_gids.gid[s_gids.next], sizeof(s_gids.gid[0]), "%s", gid);
    s_gids.next = (uint8_t)((s_gids.next + 1) % AT_GIDS_MAX);
    if (s_gids.n < AT_GIDS_MAX) s_gids.n++;
    save_gids();
}

const char *at_access_redeem(const char *token, const uint8_t guest_pub[AT_AUTH_PUBKEY_LEN])
{
    at_grant_t g;
    at_auth_result_t r = at_auth_grant_peek(token, &g);
    if (r != AT_AUTH_OK) return at_auth_result_name(r);
    at_user_t admin;
    if (!at_access_user_find(g.by, &admin) || admin.perm != AT_PERM_ADMIN) return "unknown_signer";
    r = at_auth_grant_verify(token, admin.pub, &g);
    if (r != AT_AUTH_OK) return at_auth_result_name(r);
    if (strcmp(g.dev, s_device_id) != 0) return "wrong_device";
    if (memcmp(g.key, guest_pub, AT_AUTH_PUBKEY_LEN) != 0) return "wrong_key";
    if (at_access_gid_used(g.gid)) return "already_used";
    int64_t now;
    if (s_io->wall_time && s_io->wall_time(&now) && (now > g.redeem_by || (g.exp != 0 && now > g.exp))) return "expired";
    if (g.perm == AT_PERM_NONE) return "malformed";
    at_user_t u;
    memset(&u, 0, sizeof(u));
    snprintf(u.kid, sizeof(u.kid), "%s", g.key_id);
    memcpy(u.pub, g.key, sizeof(u.pub));
    u.perm = (uint8_t)g.perm;
    snprintf(u.label, sizeof(u.label), "%s", g.label);
    u.exp = g.exp;
    snprintf(u.by, sizeof(u.by), "%s", g.by);
    if (!at_access_user_add(&u)) return "table_full";
    at_access_gid_burn(g.gid);
    return NULL;
}

// --- claiming ----------------------------------------------------------------------

static void claim_tick(void)
{
    if (s_claim.state == AT_CLAIM_WAITING && s_io->uptime_s() - s_claim.started > AT_CLAIM_WINDOW_S)
        s_claim.state = AT_CLAIM_TIMEOUT;
}

int at_access_claim_start(const uint8_t pub[AT_AUTH_PUBKEY_LEN], const char *label)
{
    if (at_access_has_admin()) return 409;
    memset(&s_claim.user, 0, sizeof(s_claim.user));
    memcpy(s_claim.user.pub, pub, AT_AUTH_PUBKEY_LEN);
    at_auth_key_id(pub, s_claim.user.kid);
    snprintf(s_claim.user.label, sizeof(s_claim.user.label), "%s", label ? label : "");
    s_claim.user.perm = AT_PERM_ADMIN;
    snprintf(s_claim.user.by, sizeof(s_claim.user.by), "%s", s_claim.user.kid);
    s_claim.started = s_io->uptime_s();
    if (!at_access_user_add(&s_claim.user)) return 500;
    s_claim.state = AT_CLAIM_CLAIMED;
    return 202;
}

at_claim_state_t at_access_claim_state(void)
{
    claim_tick();
    return s_claim.state;
}

bool at_access_claim_open(void) { return at_access_claim_state() == AT_CLAIM_WAITING; }

void at_access_button_pressed(void)
{
    if (!at_access_claim_open() || at_access_has_admin()) return;
    if (at_access_user_add(&s_claim.user)) s_claim.state = AT_CLAIM_CLAIMED;
}

// --- the old API ----------------------------------------------------------------------

bool at_access_legacy_enabled(void) { return s_legacy; }

int at_access_legacy_set(bool enabled)
{
    if (!enabled && !at_access_has_admin()) return 1;   // would lock everyone out
    s_legacy = enabled;
    uint8_t l = enabled ? 1 : 0;
    return s_io->save(KEY_LEGACY, &l, 1);
}

// ---------------------------------------------------------------------------
// Cloud commands (§9.1)

const char *at_access_cloud_cmd(const char *message, int64_t now, bool clock_ok, at_cloud_cmd_t *out, char *id,
                                size_t id_cap)
{
    if (id_cap) id[0] = 0;
    if (!message) return "malformed";
    // Nothing is acted on before the signature has checked out; the id is only for the refusal's acknowledgement.
    if (id_cap) at_auth_cloud_cmd_untrusted_id(message, id, id_cap);
    at_auth_result_t r = at_auth_cloud_cmd_peek(message, out);
    if (r != AT_AUTH_OK) return at_auth_result_name(r);
    at_user_t u;
    if (!at_access_user_find(out->kid, &u)) return "unknown_signer";
    r = at_auth_cloud_cmd_verify(message, u.pub, s_device_id, now, clock_ok, out);
    if (r != AT_AUTH_OK) return at_auth_result_name(r);
    for (int i = 0; i < AT_CMD_IDS_MAX; i++)
        if (s_cmd_ids[i].id[0] && strcmp(s_cmd_ids[i].id, out->id) == 0) return "replay";
    snprintf(s_cmd_ids[s_cmd_next].id, sizeof(s_cmd_ids[0].id), "%s", out->id);
    s_cmd_next = (uint8_t)((s_cmd_next + 1) % AT_CMD_IDS_MAX);
    if (u.perm < at_auth_cmd_json_perm(out->cmd_json)) return "forbidden";
    return NULL;
}
