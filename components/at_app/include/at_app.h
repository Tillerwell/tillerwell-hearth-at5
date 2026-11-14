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
#include "at_runlog.h"
#include "at_fuel.h"
#include "at_battery.h"
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

/* Thermostat band (Auto): the user's range, low..high. hyst_* are no longer used (1.47, Heater Care): kept so the
 * saved entry keeps its layout. Kept in its own NVS entry so older saved settings stay valid. */
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
/* Removes a blob; fine if it was never there. */
esp_err_t at_settings_blob_erase(const char *key);

#define AT_DISPLAY_NAME_MAX 32
#define AT_HOSTNAME_MAX 31
/* The user's name for the box (empty by default), and the hostname derived from it for DHCP/mDNS. device_name stays the
 * stable hearth-xxxxxx id; the hostname is that id until a display name is set, and again after a factory reset. */
void at_settings_display_name(char *out, size_t len);
esp_err_t at_settings_display_name_set(const char *name);
void at_settings_hostname(char *out, size_t len);

/* Automatic firmware updates (at_autoupdate): off until an admin turns them on. Own NVS entry, like the display name. */
bool at_settings_auto_update(void);
esp_err_t at_settings_auto_update_set(bool on);
/* Free heap after each start-up step (1.0046), for GET /api/info: what each part of the firmware holds. Called from
 * app_main only, in order; the stage names must be string literals. */
#define AT_BOOT_MARKS 10
typedef struct {
    const char *stage;
    uint32_t free, largest;
} at_boot_mark_t;
void at_app_boot_mark(const char *stage);
int at_app_boot_marks(at_boot_mark_t *out, int max);
/* The last crash, kept across the restart in RTC memory (at_crash.c, 1.0046). Lost when the power goes. */
#define AT_CRASH_STACK 12
typedef struct {
    uint32_t pc, ra, sp, cause, mtval; /* the RISC-V exception frame: mepc, ra, sp, mcause, mtval */
    uint32_t uptime_s;                 /* how long the box had been up */
    uint32_t crashes;                  /* crashes since the power was last connected */
    uint32_t boots_since;              /* restarts since this crash; 0 = the last restart was this crash */
    char task[16];                     /* the task that was running */
    char why[96];                      /* the abort() or assert message, else the exception name */
    uint32_t stack[AT_CRASH_STACK];    /* code addresses found on that task's stack, nearest first */
    uint8_t stack_n;
} at_crash_t;
/* Called once at start-up, before anything else reads it. */
void at_crash_init(void);
bool at_crash_last(at_crash_t *out);
/* The built-in MQTT broker and client (1.0046+: off unless an admin turns them on; Home Assistant uses HTTP). */
bool at_settings_mqtt(void);
esp_err_t at_settings_mqtt_set(bool on);

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
    float auto_low, auto_high, hyst_min, hyst_max;  /* hyst_*: always 0.5 + 0.5, for older apps (1.47) */
    float auto_start_at;
    at_ctl_status_t thermo;     /* Heater Care: working range, locks, learning */
    bool clock_ok;
    int64_t epoch;              /* wall clock when the view was taken, 0 if not set */
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

/* Commands. Return false if nothing was queued (bypass on, queue full) or the Heater Care lock refused it.
 *
 * The Heater Care lock (1.61, docs/CARE_LOCK.md): a stop inside the minimum run, or a start inside the 20 minute rest
 * after a run, is refused unless override is set. Only a signed request from the apps may set it (at_web); Matter,
 * MQTT, Home Assistant, the website and timers never do. The plain functions are the locked ones, no override. */
