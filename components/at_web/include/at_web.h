/*
 * HTTP server: single-page UI (status, controls, Wi-Fi / MQTT setup, bus log,
 * firmware upload) and its JSON API. Also serves the captive-portal setup
 * page while the setup access point is up.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent. */
esp_err_t at_web_start(void);

/* Runs a POST /api/cmd body ({"cmd":"start_preheat","minutes":20}) through the same safety layer the web UI uses. The
 * caller must already have checked who may ask. False: nothing was done (unknown command, bad value, or the box
 * refused, e.g. bypass is on). */
/* reason (may be NULL) gets a short code when the box refuses for a known reason ("heater_care_min_run"), else "". */
bool at_web_exec_cmd(const char *json, char *reason, size_t n);

#ifdef __cplusplus
}
#endif
