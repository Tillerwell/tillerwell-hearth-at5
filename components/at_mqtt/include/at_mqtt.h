/*
 * MQTT client with Home Assistant MQTT discovery, and a built-in broker (Mosquitto port, off unless an admin turns MQTT on, 1.0046) so no
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

/* Start the built-in broker and the client, if MQTT is turned on (at_settings_mqtt). Call once the network is up. */
esp_err_t at_mqtt_start(void);
/* Stop both and give their memory back (MQTT turned off). */
void at_mqtt_stop(void);
/* Reconnect after the device name or the Home Assistant setting changed. */
void at_mqtt_reload(void);
/* The box's own client is connected to its broker (built-in). */
bool at_mqtt_connected(void);

#ifdef __cplusplus
}
#endif
