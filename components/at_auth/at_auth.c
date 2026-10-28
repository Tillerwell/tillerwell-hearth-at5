#include "at_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

const char *at_auth_result_name(at_auth_result_t r)
{
    switch (r) {
    case AT_AUTH_OK: return "ok";
    case AT_AUTH_MALFORMED: return "malformed";
    case AT_AUTH_BAD_SIGNATURE: return "bad_signature";
    case AT_AUTH_UNKNOWN_SIGNER: return "unknown_signer";
    case AT_AUTH_EXPIRED: return "expired";
    case AT_AUTH_WRONG_DEVICE: return "wrong_device";
    case AT_AUTH_NO_CLOCK: return "no_clock";
    }
    return "malformed";
}

static const char *const PERM_NAMES[] = { "none", "view", "control", "manager", "admin" };

at_perm_t at_perm_from_name(const char *name)
{
    for (int p = AT_PERM_VIEW; p <= AT_PERM_ADMIN; p++)
        if (name && strcmp(name, PERM_NAMES[p]) == 0) return (at_perm_t)p;
    return AT_PERM_NONE;
}

const char *at_perm_name(at_perm_t p)
{
    return (p >= AT_PERM_NONE && p <= AT_PERM_ADMIN) ? PERM_NAMES[p] : "none";
}

// ---------------------------------------------------------------------------
// base64url (RFC 4648 §5), no padding

static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t at_b64url_encode(const uint8_t *in, size_t len, char *out, size_t out_size)
{
    size_t need = (len * 4 + 2) / 3;
    if (out_size < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = B64URL[(v >> 18) & 63];
        out[o++] = B64URL[(v >> 12) & 63];
        if (i + 1 < len) out[o++] = B64URL[(v >> 6) & 63];
        if (i + 2 < len) out[o++] = B64URL[v & 63];
    }
    out[o] = '\0';
    return o;
}

static int b64url_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

int at_b64url_decode(const char *in, size_t in_len, uint8_t *out, size_t out_size)
{
    if (in_len % 4 == 1) return -1;
    size_t need = in_len * 3 / 4;
    if (need > out_size) return -1;
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        int v = b64url_value(in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    // Leftover bits must be zero, so each encoding has exactly one decoding.
    if (bits > 0 && (acc & ((1u << bits) - 1))) return -1;
    return (int)o;
}

// ---------------------------------------------------------------------------
// Keys and signatures

void at_auth_key_id(const uint8_t pub[AT_AUTH_PUBKEY_LEN], char out[AT_AUTH_KID_LEN + 1])
{
    uint8_t hash[32];
    mbedtls_sha256(pub, AT_AUTH_PUBKEY_LEN, hash, 0);
    at_b64url_encode(hash, 8, out, AT_AUTH_KID_LEN + 1);
}

static bool verify_hash(const uint8_t pub[AT_AUTH_PUBKEY_LEN], const uint8_t hash[32], const uint8_t sig[AT_AUTH_SIG_LEN])
{
    if (pub[0] != 0x04) return false;
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_ecp_point_read_binary(&grp, &q, pub, AT_AUTH_PUBKEY_LEN) == 0 &&
              mbedtls_ecp_check_pubkey(&grp, &q) == 0 &&
              mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
              mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0 &&
              mbedtls_ecdsa_verify(&grp, hash, 32, &q, &r, &s) == 0;
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool at_auth_verify(const uint8_t pub[AT_AUTH_PUBKEY_LEN], const uint8_t *msg, size_t len, const uint8_t sig[AT_AUTH_SIG_LEN])
{
    uint8_t hash[32];
    mbedtls_sha256(msg, len, hash, 0);
    return verify_hash(pub, hash, sig);
}

// ---------------------------------------------------------------------------
// Local requests

void at_auth_sha256_hex(const uint8_t *data, size_t len, char out[65])
{
    uint8_t hash[32];
    mbedtls_sha256(data ? data : (const uint8_t *)"", len, hash, 0);
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", hash[i]);
}

size_t at_auth_string_to_sign_hash(char *out, size_t cap, const char *device_id, const char *method,
                                   const char *path, const char *nonce, const char *body_sha256_hex)
{
    int n = snprintf(out, cap, "HEARTH-REQ-1\n%s\n%s\n%s\n%s\n%s", device_id, method, path, nonce, body_sha256_hex);
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

size_t at_auth_string_to_sign(char *out, size_t cap, const char *device_id, const char *method,
                              const char *path, const char *nonce, const uint8_t *body, size_t body_len)
{
    char hex[65];
    at_auth_sha256_hex(body, body_len, hex);
    return at_auth_string_to_sign_hash(out, cap, device_id, method, path, nonce, hex);
}

// Copies the value of "<key>=" from a space-separated field list.
static bool header_field(const char *value, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    for (const char *p = value; *p;) {
        while (*p == ' ') p++;
        const char *end = strchr(p, ' ');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = len - klen - 1;
            if (vlen == 0 || vlen >= cap) return false;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = '\0';
            return true;
        }
        p += len;
    }
    return false;
}

bool at_auth_parse_header(const char *value, at_auth_header_t *out)
{
    char sig_b64[96];
    memset(out, 0, sizeof(*out));
    if (!value || strncmp(value, "v1 ", 3) != 0) return false;
    if (!header_field(value + 3, "kid", out->kid, sizeof(out->kid)) || strlen(out->kid) != AT_AUTH_KID_LEN) return false;
    if (!header_field(value + 3, "nonce", out->nonce, sizeof(out->nonce))) return false;
    if (!header_field(value + 3, "sig", sig_b64, sizeof(sig_b64))) return false;
    return at_b64url_decode(sig_b64, strlen(sig_b64), out->sig, sizeof(out->sig)) == AT_AUTH_SIG_LEN;
}

// ---------------------------------------------------------------------------
// Envelopes

at_auth_result_t at_auth_open_envelope(const char *prefix, const char *p_b64, size_t p_len,
                                       const char *s_b64, size_t s_len,
                                       const uint8_t pub[AT_AUTH_PUBKEY_LEN],
                                       char *payload, size_t payload_cap)
{
    uint8_t sig[AT_AUTH_SIG_LEN];
    if (payload_cap < 2) return AT_AUTH_MALFORMED;
    int n = at_b64url_decode(p_b64, p_len, (uint8_t *)payload, payload_cap - 1);
    if (n <= 0) return AT_AUTH_MALFORMED;
    payload[n] = '\0';
    if (at_b64url_decode(s_b64, s_len, sig, sizeof(sig)) != AT_AUTH_SIG_LEN) return AT_AUTH_MALFORMED;

    // The signature covers "<prefix>\n" || payload; hash it in pieces, no copy.
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t *)prefix, strlen(prefix));
    mbedtls_sha256_update(&ctx, (const uint8_t *)"\n", 1);
    mbedtls_sha256_update(&ctx, (const uint8_t *)payload, (size_t)n);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    return verify_hash(pub, hash, sig) ? AT_AUTH_OK : AT_AUTH_BAD_SIGNATURE;
}

