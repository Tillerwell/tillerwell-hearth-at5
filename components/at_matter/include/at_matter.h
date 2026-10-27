/*
 * Matter front end (Matter builds only; an empty component in the plain
 * Wi-Fi build).
 *
 * Endpoints:
 *   1  Thermostat (heating only): local temperature = the temperature the
 *      thermostat acts on, setpoint = Auto target, mode Heat = Auto on,
 *      Off = Auto off and send Stop.
 *   2  Temperature sensor: heater coolant temperature.
 *   3  On/Off plug-in unit "Preheat": On starts a preheat for the saved
 *      duration, Off stops the heater. Reports On while the heater runs.
 *   4  Contact sensor "Fault": closed = healthy, open = fault active.
 *
 * Transport (Wi-Fi or Thread) is chosen at run time (the mode setting). Commissioning is
 * over Bluetooth LE in both.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t at_matter_start(void);

#ifdef __cplusplus
}
#endif