typedef struct {
    at_care_t care;      /* AT_CARE_OK if the lock did not refuse it */
    uint32_t left_s;     /* how long until it would be allowed */
} at_care_why_t;
bool at_app_start_preheat_care(int minutes, bool override, at_care_why_t *why);
bool at_app_start_thermostat_care(bool override, at_care_why_t *why);
bool at_app_stop_care(at_src_t src, bool override, at_care_why_t *why);
/* Auto on or off. override (Auto on only): the app's Start anyway inside the rest, so the first start need not wait. */
void at_app_set_auto_care(bool enabled, bool finish_run, bool override, at_src_t src);
bool at_app_start_preheat(int minutes);       /* minutes <= 0 uses the saved default */
/* Change the total time of a preheat that is running (minutes from when it started). */
bool at_app_set_preheat_duration(int minutes);
bool at_app_start_thermostat(void);
/* Stop at once. Logged as an early stop (with who asked) if it is inside the minimum run. */
bool at_app_stop(void);                       /* source unknown */
bool at_app_stop_from(at_src_t src);
/* The water (circulation) pump alone; minutes <= 0 uses the saved run time (at_app_pump_minutes). */
bool at_app_start_pump(int minutes);
#define AT_PUMP_MINUTES_DEFAULT 40
int at_app_pump_minutes(void);
void at_app_set_pump_minutes(int minutes);
/* Prime the fuel pump after the tank ran dry, and spin the blower on its own. Both only while the heater is idle,
 * its status fresh, and the box's thermostat not armed (it would start the heater part-way). Stop ends either. */
bool at_app_fuel_prime(void);
bool at_app_fan_test(void);
/* Change the heater's own coolant target (20-95, -1 keeps it) and water pump in standby (1, 0, -1 keeps it). Needs
 * the settings block read first (at_app_read_heater_info, or the controller's own read at power-up). */
bool at_app_write_heater_settings(int wait_temp, int pump_standby);
bool at_app_send_pubr0(void);
bool at_app_send_pubr0_variant(int variant); /* see at_cmd_send_pubr0_variant */
/* Read the heater's version, settings block and operation report (docs/PROTOCOL.md, "Vendor test utility
 * decoded"). The replies land in at_app_heater_info over the next few seconds. */
bool at_app_read_heater_info(void);
/* Clear the heater's lockout. Refused unless the heater is idle or locked and no Hearth run is going. */
bool at_app_unlock(void);
/* The heater's periodic parameter stream (the extended frame) on or off, the vendor tool's way. */
bool at_app_diag_stream(bool on);
/* A copy of what the heater has told us; now_ms is the clock its times are measured against. */
void at_app_heater_info(at_heater_info_t *out, uint32_t *now_ms);

/* ---- fuel estimate (at_app_fuel.c) ---------------------------------------- */

typedef struct {
    at_fuel_cfg_t cfg;
    at_fuel_totals_t totals;
    at_fuel_level_t level;
    float rate_lph;           /* now, calibration applied */
    bool have_today;
    at_fuel_day_t today;
    at_fuel_tank_t tank;
    bool have_tank_left;      /* a level was set */
    float tank_left_l;
    at_fuel_pump_t pump;
    bool from_pump;           /* rate_lph is the pump's measured rate, not the per-level estimate */
    float pump_hz;            /* last measured, 0 when not streaming */
} at_fuel_view_t;

void at_app_fuel_init(void);
/* Called every few seconds with a fresh snapshot: accounts the time since the last call. */
void at_app_fuel_tick(const at_snapshot_t *snap, uint32_t now_ms);
void at_app_fuel_view(at_fuel_view_t *out);
/* Newest first; returns how many were copied. */
int at_app_fuel_log_copy(at_fuel_day_t *out, int max);
/* Each returns false (and changes nothing) if the value is out of range. */
bool at_app_fuel_set_price(float per_litre);
/* The price backdated to a local date (days since 1970-01-01): it is set, and every logged day from that date
 * up to today is costed again at it (1.63). The date must be today or up to 365 days back, which needs the clock. */
