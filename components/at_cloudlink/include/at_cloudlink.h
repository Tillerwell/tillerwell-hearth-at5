// The box's link to the Tillerwell cloud broker (docs/API_SPEC.md §8, §9): a second MQTT client, separate from the
// local one in at_mqtt, over mutual TLS with the certificate at_cloud holds. Opt-in: with no certificate installed
// nothing here runs, and the heater and the local web UI never wait on it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Endpoint list, tried in this order (cloud plan §9). The third is the standby's reserved IP and stays empty (and is
// skipped) until that address exists. The name checked against the broker certificate is the host dialled, so the
// certificate must list every entry (the IP as a subject alternative name).
#define AT_CLOUDLINK_HOST_1 "mqtt.tillerwell.com"
#define AT_CLOUDLINK_HOST_2 "mqtt.tillerwell.co.uk"
#define AT_CLOUDLINK_HOST_3 ""
#define AT_CLOUDLINK_PORT 8883

// Runs a POST /api/cmd body through the safety layer; false = the box refused or did nothing (at_web_exec_cmd).
typedef bool (*at_cloudlink_exec_t)(const char *cmd_json);

// Starts the supervisor task. Call once, after at_access_start and at_cloud_start. Cheap while no certificate is
// installed.
esp_err_t at_cloudlink_start(at_cloudlink_exec_t exec);

// Keeps the link down (true) or lets it reconnect (false). Used by the automatic update check, which needs a TLS
// session of its own. at_cloudlink_held() turns true once the link is actually gone.
void at_cloudlink_hold(bool hold);
bool at_cloudlink_held(void);

typedef struct {
    const char *state;      // "off" (no certificate), "waiting" (no network or time), "connecting", "online", "backoff"
    int endpoint;           // 0..3, the one in use or tried last
    uint32_t failures;      // in a row
    uint32_t connects;      // since boot
    uint32_t commands_ok;   // accepted and done
    uint32_t commands_refused;
    uint32_t heap_min_online;   // lowest free heap (bytes) seen while a session was up or being set up; 0 = never
    uint32_t heap_at_connect;   // free heap when the TLS session came up
} at_cloudlink_status_t;
void at_cloudlink_status(at_cloudlink_status_t *out);

#ifdef __cplusplus
}
#endif
