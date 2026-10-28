// Signed requests, invitation grants and cloud commands (docs/API_SPEC.md).
// Pure C plus mbedTLS and cJSON, host-tested against docs/auth-vectors.json.
// This component only checks signatures and parses; who the users are (the NVS
// user table), nonces and the HTTP/MQTT plumbing live in the callers.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AT_AUTH_PUBKEY_LEN 65   // SEC1 uncompressed P-256 point: 0x04 || X || Y
#define AT_AUTH_SIG_LEN    64   // r || s, 32 bytes each, big-endian
#define AT_AUTH_KID_LEN    11   // base64url of the first 8 bytes of SHA-256(public key)
#define AT_AUTH_MAX_CMD_LIFETIME_S 300
#define AT_AUTH_CLOCK_SKEW_S       60

typedef enum {
    AT_PERM_NONE = 0,
    AT_PERM_VIEW,
    AT_PERM_CONTROL,
    AT_PERM_MANAGER,
    AT_PERM_ADMIN,
} at_perm_t;

typedef enum {
    AT_AUTH_OK = 0,
    AT_AUTH_MALFORMED,
    AT_AUTH_BAD_SIGNATURE,
    AT_AUTH_UNKNOWN_SIGNER,
    AT_AUTH_EXPIRED,
    AT_AUTH_WRONG_DEVICE,
    AT_AUTH_NO_CLOCK,
} at_auth_result_t;

const char *at_auth_result_name(at_auth_result_t r);   // "ok", "bad_signature", ... (the spec's error codes)
at_perm_t at_perm_from_name(const char *name);          // "view" | "control" | "manager" | "admin", else NONE
const char *at_perm_name(at_perm_t p);

// base64url without padding. encode returns characters written (NUL added), 0 if
// out is too small. decode returns bytes written, -1 on bad input or no room.
size_t at_b64url_encode(const uint8_t *in, size_t len, char *out, size_t out_size);
int at_b64url_decode(const char *in, size_t in_len, uint8_t *out, size_t out_size);

void at_auth_key_id(const uint8_t pub[AT_AUTH_PUBKEY_LEN], char out[AT_AUTH_KID_LEN + 1]);

// True for a valid ECDSA-P256-SHA256 signature over msg by pub (and pub is a
// valid point on the curve).
bool at_auth_verify(const uint8_t pub[AT_AUTH_PUBKEY_LEN], const uint8_t *msg, size_t len,
                    const uint8_t sig[AT_AUTH_SIG_LEN]);

// --- Local HTTP requests (spec §5) -----------------------------------------
// Builds "HEARTH-REQ-1\n<device>\n<METHOD>\n<path>\n<nonce>\n<sha256 hex of body>".
// Returns its length, or 0 if out is too small.
size_t at_auth_string_to_sign(char *out, size_t cap, const char *device_id, const char *method,
                              const char *path, const char *nonce, const uint8_t *body, size_t body_len);

// The same, when the body's SHA-256 is already known (a body too big to hold, hashed as it arrives).
size_t at_auth_string_to_sign_hash(char *out, size_t cap, const char *device_id, const char *method,
                                   const char *path, const char *nonce, const char *body_sha256_hex);
void at_auth_sha256_hex(const uint8_t *data, size_t len, char out[65]);   // lowercase hex, NUL-terminated

typedef struct {
    char kid[AT_AUTH_KID_LEN + 1];
    char nonce[32];
    uint8_t sig[AT_AUTH_SIG_LEN];
} at_auth_header_t;

// Parses the Hearth-Auth header value: "v1 kid=<kid> nonce=<nonce> sig=<sig>".
bool at_auth_parse_header(const char *value, at_auth_header_t *out);

// --- Signed envelopes: grants (§7) and cloud commands (§9) -------------------
// Decodes the base64url payload into payload (NUL-terminated) and checks the
// signature over "<prefix>\n" || payload with pub.
at_auth_result_t at_auth_open_envelope(const char *prefix, const char *p_b64, size_t p_len,
                                       const char *s_b64, size_t s_len,
                                       const uint8_t pub[AT_AUTH_PUBKEY_LEN],
                                       char *payload, size_t payload_cap);

typedef struct {
    char gid[24];
    char dev[32];
    uint8_t key[AT_AUTH_PUBKEY_LEN];
    char key_id[AT_AUTH_KID_LEN + 1];
    char label[33];
    at_perm_t perm;
    int64_t iat, redeem_by, exp;   // exp 0 = no expiry
    char by[AT_AUTH_KID_LEN + 1];  // key id of the admin who signed it
} at_grant_t;

// Splits a grant token "<payload b64url>.<sig b64url>" without verifying it, so the
// caller can read g->by and find that admin's key. Then call at_auth_grant_verify.
at_auth_result_t at_auth_grant_peek(const char *token, at_grant_t *g);
at_auth_result_t at_auth_grant_verify(const char *token, const uint8_t admin_pub[AT_AUTH_PUBKEY_LEN],
                                      at_grant_t *g);

typedef struct {
    char kid[AT_AUTH_KID_LEN + 1];
    char id[40];
    char dev[32];
    int64_t iat, exp;
    char cmd_json[256];  // the "cmd" object re-serialised, same shape as a POST /api/cmd body
} at_cloud_cmd_t;

// Reads the MQTT message {"kid","p","s"}; fills out->kid so the caller can look up
// the signer, without verifying anything yet.
at_auth_result_t at_auth_cloud_cmd_peek(const char *message, at_cloud_cmd_t *out);
// Verifies the signature with the signer's key and checks device, clock and
// lifetime (spec §9). now/clock_ok come from SNTP.
at_auth_result_t at_auth_cloud_cmd_verify(const char *message, const uint8_t pub[AT_AUTH_PUBKEY_LEN],
                                          const char *device_id, int64_t now, bool clock_ok,
                                          at_cloud_cmd_t *out);

// The command id inside a cloud command message, read WITHOUT verifying anything, only so a refusal can name the
// command it refuses. Never act on it. False if the message has no readable id.
bool at_auth_cloud_cmd_untrusted_id(const char *message, char *out, size_t cap);

// --- Permissions (spec §6) ---------------------------------------------------
// The level a POST /api/cmd body (or a cloud command's "cmd") needs. Unknown
// commands need admin: fail closed.
at_perm_t at_auth_cmd_perm(const char *cmd_name);
// The level a POST /api/cmd body (or a cloud command's "cmd" object, as JSON text) needs: at_auth_cmd_perm of its
// "cmd" name, never below control. Unparseable or no name: admin (fail closed).
at_perm_t at_auth_cmd_json_perm(const char *cmd_json);
// The level a route needs. NONE means public; for POST /api/cmd use at_auth_cmd_perm.
at_perm_t at_auth_route_perm(const char *method, const char *path);
