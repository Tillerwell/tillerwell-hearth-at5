/*
 * Status LED (the board's WS2812 RGB LED, GPIO8 on the ESP32-C6-Zero).
 *
 * Wi-Fi mode (the default, and what the button always returns to):
 *   slow flashing blue    no network saved yet: setup open, unit unconfigured
 *   fast flashing blue    same, but a phone is connected over Bluetooth and the box is waiting for the Wi-Fi details
 *   solid green (10%)      paired and on the network
 *   fast flashing yellow  network saved but not connected (Wi-Fi dropped); always full brightness
 *
 * Matter mode (chosen from the web page / console only, never the button):
 *   pulsing yellow        not paired yet (back to Wi-Fi mode if nobody pairs in 15 minutes)
 *   solid yellow          paired and online
 *   slow blinking yellow  paired but offline
 *
 * While the button is held (main.c does what releasing does):
 *   under 5 s             no change; releasing does nothing
 *   5 s to 10 s           slow flashing yellow; releasing does nothing, back to the status
 *   10 s to 15 s          fast flashing red; releasing resets to factory settings, back to Wi-Fi mode
 *   15 s or more          cancelled: back to the normal status, and releasing does nothing however long it is held
 *
 * Brightness: CONFIG_AUTOTERM_LED_SETUP_BRIGHTNESS while waiting to be set up (first 15 minutes after power-up)
 * and while the button shows yellow or red; full brightness for the Wi-Fi-dropped yellow;
 * CONFIG_AUTOTERM_LED_BRIGHTNESS otherwise.
 *
 * The pairing/paired state is read from the network and Matter modules, nothing has to call in for it.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Button hold times, shared by the button task and the LED */
#define AT_BUTTON_WARN_MS 5000
#define AT_BUTTON_RESET_MS 10000
#define AT_BUTTON_CANCEL_MS 15000

esp_err_t at_led_start(void);

/* The button is down; held_ms is how long. Pass held_ms = 0 when released. */
void at_led_button_held(unsigned held_ms);

/* A phone is (true) or is no longer (false) connected to the Bluetooth setup service. */
void at_led_setup_link(bool connected);

#ifdef __cplusplus
}
#endif
