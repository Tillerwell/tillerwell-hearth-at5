// Automatic firmware updates. With the switch on (at_settings_auto_update, set by an admin from the app), the box asks
// GitHub for the latest release every few hours and installs a newer one by itself, but only while the heater is off
// and no timer is running or due to start within 30 minutes. Works with the phone away and with no cloud account.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts the background task. Call once, after at_cloudlink_start. Cheap while the switch is off.
void at_autoupdate_start(void);

#ifdef __cplusplus
}
#endif
