/*
 * HTTP server: single-page UI (status, controls, Wi-Fi / MQTT setup, bus log,
 * firmware upload) and its JSON API. Also serves the captive-portal setup
 * page while the setup access point is up.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent. */
esp_err_t at_web_start(void);

#ifdef __cplusplus
}
#endif