static bool json_str(const cJSON *obj, const char *name, char *out, size_t cap)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t len = strlen(item->valuestring);
    if (len == 0 || len >= cap) return false;
    memcpy(out, item->valuestring, len + 1);
    return true;
}

static bool json_int(const cJSON *obj, const char *name, int64_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(item)) return false;
    *out = (int64_t)item->valuedouble;
    return (double)*out == item->valuedouble;  // whole numbers only
}

static const char *find_dot(const char *token)
{
    const char *dot = strchr(token, '.');
    return (dot && dot != token && dot[1] && !strchr(dot + 1, '.')) ? dot : NULL;
}

static at_auth_result_t grant_fields(const char *payload, at_grant_t *g)
{
    at_auth_result_t res = AT_AUTH_MALFORMED;
    char typ[8], perm[12], key_b64[96];
    int64_t v = 0;
    cJSON *root = cJSON_Parse(payload);
    if (!root) return AT_AUTH_MALFORMED;
    memset(g, 0, sizeof(*g));
    if (json_int(root, "v", &v) && v == 1 &&
        json_str(root, "typ", typ, sizeof(typ)) && strcmp(typ, "grant") == 0 &&
        json_str(root, "gid", g->gid, sizeof(g->gid)) &&
        json_str(root, "dev", g->dev, sizeof(g->dev)) &&
        json_str(root, "key", key_b64, sizeof(key_b64)) &&
        at_b64url_decode(key_b64, strlen(key_b64), g->key, sizeof(g->key)) == AT_AUTH_PUBKEY_LEN && g->key[0] == 0x04 &&
        json_str(root, "label", g->label, sizeof(g->label)) &&
        json_str(root, "perm", perm, sizeof(perm)) && (g->perm = at_perm_from_name(perm)) != AT_PERM_NONE &&
        json_int(root, "iat", &g->iat) && json_int(root, "redeem_by", &g->redeem_by) && json_int(root, "exp", &g->exp) &&
        json_str(root, "by", g->by, sizeof(g->by)) && strlen(g->by) == AT_AUTH_KID_LEN) {
        at_auth_key_id(g->key, g->key_id);
        res = AT_AUTH_OK;
    }
    cJSON_Delete(root);
    return res;
}

