/*
 * Application layer shared by every front end (web UI, MQTT, Matter, console):
 * persistent settings, the UART bridge task and the thermostat task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "at_core.h"
#include "at_ctl.h"
#include "at_fuel.h"
#include "at_timers.h"
#include "at_tz.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Matter pairing details, shown by the web UI -----------------------------
 * Provided by the Matter component in Matter builds; a weak default in
 * at_settings.c reports "no Matter". qr may be empty. */
typedef struct {
    const char *transport;   /* "wifi" | "thread" | "" */
    char qr[40];
    char manual[16];
    bool commissioned;
    bool online;             /* on its Wi-Fi or Thread network */
} at_matter_info_t;
void at_matter_get_info(at_matter_info_t *out);

/* ---- settings (NVS) --------------------------------------------------------- */

/* How the unified image connects. Other images ignore it. */
#define AT_MODE_WIFI   0   /* default: own Wi-Fi + web UI + MQTT, Matter off */
#define AT_MODE_MATTER 1   /* Matter over Wi-Fi or Thread (the Matter app picks) */

typedef struct {
    uint32_t version;
    char device_name[32];       /* mDNS / hostname / AP name suffix source */
    char wifi_ssid[33];
    char wifi_pass[65];
    /* Home Assistant discovery on/off. The rest of the MQTT fields are legacy and unused (the broker is always built in); kept so saved settings keep their layout. */
    bool mqtt_enabled;
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[33];
    char mqtt_pass[65];
    char mqtt_discovery_prefix[24];
    char web_password[33];      /* empty = no password on the web UI */
    uint16_t preheat_minutes;
    bool auto_enabled;
    float auto_target;
    bool freeze_enabled;
    float freeze_target;
    bool bypass;
    bool debug_mode;            /* periodically send PUBR0 to unlock extended telemetry */
    uint16_t debug_interval_s;
    bool use_external_temp;
    uint8_t mode;               /* AT_MODE_* */
} at_settings_t;

/* Thermostat band (Auto): keep the cabin between low and high; hyst_* are extra degrees past each end
 * before the heater starts/stops. Kept in its own NVS entry so older saved settings stay valid. */
typedef struct {
    float low, high, hyst_min, hyst_max;
} at_band_t;
/* False if nothing was saved yet. */
bool at_settings_band_load(at_band_t *out);
esp_err_t at_settings_band_save(const at_band_t *in);

/* Coolant temperature (C) at which a timed preheat is stopped early. Own NVS entry; 88 until set. */
#define AT_PREHEAT_TEMP_DEFAULT 88.0f
float at_settings_preheat_temp_load(void);
esp_err_t at_settings_preheat_temp_save(float celsius);

/* A small blob in the settings namespace. False if missing or not exactly `len` bytes. */
bool at_settings_blob_load(const char *key, void *out, size_t len);
esp_err_t at_settings_blob_save(const char *key, const void *in, size_t len);

esp_err_t at_settings_init(void);
void at_settings_get(at_settings_t *out);
/* Replace and persist. */
esp_err_t at_settings_set(const at_settings_t *in);
/* Wipe Wi-Fi/MQTT/web settings and, in Matter builds, the Matter fabric, then restart. */
void at_settings_factory_reset(void);
/* As above, but the box comes back up in the given mode (AT_MODE_WIFI or AT_MODE_MATTER). */
void at_settings_factory_reset_to_mode(uint8_t mode);
/* Same, but a moment later so the HTTP reply that asked for it can go out first. */
void at_settings_factory_reset_soon(void);
/* Unified image: save the connection mode (AT_MODE_*) and restart shortly after. */
esp_err_t at_settings_set_mode_and_restart(uint8_t mode);

/* ---- runtime ------------------------------------------------------------- */

uint32_t at_now_ms(void);

/* Start the UART bridge and thermostat tasks. Safe to call once, early: the
 * relay keeps the controller working whatever the network is doing. */
esp_err_t at_app_start(void);

/* Everything a front end needs to display, in one consistent copy. */
typedef struct {
    at_snapshot_t snap;
    bool auto_enabled;
    float auto_target;
    float auto_low, auto_high, hyst_min, hyst_max;
    float preheat_temp;
    bool freeze_enabled;
    float freeze_target;
    char auto_note[AT_CTL_NOTE_LEN];
    char freeze_note[AT_CTL_NOTE_LEN];
    bool have_effective_temp;
    float effective_temp;
    bool effective_is_external;
    bool have_external;
    float external_temp;
    uint16_t preheat_minutes;
    bool debug_mode;
    uint16_t debug_interval_s;
    bool use_external_temp;
} at_view_t;

void at_app_view(at_view_t *out);

/* Commands. Return false if nothing was queued (bypass on, queue full). */
bool at_app_start_preheat(int minutes);       /* minutes <= 0 uses the saved default */
/* Change the total time of a preheat that is running (minutes from when it started). */
bool at_app_set_preheat_duration(int minutes);
bool at_app_start_thermostat(void);
bool at_app_stop(void);
bool at_app_start_pump(void);
bool at_app_send_pubr0(void);

