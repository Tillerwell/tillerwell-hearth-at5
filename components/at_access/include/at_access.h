// Who may use the box (docs/API_SPEC.md §4-§7): the user table in NVS, one-time
// nonces, redeemed invites and the button-confirmed admin claim. Pure logic; the
// platform (NVS, random numbers, clocks) comes in through at_access_io_t so
// host_tests/test_access.c can run it unchanged.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "at_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AT_USERS_MAX 16
#define AT_NONCES_MAX 16
#define AT_NONCE_TTL_S 60
#define AT_GIDS_MAX 32
#define AT_CLAIM_WINDOW_S 60
#define AT_NONCE_LEN 22   // base64url of 16 bytes
#define AT_LABEL_MAX 32

typedef struct {
    char kid[AT_AUTH_KID_LEN + 1];
    uint8_t pub[AT_AUTH_PUBKEY_LEN];
    uint8_t perm;                  // at_perm_t
    char label[AT_LABEL_MAX + 1];
    int64_t exp;                   // 0 = none
    char by[AT_AUTH_KID_LEN + 1];
} at_user_t;

typedef struct {
    bool (*load)(const char *key, void *out, size_t len);
    int (*save)(const char *key, const void *in, size_t len);   // 0 = ok
    void (*random)(void *buf, size_t len);
    uint32_t (*uptime_s)(void);
    bool (*wall_time)(int64_t *now);   // false while the clock is not trustworthy
} at_access_io_t;

// device_id is "hearth-" plus the factory MAC as 12 lowercase hex digits.
void at_access_init(const at_access_io_t *io, const char *device_id);
const char *at_access_device_id(void);

// --- users -------------------------------------------------------------
int at_access_user_count(void);
bool at_access_user_at(int i, at_user_t *out);
bool at_access_user_find(const char *kid, at_user_t *out);   // an expired user is not found
bool at_access_has_admin(void);
bool at_access_user_add(const at_user_t *u);                  // replaces the same kid; false if full
// 0 = removed, 1 = not found, 2 = refused (the last admin)
int at_access_user_remove(const char *kid);

// --- nonces (§5.1) -----------------------------------------------------
void at_access_nonce_new(char out[AT_NONCE_LEN + 1]);
bool at_access_nonce_spend(const char *nonce);   // true once, within the ttl

// --- signed requests (§5.3 steps 1, 2 and 4) --------------------------------
// Parses the header and spends its nonce. NULL = fine, else the spec's error code
// ("auth_required", "nonce_invalid").
const char *at_access_open(const char *hdr_value, at_auth_header_t *h);
// Rebuilds the string to sign from the request as received. NULL = fine, else "bad_signature".
const char *at_access_verify(const at_auth_header_t *h, const uint8_t pub[AT_AUTH_PUBKEY_LEN], const char *method,
                             const char *path, const char *body_sha256_hex);

// --- invitations (§7.2, §7.3) ---------------------------------------------
bool at_access_gid_used(const char *gid);
void at_access_gid_burn(const char *gid);
// Checks and redeems a grant token presented by the holder of guest_pub (the key that signed this request).
// NULL = user added, else the error code.
const char *at_access_redeem(const char *token, const uint8_t guest_pub[AT_AUTH_PUBKEY_LEN]);

// --- claiming the first admin (§7.1) -----------------------------------
typedef enum { AT_CLAIM_IDLE, AT_CLAIM_WAITING, AT_CLAIM_CLAIMED, AT_CLAIM_TIMEOUT } at_claim_state_t;
// 202 = started (button window open), 409 = already has an admin, 423 = another claim is waiting
int at_access_claim_start(const uint8_t pub[AT_AUTH_PUBKEY_LEN], const char *label);
at_claim_state_t at_access_claim_state(void);
bool at_access_claim_open(void);          // for the LED
void at_access_button_pressed(void);      // a short press: confirms an open claim

// --- the old unauthenticated API (§3) -----------------------------------
bool at_access_legacy_enabled(void);
int at_access_legacy_set(bool enabled);   // 0 = ok

// ESP-IDF only: wires the logic to NVS, the RNG and the clock. Call once after at_settings_init.
void at_access_start(void);

#ifdef __cplusplus
}
#endif