at_auth_result_t at_auth_grant_peek(const char *token, at_grant_t *g)
{
    char payload[640];
    const char *dot = token ? find_dot(token) : NULL;
    if (!dot) return AT_AUTH_MALFORMED;
    int n = at_b64url_decode(token, (size_t)(dot - token), (uint8_t *)payload, sizeof(payload) - 1);
    if (n <= 0) return AT_AUTH_MALFORMED;
    payload[n] = '\0';
    return grant_fields(payload, g);
}

at_auth_result_t at_auth_grant_verify(const char *token, const uint8_t admin_pub[AT_AUTH_PUBKEY_LEN], at_grant_t *g)
{
    char payload[640];
    const char *dot = token ? find_dot(token) : NULL;
    if (!dot) return AT_AUTH_MALFORMED;
    at_auth_result_t res = at_auth_open_envelope("HEARTH-GRANT-1", token, (size_t)(dot - token), dot + 1, strlen(dot + 1),
                                                 admin_pub, payload, sizeof(payload));
    if (res != AT_AUTH_OK) return res;
    return grant_fields(payload, g);
}

// ---------------------------------------------------------------------------
// Cloud commands

static at_auth_result_t cmd_message(const char *message, char *kid, char *p, size_t p_cap, char *s, size_t s_cap)
{
    cJSON *root = cJSON_Parse(message);
    if (!root) return AT_AUTH_MALFORMED;
    bool ok = json_str(root, "kid", kid, AT_AUTH_KID_LEN + 1) && strlen(kid) == AT_AUTH_KID_LEN &&
              json_str(root, "p", p, p_cap) && json_str(root, "s", s, s_cap);
    cJSON_Delete(root);
    return ok ? AT_AUTH_OK : AT_AUTH_MALFORMED;
}

static at_auth_result_t cmd_fields(const char *payload, at_cloud_cmd_t *out)
{
    at_auth_result_t res = AT_AUTH_MALFORMED;
    int64_t v = 0;
    cJSON *root = cJSON_Parse(payload);
    if (!root) return AT_AUTH_MALFORMED;
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (json_int(root, "v", &v) && v == 1 && json_str(root, "id", out->id, sizeof(out->id)) &&
        json_str(root, "dev", out->dev, sizeof(out->dev)) && json_int(root, "iat", &out->iat) &&
        json_int(root, "exp", &out->exp) && cJSON_IsObject(cmd) &&
        cJSON_PrintPreallocated((cJSON *)cmd, out->cmd_json, sizeof(out->cmd_json), false)) {
        res = AT_AUTH_OK;
    }
    cJSON_Delete(root);
    return res;
}

at_auth_result_t at_auth_cloud_cmd_peek(const char *message, at_cloud_cmd_t *out)
{
    char p[768], s[96];
    memset(out, 0, sizeof(*out));
    return cmd_message(message, out->kid, p, sizeof(p), s, sizeof(s));
}

at_auth_result_t at_auth_cloud_cmd_verify(const char *message, const uint8_t pub[AT_AUTH_PUBKEY_LEN],
                                          const char *device_id, int64_t now, bool clock_ok, at_cloud_cmd_t *out)
{
    char p[768], s[96], payload[600];
    memset(out, 0, sizeof(*out));
    at_auth_result_t res = cmd_message(message, out->kid, p, sizeof(p), s, sizeof(s));
    if (res != AT_AUTH_OK) return res;
    // Nothing in the payload is trusted until the signature has been checked.
    res = at_auth_open_envelope("HEARTH-CMD-1", p, strlen(p), s, strlen(s), pub, payload, sizeof(payload));
    if (res != AT_AUTH_OK) return res;
    if ((res = cmd_fields(payload, out)) != AT_AUTH_OK) return res;
    if (strcmp(out->dev, device_id) != 0) return AT_AUTH_WRONG_DEVICE;
    if (!clock_ok) return AT_AUTH_NO_CLOCK;
    if (out->exp < out->iat || out->exp - out->iat > AT_AUTH_MAX_CMD_LIFETIME_S) return AT_AUTH_EXPIRED;
    if (now > out->exp || now < out->iat - AT_AUTH_CLOCK_SKEW_S) return AT_AUTH_EXPIRED;
    return AT_AUTH_OK;
}

bool at_auth_cloud_cmd_untrusted_id(const char *message, char *out, size_t cap)
{
    char kid[AT_AUTH_KID_LEN + 1], p[768], s[96], payload[600];
    if (cmd_message(message, kid, p, sizeof(p), s, sizeof(s)) != AT_AUTH_OK) return false;
    int n = at_b64url_decode(p, strlen(p), (uint8_t *)payload, sizeof(payload) - 1);
    if (n < 0) return false;
    payload[n] = 0;
    cJSON *root = cJSON_Parse(payload);
    bool ok = root && json_str(root, "id", out, cap);
    cJSON_Delete(root);
    return ok;
}