bool at_app_fuel_set_price_from(float per_litre, int32_t from_day);
bool at_app_fuel_set_factor(float factor);
bool at_app_fuel_set_rates(float starting, float low, float middle, float high);
/* The tank's size, what is in it now, or "filled to the top" (needs the size). */
/* Pump calibration (see at_fuel.h): reset the count at a known fill, burn, refill to the same mark, then
 * calibrate with the litres it took. set_ml_per_pulse(0) goes back to the estimate. */
bool at_app_fuel_pump_reset_count(void);
bool at_app_fuel_pump_calibrate(float litres);
bool at_app_fuel_set_ml_per_pulse(float ml);
bool at_app_fuel_set_tank_capacity(float litres);
bool at_app_fuel_set_tank_level(float litres);
bool at_app_fuel_tank_filled(void);
/* Battery-use estimate (1.62, at_battery.h). */
typedef struct {
    at_batt_phase_t phase;
    float factor;
    float watts;     /* now, factor applied */
    float volts;     /* what the amp-hours are divided by */
    double ah_total, wh_total;
    double ah_today; /* 0 without a clock */
    double ah_run;   /* the run going, or the last one */
} at_battery_view_t;

void at_app_battery_init(void);
void at_app_battery_tick(const at_snapshot_t *snap, uint32_t now_ms);
void at_app_battery_view(at_battery_view_t *out);
/* AT_BATT_FACTOR_MIN to AT_BATT_FACTOR_MAX; false (and nothing changed) otherwise. */
bool at_app_battery_set_factor(float factor);

/* Today's local date in days since 1970-01-01; false without a clock and time zone. */
bool at_app_local_day(int32_t *day);

void at_app_set_auto(bool enabled);
/* Auto on/off. Turning it off inside a run's minimum run: finish_run lets the run reach it (the default for callers
 * that cannot ask), otherwise it stops now and is logged as an early stop from src. */
void at_app_set_auto_from(bool enabled, bool finish_run, at_src_t src);
void at_app_set_auto_target(float target);
/* Keep the cabin between low and high (0..30 C, at least 1 C apart). Lowering it below the room inside a run's
 * minimum run: finish_run lets the run reach it, otherwise it stops now (an early stop from src). */
void at_app_set_auto_range(float low, float high);
void at_app_set_auto_range_from(float low, float high, bool finish_run, at_src_t src);
/* Heater Care: forget what was learned; the apps showed the early-stop explainer. */
void at_app_thermo_learn_reset(void);
void at_app_early_stop_seen(void);
/* A consistent copy of the learning, the early-stop log and the clock it is measured against (for the JSON). */
bool at_app_learn_snapshot(at_learn_t *out, at_ctl_early_t *early, int *n_early, uint32_t *now_ms);
/* Replace the learned store (a backup being restored) and save it. False if it does not make sense. */
bool at_app_learn_put(const at_learn_store_t *st);
/* GET /api/thermo/history (docs/API_SPEC.md): the learning history and the last runs. Caller frees. */
char *at_app_thermo_history_json(void);
/* The learning as the backup's "thermo_learn" object / put one back. */
struct cJSON;
struct cJSON *at_app_thermo_learn_backup(void);
/* The backup's "heater_care" object: GET /api/thermo/history without "early_stops", so the website can show Heater
 * Care to apps away from the box. Raw JSON node; NULL when out of memory. */
struct cJSON *at_app_thermo_care_backup(void);
bool at_app_thermo_learn_restore(const struct cJSON *o);
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
/* True while the heater runs on only to finish a timer's minimum run: the next thermostat timer may take it over. */
bool at_app_timer_can_take_over(void);
/* Warm by (1.56, docs/ADAPTIVE_THERMOSTAT.md §10). Plan: the lead for timer t starting at start_epoch, worked out from
 * the room now. Begin: as at_app_timer_thermostat_begin, following the run for the arrival error; result_hint an
 * at_warm_res_t, *room the reading now (NAN if none). Frost run: the run going is frost protection's. */
