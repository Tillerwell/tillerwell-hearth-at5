/*
 * Network bring-up for the plain Wi-Fi build, plus connectivity tracking for
 * all builds.
 *
 * Own-Wi-Fi mode (plain Wi-Fi image, or the unified image in Wi-Fi mode):
 *   - no saved network: start a setup access point ("Autoterm-xxxx") with a
 *     captive portal (every DNS name resolves to the device, the web UI's
 *     setup page lists networks and takes the password);
 *   - saved network: join it, retry with back-off, and if it cannot be
 *     reached for two minutes bring the setup AP up as well (AP+STA) so the
 *     credentials can be corrected; the AP goes away again once connected.
 * Matter modes: Matter commissioning owns Wi-Fi; this module only
 * watches the IP events so the web UI and MQTT can start.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*at_net_ip_cb_t)(bool connected);

/* Initialise netif/event loop (idempotent). Call before anything network. */
esp_err_t at_net_init(void);

/* own_wifi: manage Wi-Fi here (STA / setup AP as described above). Otherwise
 * Matter owns the network and this only watches IP events. cb fires on IP
 * up/down. */
esp_err_t at_net_start(at_net_ip_cb_t cb, bool own_wifi);

bool at_net_connected(void);
/* Own-Wi-Fi mode has a network saved (connected or not). False in Matter modes. */
bool at_net_has_saved_network(void);
bool at_net_setup_ap_active(void);
/* "192.168.1.20" or "" */
void at_net_ip_str(char *out, size_t len);
void at_net_ap_ssid(char *out, size_t len);
int at_net_rssi(void);
/* Station disconnects since boot and the reason code of the last one (WIFI_REASON_*), for setup feedback. */
uint32_t at_net_disconnect_count(void);
uint8_t at_net_last_disconnect_reason(void);

/* Announce (true) or withdraw (false) the mDNS service Home Assistant finds the box by. Safe to call before the network
 * is up: the saved setting is applied as soon as mDNS starts. */
void at_net_ha_advertise(bool on);

/* Blocking scan; fills a JSON array string [{"ssid":..,"rssi":..,"auth":..},..]. */
esp_err_t at_net_scan_json(char *out, size_t len);

/* Save new credentials and (re)connect. Plain Wi-Fi build only. */
esp_err_t at_net_set_credentials(const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
