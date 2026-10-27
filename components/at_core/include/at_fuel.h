#pragma once
/* Estimated diesel use. Without the extended telemetry (it needs debug mode on, and has not yet been confirmed
 * to stream with a panel fitted) the heater does not report its fuel pump rate, so fuel is estimated from the
 * status frame's state/substate: time at each power level x a per-level rate x a calibration factor.
 * When the extended frames do arrive, the pump's pulse count replaces that (at_fuel_tick_pump, once calibrated).
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

/* The tank: its size, and what the owner said was in it at some point. What is left now is that, less the
 * estimate's litres since. Saved as one blob: bump AT_FUEL_TANK_VERSION if the layout changes. */
#define AT_FUEL_TANK_VERSION 1
#define AT_FUEL_TANK_MAX_L 2000.0f
typedef struct {
    uint32_t version;
    float capacity_l;    /* 0 = not set */
    bool have_level;
    float level_l;       /* litres in the tank... */
    double used_at_set;  /* ...when the estimate's running total stood at this */
    int64_t set_epoch;   /* when that was (unix seconds), 0 without a clock */
} at_fuel_tank_t;

void at_fuel_tank_init(at_fuel_tank_t *t);
/* 1 to AT_FUEL_TANK_MAX_L. A level above the new size is brought down to it. False if out of range. */
bool at_fuel_tank_set_capacity(at_fuel_tank_t *t, float litres);
/* What is in the tank now: 0 to the capacity (or AT_FUEL_TANK_MAX_L before one is set). */
bool at_fuel_tank_set_level(at_fuel_tank_t *t, float litres, double used_total, int64_t epoch);
/* Litres left by the estimate, 0 to the capacity. False if no level was ever set. */
bool at_fuel_tank_remaining(const at_fuel_tank_t *t, double used_total, float *litres);

/* The fuel pump's own pulse count, from the extended telemetry's pump frequency (docs/PROTOCOL.md, field [15]).
 * Litres are then pulses x ml_per_pulse, which is a measurement rather than an estimate. ml_per_pulse is not a
 * datasheet number: it is worked out once from a real refill (at_fuel_pump_calibrate) and until then the
 * per-level rates above are used. Saved as one blob: bump AT_FUEL_PUMP_VERSION if the layout changes. */
#define AT_FUEL_PUMP_VERSION 1
#define AT_FUEL_PUMP_MIN_PULSES 500.0 /* fewer than this since the count was reset is too little to calibrate on */
typedef struct {
    uint32_t version;
    float ml_per_pulse; /* 0 = not calibrated: the estimate is used */
    double pulses;      /* counted since the count was last reset, whenever the pump frequency was known */
} at_fuel_pump_t;

void at_fuel_pump_init(at_fuel_pump_t *p);
bool at_fuel_pump_valid(const at_fuel_pump_t *p);
/* 0 clears the calibration, otherwise 0.005 to 0.2. False if out of range. */
bool at_fuel_pump_set_ml(at_fuel_pump_t *p, float ml_per_pulse);
/* Restart the count: do this when filling the tank to the mark that a calibration run starts from. */
void at_fuel_pump_reset_count(at_fuel_pump_t *p);
/* `litres` is what the pulses since the reset really burned (topped back up to the same mark). False if there
 * are too few pulses to say, or the result is out of range; the calibration is then left alone. */
bool at_fuel_pump_calibrate(at_fuel_pump_t *p, float litres);
/* Litres per hour at this pump frequency, 0 if not calibrated. */
float at_fuel_pump_rate_lph(const at_fuel_pump_t *p, float pump_hz);

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

/* As at_fuel_tick, but with the pump frequency (Hz, or < 0 if the extended telemetry is not streaming). With a
 * calibrated pump the litres are its pulses: that also counts fuel pumped outside a burn (priming), and nothing
 * while the heater is in a level with the pump stopped. The ignition phase, when the pump may not be pulsing yet,
 * keeps the starting rate. The pulse count runs whether or not it is calibrated. `pump` may be NULL. */
at_fuel_level_t at_fuel_tick_pump(at_fuel_totals_t *t, at_fuel_log_t *log, at_fuel_run_t *run, const at_fuel_cfg_t *cfg,
                                  at_fuel_pump_t *pump, float pump_hz, uint8_t state, uint8_t substate,
                                  bool burner_active, double dt_s, int32_t day);

/* The entry for `day`, or NULL if nothing was logged that day. */
const at_fuel_day_t *at_fuel_log_find(const at_fuel_log_t *log, int32_t day);
/* The i-th newest entry (0 = newest), or NULL past the end. */
const at_fuel_day_t *at_fuel_log_at(const at_fuel_log_t *log, int i);
