/*
 * Bluetooth LE setup service (plain Wi-Fi image only).
 *
 * The phone app finds the box by its advertised service, reads its identity, asks it to scan for Wi-Fi networks
 * and writes the home Wi-Fi name and password; the box joins and reports the result. Wire format: docs/BLE_SETUP.md.
 *
 * Setup is open while the box is not on a Wi-Fi network, for 10 minutes after power-up, and for 3 minutes after the
 * last setup write. Otherwise it stops advertising, so a box that is already working can't be re-pointed by a
 * passer-by: power-cycling it (physical access) opens the window again.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t at_ble_start(void);
/* Bluetooth is up (it holds about 30 KB of heap while it is). */
bool at_ble_running(void);

#ifdef __cplusplus
}
#endif