/* ---- fuel estimate (at_app_fuel.c) ---------------------------------------- */

typedef struct {
    at_fuel_cfg_t cfg;
    at_fuel_totals_t totals;
    at_fuel_level_t level;
    float rate_lph;           /* now, calibration applied */
    bool have_today;
    at_fuel_day_t today;
} at_fuel_view_t;

void at_app_fuel_init(void);
/* Called every few seconds with a fresh snapshot: accounts the time since the last call. */
void at_app_fuel_tick(const at_snapshot_t *snap, uint32_t now_ms);
void at_app_fuel_view(at_fuel_view_t *out);
/* Newest first; returns how many were copied. */
int at_app_fuel_log_copy(at_fuel_day_t *out, int max);
/* Each returns false (and changes nothing) if the value is out of range. */
bool at_app_fuel_set_price(float per_litre);
bool at_app_fuel_set_factor(float factor);
bool at_app_fuel_set_rates(float starting, float low, float middle, float high);
/* Today's local date in days since 1970-01-01; false without a clock and time zone. */
bool at_app_local_day(int32_t *day);

void at_app_set_auto(bool enabled);
void at_app_set_auto_target(float target);
/* Keep the cabin between low and high (0..30 C). */
void at_app_set_auto_range(float low, float high);
void at_app_set_hysteresis(float hyst_min, float hyst_max);
/* Stop a run that isn't the thermostat's when the coolant reaches this (20..95 C). */
void at_app_set_preheat_temp(float celsius);
void at_app_set_freeze(bool enabled);
void at_app_set_freeze_target(float target);
void at_app_set_preheat_minutes(int minutes);
void at_app_set_bypass(bool on);
void at_app_set_debug(bool on, int interval_s);
void at_app_set_use_external_temp(bool on);
void at_app_set_external_temp(float celsius);

int at_app_heater_rx_mv(void); /* idle voltage at the heater RX pin in mV, -1 if not read */
void at_app_rx_probe(int high_pct[2], int edges[2]); /* RX pins: % of time high, edges seen, last second */
int at_app_log_copy(at_log_entry_t *out, int max);

/* Thermostat runs owned by a timer (used by the timers; nothing here is saved, so a reboot mid-run drops back
 * to the user's own Auto settings). Begin remembers the band and arms Auto with low..high (widened to the
 * hysteresis minimum); end optionally disarms Auto (sending stop) and puts the band back unless the user
 * changed it meanwhile. */
bool at_app_timer_thermostat_begin(int low_c, int high_c);
void at_app_timer_thermostat_end(bool disarm);
/* Smallest allowed width of an Auto range (hysteresis start + stop). */
float at_app_band_min_gap(void);

/* ---- timers and the clock (at_app_timers.c) ---------------------------------------------------------- */

/* Load the saved timers and time zone, start the clock and the task that fires timers. Called once by
 * at_app_start. */
void at_timers_start(void);

/* The stored list, up to max entries; returns the count. */
int at_app_timers_get(at_timer_t *out, int max);
/* Validate and replace the whole list. ESP_OK on success; ESP_ERR_INVALID_ARG with *problem filled if a rule
 * fails (nothing changes); ESP_FAIL if it could not be saved. */
esp_err_t at_app_timers_set(const at_timer_t *list, int n, at_timer_problem_t *problem);
/* Bumped on every change to the list, so MQTT can republish. */
uint32_t at_app_timers_version(void);

typedef struct {
    bool clock_ok;              /* time and zone are set, so timers may fire */
    bool time_set;
    bool tz_set;
    char timezone[AT_TZ_MAX];   /* POSIX TZ string, empty if unset */
    bool have_next;
    char next_id[AT_TIMER_ID_LEN];
    int64_t next_start_epoch;
    bool active;
    char active_id[AT_TIMER_ID_LEN];
    char note[AT_TIMER_NOTE_LEN];
} at_timer_status_t;
void at_app_timer_status(at_timer_status_t *out);

/* The list as a JSON array (the /api/timers format); malloc'd, the caller frees it. */
char *at_app_timers_json(void);
/* Parse a JSON array of timers and replace the stored list. Returns 200 on success, 400 if the text or any rule
 * is wrong (err says why; nothing changes), 500 if it could not be saved. */
int at_app_timers_put_json(const char *json, char *err, size_t err_len);

/* Time zone: an IANA name from the built-in table or a POSIX TZ string. False if unrecognised. */
bool at_app_set_timezone(const char *zone);
/* Set the clock from the phone. Refused (false) once the box has had network time this boot, or if epoch is
 * not plausible. */
bool at_app_set_time(int64_t epoch_s);
/* Ask for network time (called when an IP address is obtained). Idempotent. */
void at_app_clock_sntp_start(void);

/* The state document shared by the web UI (/api/status) and the MQTT state
 * topic. Returns a malloc'd JSON string; the caller frees it. */
char *at_app_state_json(void);
/* GET /api/fuel: the daily fuel log, newest first. Caller frees. */
char *at_app_fuel_json(void);

#ifdef __cplusplus
}
#endif
