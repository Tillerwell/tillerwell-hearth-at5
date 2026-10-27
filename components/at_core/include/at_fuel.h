#pragma once
/* Estimated diesel use. The heater does not report its fuel pump rate on the panel bus (the extended
 * telemetry that has it has never been made to stream with a panel fitted), so fuel is estimated from the
 * status frame's state/substate: time at each power level x a per-level rate x a calibration factor.
 * Pure C, no ESP-IDF: host_tests/ covers it. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AT_FUEL_OFF = 0,  /* not burning: idle, cooldown, blowing, pump only */
    AT_FUEL_STARTING, /* ignition and combustion chamber heating */
    AT_FUEL_LOW,
    AT_FUEL_MIDDLE,
    AT_FUEL_HIGH,
    AT_FUEL_LEVELS
} at_fuel_level_t;

typedef struct {
    float rate_lph[AT_FUEL_LEVELS]; /* litres per hour at each level; [AT_FUEL_OFF] is always 0 */
    float factor;                   /* calibration, measured / estimated; 1 = the rates as they are */
    float price;                    /* per litre, in the owner's currency; 0 = not set */
} at_fuel_cfg_t;

/* Running totals since the box was set up. Saved to flash as one blob: keep the layout stable,
 * bump AT_FUEL_TOTALS_VERSION if it changes. */
#define AT_FUEL_TOTALS_VERSION 1
typedef struct {
    uint32_t version;
    double litres, cost;
    uint32_t level_s[AT_FUEL_LEVELS]; /* seconds at each level */
    uint32_t starts;                  /* times the heater began a start (idle -> starting) */
} at_fuel_totals_t;

/* One day's use, by local date. */
typedef struct {
    int32_t day;      /* days since 1970-01-01, local time */
    uint32_t ml;      /* millilitres */
    uint32_t cost_m;  /* cost x 1000 */
    uint16_t burn_min;
    uint16_t starts;
} at_fuel_day_t;

#define AT_FUEL_DAYS 366
typedef struct {
    at_fuel_day_t d[AT_FUEL_DAYS]; /* ring */
    uint16_t head;                 /* index of the newest day */
    uint16_t count;
} at_fuel_log_t;

/* Running state that is not saved. */
typedef struct {
    bool have_state;
    uint8_t last_state;
    double burn_s_frac, day_burn_s_frac; /* sub-minute remainders for burn_min */
    double day_ml_frac, day_cost_frac;
} at_fuel_run_t;

void at_fuel_cfg_defaults(at_fuel_cfg_t *cfg);
bool at_fuel_cfg_valid(const at_fuel_cfg_t *cfg);
void at_fuel_totals_init(at_fuel_totals_t *t);
void at_fuel_log_init(at_fuel_log_t *log);

at_fuel_level_t at_fuel_level(uint8_t state, uint8_t substate, bool burner_active);
const char *at_fuel_level_name(at_fuel_level_t level);
/* Litres per hour now, calibration applied. */
float at_fuel_rate_now(const at_fuel_cfg_t *cfg, at_fuel_level_t level);

/* Account `dt_s` seconds spent in `state`/`substate`. `day` is today's local date in days since
 * 1970-01-01, or -1 with no clock (totals still count, the daily log does not). Returns the level. */
at_fuel_level_t at_fuel_tick(at_fuel_totals_t *t, at_fuel_log_t *log, at_fuel_run_t *run, const at_fuel_cfg_t *cfg,
                             uint8_t state, uint8_t substate, bool burner_active, double dt_s, int32_t day);

/* The entry for `day`, or NULL if nothing was logged that day. */
const at_fuel_day_t *at_fuel_log_find(const at_fuel_log_t *log, int32_t day);
/* The i-th newest entry (0 = newest), or NULL past the end. */
const at_fuel_day_t *at_fuel_log_at(const at_fuel_log_t *log, int i);
