/*
 * MQTT client with Home Assistant MQTT discovery. Topics (base = hearth/<device name>):
 *   <base>/state               JSON state document (retained), every 2 s
 *   <base>/availability        online / offline (last will)
 *   <base>/cmd/<name>[/set]    commands, see at_mqtt.c
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start (or stop) according to the saved settings. Call once the network is up. */
esp_err_t at_mqtt_start(void);
/* Re-read settings after they changed. */
void at_mqtt_reload(void);
bool at_mqtt_connected(void);

#ifdef __cplusplus
}
#endif
