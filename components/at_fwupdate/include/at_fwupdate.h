// Firmware update fetched by the box itself (docs/API_SPEC.md §9.7), for a phone that is away from the heater's
// network: an admin signs an `ota` command naming a release image and its SHA-256, and the box downloads it over HTTPS
// and installs it only if the hash matches. The signature on the command is what authorises the image, not the URL.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What a valid `ota` command names. The URL must be a Tillerwell release asset (see at_fwupdate_parse).
#define AT_FWUPDATE_URL_MAX 256
#define AT_FWUPDATE_URL_PREFIX "https://github.com/Tillerwell/tillerwell-hearth-at5/releases/download/"
#define AT_FWUPDATE_MIN_SIZE 100000u

typedef struct {
    char version[8];                 // RoamOS version of the image, e.g. "1.0032"
    char url[AT_FWUPDATE_URL_MAX];
    char sha256[65];                 // lower-case hex of the whole image
    uint32_t size;                   // bytes
} at_fwupdate_req_t;

// Checks an `ota` command body (the signed command, `{"cmd":"ota","version":…,"url":…,"sha256":…,"size":…}`) against
// the running version and the room in the update partition. NULL = acceptable, else the reason code the box acks with:
// "malformed", "bad_url", "older" (no remote rollback), "too_big". Pure, host-tested.
const char *at_fwupdate_parse(const char *cmd_json, const char *running, uint32_t max_size, at_fwupdate_req_t *out);

// Validates and remembers the request; nothing is downloaded yet. NULL = accepted, else a reason ("busy" as well).
const char *at_fwupdate_request(const char *cmd_json);

// Starts the download for an accepted request in its own task. The caller should have freed memory first (the cloud
// link stops its TLS session): the download needs a session of its own. False if there is nothing to start.
bool at_fwupdate_start(void);

// True from at_fwupdate_start until the download has failed (a success restarts the box).
bool at_fwupdate_busy(void);

// Reason code of the last failed update since boot ("download_failed", "size_mismatch", "bad_hash", "bad_image",
// "wrong_version"), or "" if none has failed.
const char *at_fwupdate_last_error(void);

#ifdef __cplusplus
}
#endif
