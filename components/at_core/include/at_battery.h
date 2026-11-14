#pragma once
/* Estimated battery use (firmware 1.62). The heater reports no current, so, like the fuel estimate, this is time in
 * each phase x a power for that phase, from the status frame's state/substate. The watts are Autoterm's published
 * figures for the Flow 5D / Binar-5S (start-up 122 W for 100 s, at most 42 W running with the pump) with the lower
 * levels scaled between the pump alone and that maximum, and agree with one measured start (about 110 W for 2 min,
 * then about 37 W at High). Amp-hours are watts / the supply voltage the heater reports, so a 24 V heater counts
 * half the amp-hours. The heater's standby draw (30-40 mA) and the box's own are not counted. `factor` calibrates
 * it against a battery monitor. Pure C, no ESP-IDF: host_tests/ covers it. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AT_BATT_OFF = 0,  /* idle, locked: nothing counted */
    AT_BATT_STARTING, /* glow plug, ignition, chamber heating (the glow plug's own energy is added once per start) */
    AT_BATT_LOW,
    AT_BATT_MIDDLE,
    AT_BATT_HIGH,
    AT_BATT_PUMP,     /* water pump only, or waiting with the pump running */
    AT_BATT_FAN,      /* blowing: purge, cooldown */
    AT_BATT_PHASES
} at_batt_phase_t;

#define AT_BATT_START_W       122.0f /* Autoterm: start-up power input... */
#define AT_BATT_START_S       100.0f /* ...for this long */
#define AT_BATT_NOMINAL_V     12.0f  /* used while the heater has not reported a believable voltage */
#define AT_BATT_FACTOR_MIN    0.2f
#define AT_BATT_FACTOR_MAX    5.0f

/* Running totals. Saved to flash as one blob: keep the layout stable, bump AT_BATT_TOTALS_VERSION if it changes. */
#define AT_BATT_TOTALS_VERSION 1
typedef struct {
    uint32_t version;
    double ah, wh;    /* since the box was set up */
    int32_t day;      /* the local date ah_day belongs to (days since 1970-01-01), -1 for none */
    double ah_day;
    double ah_run;    /* the run going, or the last one: reset when a start begins */
} at_batt_totals_t;

/* Running state that is not saved. */
typedef struct {
    bool have_state;
    uint8_t last_state;
} at_batt_run_t;

void at_batt_totals_init(at_batt_totals_t *t);
bool at_batt_totals_valid(const at_batt_totals_t *t);
bool at_batt_factor_valid(float factor);

at_batt_phase_t at_batt_phase(uint8_t state, uint8_t substate, bool burner_active);
const char *at_batt_phase_name(at_batt_phase_t phase);
/* Watts in this phase, before the factor. */
float at_batt_watts(at_batt_phase_t phase);
/* The voltage to divide by: the heater's reading if it is believable (9-32 V), else AT_BATT_NOMINAL_V. */
float at_batt_volts(float supply_v);

/* Account `dt_s` seconds in `state`/`substate` at `supply_v`. `day` is today's local date in days since 1970-01-01,
 * or -1 with no clock (the totals still count, today's does not). Returns the phase. */
at_batt_phase_t at_batt_tick(at_batt_totals_t *t, at_batt_run_t *run, float factor, uint8_t state, uint8_t substate,
                             bool burner_active, float supply_v, double dt_s, int32_t day);
/* Amp-hours for `day`, 0 if nothing was counted that day. */
double at_batt_ah_today(const at_batt_totals_t *t, int32_t day);