// ---------------------------------------------------------------------------
// Permissions (spec §6, CLOUD-PLANNING.md Q8)

typedef struct {
    const char *name;
    at_perm_t perm;
} cmd_perm_t;

static const cmd_perm_t CMD_PERMS[] = {
    { "start_preheat", AT_PERM_CONTROL }, { "stop", AT_PERM_CONTROL },
    { "auto", AT_PERM_CONTROL },          { "auto_target", AT_PERM_CONTROL },
    { "set_time", AT_PERM_CONTROL },      { "tank_level", AT_PERM_CONTROL },
    { "tank_filled", AT_PERM_CONTROL },
    { "auto_range", AT_PERM_MANAGER },    { "hysteresis", AT_PERM_MANAGER },
    { "preheat_temp", AT_PERM_MANAGER },  { "preheat_minutes", AT_PERM_MANAGER },
    { "timezone", AT_PERM_MANAGER },      { "display_name", AT_PERM_MANAGER },
    { "fuel_price", AT_PERM_MANAGER },    { "fuel_factor", AT_PERM_MANAGER },
    { "fuel_rates", AT_PERM_MANAGER },    { "tank_capacity", AT_PERM_MANAGER },
    { "factory_reset", AT_PERM_ADMIN },   { "reboot", AT_PERM_ADMIN },
    { "ota", AT_PERM_ADMIN },
};

at_perm_t at_auth_cmd_perm(const char *cmd_name)
{
    for (size_t i = 0; cmd_name && i < sizeof(CMD_PERMS) / sizeof(CMD_PERMS[0]); i++)
        if (strcmp(cmd_name, CMD_PERMS[i].name) == 0) return CMD_PERMS[i].perm;
    return AT_PERM_ADMIN;
}

at_perm_t at_auth_cmd_json_perm(const char *cmd_json)
{
    cJSON *j = cmd_json ? cJSON_Parse(cmd_json) : NULL;
    const cJSON *c = j ? cJSON_GetObjectItemCaseSensitive(j, "cmd") : NULL;
    at_perm_t p = cJSON_IsString(c) ? at_auth_cmd_perm(c->valuestring) : AT_PERM_ADMIN;
    cJSON_Delete(j);
    return p < AT_PERM_CONTROL ? AT_PERM_CONTROL : p;
}

typedef struct {
    const char *method;
    const char *path;  // a trailing '*' matches any rest of the path
    at_perm_t perm;
} route_perm_t;

static const route_perm_t ROUTE_PERMS[] = {
    { "GET", "/api/auth/nonce", AT_PERM_NONE },  { "POST", "/api/auth/claim", AT_PERM_NONE },
    { "GET", "/api/auth/claim", AT_PERM_NONE },  { "POST", "/api/auth/redeem", AT_PERM_NONE },
    { "GET", "/api/hello", AT_PERM_NONE },
    { "GET", "/api/status", AT_PERM_VIEW },      { "GET", "/api/info", AT_PERM_VIEW },
    { "GET", "/api/timers", AT_PERM_VIEW },      { "GET", "/api/fuel", AT_PERM_VIEW },
    { "GET", "/api/auth/me", AT_PERM_VIEW },
    { "PUT", "/api/timers", AT_PERM_CONTROL },   { "POST", "/api/cmd", AT_PERM_CONTROL },
    { "GET", "/api/wifi/scan", AT_PERM_MANAGER }, { "POST", "/api/wifi", AT_PERM_MANAGER },
    { "POST", "/api/mode", AT_PERM_MANAGER },
    { "*", "/api/auth/users*", AT_PERM_ADMIN },  { "*", "/api/cloud*", AT_PERM_ADMIN },
    { "POST", "/api/ota", AT_PERM_ADMIN },       { "POST", "/api/auth/grant-revoke", AT_PERM_ADMIN },
};

at_perm_t at_auth_route_perm(const char *method, const char *path)
{
    if (!method || !path) return AT_PERM_ADMIN;
    size_t plen = strcspn(path, "?");
    for (size_t i = 0; i < sizeof(ROUTE_PERMS) / sizeof(ROUTE_PERMS[0]); i++) {
        const route_perm_t *r = &ROUTE_PERMS[i];
        if (strcmp(r->method, "*") != 0 && strcmp(r->method, method) != 0) continue;
        size_t rlen = strlen(r->path);
        bool prefix = rlen && r->path[rlen - 1] == '*';
        if (prefix ? (plen >= rlen - 1 && strncmp(path, r->path, rlen - 1) == 0)
                   : (plen == rlen && strncmp(path, r->path, rlen) == 0))
            return r->perm;
    }
    return AT_PERM_ADMIN;  // unknown routes fail closed
}
