/*
 * MQTT client with Home Assistant MQTT discovery, and an always-on built-in broker (Mosquitto port) so no
 * separate broker has to be installed: the client then connects to it over loopback. Topics (base = hearth/<device name>):
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

/* Start the built-in broker and the client. Call once the network is up. */
esp_err_t at_mqtt_start(void);
/* Reconnect after the device name or the Home Assistant setting changed. */
void at_mqtt_reload(void);
/* The box's own client is connected to its broker (built-in). */
bool at_mqtt_connected(void);

#ifdef __cplusplus
}
#endif
