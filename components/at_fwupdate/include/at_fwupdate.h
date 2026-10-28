// Firmware update fetched by the box itself (docs/API_SPEC.md §9.7), for a phone that is away from the heater's
// network: an admin signs an `ota` command naming a release image and its SHA-256, and the box downloads it over HTTPS
// and installs it only if the hash matches. The signature on the command is what authorises the image, not the URL.
#pragma once

#include <stdbool.h>
#include <stddef.h>
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

// ---- automatic updates (at_autoupdate) ----------------------------------------------------------------------------
// With the admin's switch on, the box looks for a newer release itself and installs it, but only while the heater is
// off and no timer is running or due soon. These two are the pure parts, host-tested.

// A timer starting within this many seconds counts as "about to run".
#define AT_AUTOUPDATE_QUIET_S (30 * 60)

// True when it is safe to start an install now: the clock is set (so a timer's start time means something), the heater
// is off, no timer is running, and the next timer start is more than AT_AUTOUPDATE_QUIET_S away (or there is none).
bool at_autoupdate_idle(bool clock_ok, bool heater_off, bool timer_active, bool have_next, int64_t next_start_epoch,
                        int64_t now_epoch);

// Reads GitHub's "latest release" JSON. 1 = a newer image than `running` exists and `cmd` now holds the `ota` command
// body for it (the same JSON a signed cloud command carries, so it goes through at_fwupdate_request); 0 = up to date;
// -1 = the JSON is unusable or lists no image with a checksum.
int at_autoupdate_pick(const char *release_json, const char *running, char *cmd, size_t cmd_len);

#ifdef __cplusplus
}
#endif