bool at_app_warm_plan(const at_timer_t *t, int64_t start_epoch, at_warm_plan_t *out);
bool at_app_warm_begin(const at_timer_t *t, int64_t start_epoch, const at_warm_plan_t *plan, uint8_t result_hint,
                       float *room);
bool at_app_timer_frost_run(void);
/* The median lead of the last warm-by runs in minutes (false if none), and the estimate a prediction would use now
 * (at_warm_est_t) for a set temperature of top. */
bool at_app_warm_typical_lead(uint32_t *min);
uint8_t at_app_warm_estimate(float top);
/* The warm-by store as the backup's lists / put lists back (only the keys present). */
bool at_app_warm_snapshot(at_warm_store_t *out);
bool at_app_warm_put(const at_warm_store_t *st);
bool at_app_coast_snapshot(at_coast_store_t *out);
bool at_app_coast_put(const at_coast_store_t *st);
/* Early start's busy widening (1.60), for the backup. */
bool at_app_early_snapshot(at_early_store_t *out);
bool at_app_early_put(const at_early_store_t *st);
/* The heater's learned cooldown lengths (1.52) and Heater Care's run log (1.55), for the backup (1.58): copy out / put
 * back and save. False if what is put back does not make sense. */
bool at_app_cd_snapshot(at_cd_store_t *out);
bool at_app_cd_put(const at_cd_store_t *st);
/* Merge runs into the log (at_runlog_merge): display only, nothing learned changes. */
bool at_app_runlog_merge(const at_learn_run_t *in, int n);
/* Smallest allowed width of an Auto range: 1 C (Heater Care widens the working range itself). */
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
    bool next_warm_by;          /* the next timer starts early to be warm by its start (1.56) */
    int64_t next_expected_epoch; /* warm by: when it is expected to start (or did) */
    uint8_t next_estimate;      /* warm by: at_warm_est_t behind that */
    int64_t next_earliest_epoch; /* the soonest the next timer can start the heater (start less 2 h for warm by) */
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
/* Add or replace one timer (matched by id) / remove one, keeping the rest; same rules and return codes (404: no
 * timer with that id). `timer` is a cJSON object in the /api/timers format. */
struct cJSON;
int at_app_timer_set_json(const struct cJSON *timer, char *err, size_t err_len);
int at_app_timer_delete(const char *id, char *err, size_t err_len);

/* Time zone: an IANA name from the built-in table or a POSIX TZ string. False if unrecognised. */
bool at_app_set_timezone(const char *zone);
/* Set the clock from the phone. Refused (false) once the box has had network time this boot, or if epoch is
 * not plausible. */
bool at_app_set_time(int64_t epoch_s);
/* True once network time (SNTP) has arrived this boot. Unlike time_set it is false when only the phone set the clock:
 * the cloud link trusts nothing else, since a phone can be wrong. */
bool at_app_clock_synced(void);
/* Ask for network time (called when an IP address is obtained). Idempotent. */
void at_app_clock_sntp_start(void);

/* The state document shared by the web UI (/api/status) and the MQTT state
 * topic. Returns a malloc'd JSON string; the caller frees it. */
char *at_app_state_json(void);
/* The same document without a copy: fn gets it while the build lock is held (keep fn short, and do not ask for
 * the state again inside it). ESP_ERR_NO_MEM when there is no document to give, else what fn returned. */
esp_err_t at_app_state_json_with(esp_err_t (*fn)(const char *js, void *ctx), void *ctx);
/* Called once by at_app_start, before anything asks for the state. */
void at_app_state_json_init(void);
/* GET /api/fuel: the daily fuel log, newest first. Caller frees. */
char *at_app_fuel_json(void);
/* GET /api/backup and the cloud's `backup` topic (docs/API_SPEC.md §7.5): every setting an owner chose, and nothing
 * that belongs to this box alone (Wi-Fi, users, keys, fuel history) or to the moment (whether it is running). Caller frees. */
char *at_app_backup_json(void);

#ifdef __cplusplus
}
#endif
