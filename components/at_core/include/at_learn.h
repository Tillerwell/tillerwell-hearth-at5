/*
 * Heater Care learning (docs/ADAPTIVE_THERMOSTAT.md §3.2): how fast the space heats, and the thermostat range width
 * that makes a run take about 40 minutes from flame established to the top.
 *
 * Pure C over plain data, fed by at_ctl once per tick, so host_tests/ covers it with recorded readings. No ESP-IDF,
 * no clock of its own: times are the uptime clock in ms (differences only), epochs come from the caller for the
 * history entries.
 *
 * What is learned belongs to the space: one learned width, changed by whichever sensor is in use, with a 3-run
 * window, warm-up delays, cooling times and a limit state kept apart for each sensor (the external room sensor and
 * the Comfort Controller's whole-degree reading).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_LEARN_VERSION     1
#define AT_LEARN_WIN         3       /* counted runs per sensor a decision looks at */
#define AT_LEARN_MED         10      /* warm-up delays and cooling times kept per sensor */
#define AT_LEARN_HIST        20      /* history entries (changes and events) */
#define AT_LEARN_RUNS        20      /* run log, RAM only */
#define AT_LEARN_MAX_W       4.0f    /* widest learned/working range, unless the user's own is wider */
#define AT_LEARN_CTL_FLOOR   2.0f    /* narrowest working range on the controller's whole-degree sensor */
#define AT_LEARN_CTL_SLACK   1.0f    /* whole-degree sensor: real swing = working width - this; its working width is
                                        * the learned width in whole degrees + this (never saved) */
#define AT_LEARN_TARGET_MIN  40.0f   /* climb time aimed at, from flame established */
#define AT_LEARN_FAST_MIN    30.0f   /* a climb faster than this is "fast" */
#define AT_LEARN_SLOW_MIN    60.0f   /* slower than this is "slow" */
#define AT_LEARN_MAX_OFF_MS  (2u * 3600u * 1000u)  /* off longer than this before a run: not counted */
#define AT_LEARN_MAX_WARM_MS (20u * 60000u)        /* warm-up delay longer than this: not enough data */
#define AT_LEARN_MIN_FIT_MS  (10u * 60000u)        /* fewer minutes of readings to fit than this: not enough data */
#define AT_LEARN_MIN_RATE    0.005f                /* C per minute: below this the room lost heat */
#define AT_LEARN_COOL_MIN_FALL  0.2f               /* a run started above the working bottom (1.60): the room fell at
                                                    * least this far from its peak for its cooling time to be kept, */
#define AT_LEARN_COOL_MAX_SCALE 5.0f               /* scaled to the whole fall by at most this much */
#define AT_LEARN_LOST_HEAT   0.5f                  /* a fall this far below the run's highest reading: lost heat */

typedef enum { AT_SENS_EXT = 0, AT_SENS_CTL = 1 } at_sensor_t;
typedef enum { AT_LIMIT_NONE = 0, AT_LIMIT_MAX = 1, AT_LIMIT_MIN = 2 } at_limit_t;
typedef enum { AT_VOTE_OK = 0, AT_VOTE_FAST = 1, AT_VOTE_SLOW = 2 } at_vote_t;
typedef enum {
    AT_EV_NONE = 0,
    AT_EV_WIDENED,
    AT_EV_NARROWED,
    AT_EV_LIMIT,          /* limit reached (`limit` says which) */
    AT_EV_LIMIT_CLEARED,
    AT_EV_RESET,
} at_learn_ev_t;

/* How a run ended (thermo.last_run_end). */
typedef enum {
    AT_END_NONE = 0,
    AT_END_TOP,           /* Hearth's stop at the top, after the minimum run */
    AT_END_USER,          /* a person asked for a stop (app, device web UI, console) */
    AT_END_EXTERNAL,      /* another system asked (Matter/HomeKit, Home Assistant, MQTT, the Comfort Controller) */
    AT_END_CEILING,       /* the room reached the ceiling */
    AT_END_FAULT,         /* the heater stopped on a fault */
    AT_END_NO_READING,    /* every temperature reading was lost; stopped after the minimum run */
    AT_END_HEATER,        /* the heater stopped by itself, no stop seen and no fault */
    AT_END_TIMER,         /* a thermostat timer ended; stopped after the minimum run */
    AT_END_THERMO_OFF,    /* the thermostat was turned off with "let it finish"; stopped after the minimum run */
} at_run_end_t;

/* Why a run did or did not count towards learning. */
typedef enum {
    AT_WHY_COUNTED = 0,
    AT_WHY_NOT_THERMOSTAT,   /* not started by Hearth's thermostat */
    AT_WHY_LONG_OFF,         /* off 2 hours or more before it, or not known (after a restart) */
    AT_WHY_ENDED,            /* did not end at the top with Hearth's stop */
    AT_WHY_SENSOR_CHANGED,
    AT_WHY_LOST_HEAT,        /* "lost heat mid-run, not counted" */
    AT_WHY_NOT_ENOUGH_DATA,
    AT_WHY_COLD_SPELL,       /* "cold spell, not counted" (a slow run after a fast cool-down) */
} at_learn_why_t;

typedef struct {
    float need;              /* real swing needed for a 40 minute climb, C (before rounding), on either sensor */
    float rate;              /* rise rate, C per minute */
    uint8_t vote;            /* at_vote_t */
} at_learn_vote_t;

typedef struct {
    int64_t epoch;           /* 0 when the clock was not set */
    uint8_t kind;            /* at_learn_ev_t */
    uint8_t sensor;          /* at_sensor_t */
    uint8_t limit;           /* AT_EV_LIMIT / AT_EV_LIMIT_CLEARED: which limit */
    float old_w, new_w;      /* learned width (the real swing) before and after */
    float raw_w;             /* the width worked out, before rounding */
    float work_w;            /* the working width on that sensor afterwards */
    float rate1, rate2;      /* the rise rates of the two runs behind it, 0 if none */
} at_learn_event_t;

typedef struct {
    uint8_t n;                               /* votes in the window, oldest first */
    at_learn_vote_t v[AT_LEARN_WIN];
    uint8_t nwarm;                           /* warm-up delays, oldest first */
    uint16_t warm_s[AT_LEARN_MED];
    uint8_t ncool;                           /* cooling times, oldest first */
    uint32_t cool_s[AT_LEARN_MED];
    uint8_t limit;                           /* at_limit_t */
} at_learn_sensor_t;

/* Everything kept across a restart and in the backup. One NVS blob; `version` guards its layout. */
typedef struct {
    uint16_t version;
    uint8_t last_change;                     /* AT_EV_WIDENED / AT_EV_NARROWED / 0: the last width change */
    float width;                             /* the real swing the space needs: 0 = nothing learned yet; else 1..4
                                              * in 0.5 steps, the same number on both sensors */
    at_learn_sensor_t s[2];                  /* [AT_SENS_EXT], [AT_SENS_CTL] */
    uint8_t nhist;                           /* oldest first */
    at_learn_event_t hist[AT_LEARN_HIST];
} at_learn_store_t;

/* One run, for the "Why?" screen. */
typedef struct {
    int64_t start_epoch;     /* 0 when the clock was not set */
    uint32_t light_s;        /* switch-on to flame established (0 if never) */
    uint32_t burn_s;         /* flame established to the stop (or the end) */
    uint8_t sensor;
    uint8_t end;             /* at_run_end_t */
    uint8_t why;             /* at_learn_why_t */
    uint8_t vote;            /* at_vote_t, counted runs only */
    bool have_rate;
    bool rate_ok;            /* the rise rate is a good fit, whatever the run's end (warm by, at_warm) */
    float start_temp, peak_temp;
    float rate, need, climb_min;
    uint16_t warm_s;         /* this run's warm-up delay, 0xffff if never seen */
    /* the off period before this run */
    bool have_cool;
    uint32_t cool_s;         /* peak after the stop to the working bottom (from 1.60 scaled to it when the next run
                              * started higher up, docs/EARLY_START.md §6) */
    float dip;               /* how far below the bottom the room fell during the off lock */
} at_learn_run_t;

typedef struct {
    at_learn_store_t st;
    bool dirty;              /* st changed: the caller saves it */
    int64_t epoch;           /* set by the caller before each call, 0 = clock not set */

    /* current run */
    bool in_run;
    uint32_t on_ms;
    bool flame;
    uint32_t flame_ms;
    uint8_t sensor;
    bool sensor_changed;
    bool by_thermo;
    bool off_ok;             /* off under 2 hours before it */
    bool have_start_temp;
    float start_temp, peak_temp;
    bool have_base;
    float base;              /* lowest reading from flame established on, until the warm-up ends */
    bool warm_done;
    uint32_t warm_ms;        /* warm-up end */
    bool have_last;
    float last;              /* last sample, to see whole-degree tick-overs */
    bool fit_frozen;         /* Hearth (or someone) sent the stop: later readings are the purge */
    uint32_t stop_ms;
    double sx, sy, sxx, sxy; /* least squares over (minutes since flame, C) */
    int n;
    uint32_t fit_first_ms, fit_last_ms;
    float maxv;
    bool lost_heat;
    bool resumed;            /* stopped, then started again before it went idle: not counted */

    /* the off period since the last stop at the top */
    bool cool_on;
    uint8_t cool_sensor;
    bool cool_bad;           /* sensor changed: not recorded */
    bool have_peak;
    float peak;
    uint32_t peak_ms;
    bool reached_bottom;
    uint32_t cool_ms;
    bool cool_last_ok;       /* the latest reading on the way down (1.60): a run started above the working bottom */
    float cool_last_temp;    /* scales the cooling seen so far to the whole fall (at_learn_run_start) */
    uint32_t cool_last_ms;
    float cool_low;          /* the working bottom at that reading */
    float dip;               /* during the current/last off lock, for the status */
    bool last_have_cool;
    uint8_t last_cool_sensor;
    uint32_t last_cool_s;
    float last_dip;

    at_learn_run_t runs[AT_LEARN_RUNS];     /* oldest first */
    int nruns;
} at_learn_t;

void at_learn_init(at_learn_t *L);
/* Put a saved store back (NVS or a backup). False, and nothing changes, if it does not make sense. */
bool at_learn_restore(at_learn_t *L, const at_learn_store_t *st);
/* Forget everything learned (Reset learning), with one history entry. */
void at_learn_reset(at_learn_t *L);

/* The working width: the user's range, widened by the learned width (on the controller's sensor the learned width
 * rounded up to a whole degree plus 1 C, when anything is learned), at least 2 C on the controller's sensor, capped
 * at 4 C unless the user's own range is wider. */
float at_learn_working_width(float learned, float user_w, bool ctl_sensor);
/* Round up to the next 0.5 C, or the next whole degree. */
float at_learn_round_up(float w, bool whole);

/* The run's hooks, in order. */
void at_learn_run_start(at_learn_t *L, uint32_t now_ms, at_sensor_t sensor, bool by_thermo, bool off_known,
                        uint32_t off_ms);
void at_learn_flame(at_learn_t *L, uint32_t now_ms);
/* Every tick with a usable (filtered) reading, in a run or not. off_lock: the minimum off lock is holding a start. */
void at_learn_sample(at_learn_t *L, uint32_t now_ms, float temp, at_sensor_t sensor, float work_low,
                     bool off_lock);
void at_learn_sensor_changed(at_learn_t *L);
/* A stop was sent (by Hearth at_top, or by someone): the fit ends here; at the top the cooling record starts. */
void at_learn_stop(at_learn_t *L, uint32_t now_ms, bool at_top);
/* A start was asked for after that stop and the run carries on: it was broken by the stop, so it is never counted
 * (logged "not_at_top"), and no cooling record follows it. */
void at_learn_resume(at_learn_t *L);
/* The heater is idle again: judge the run and decide. user_w is the user's range width now. */
void at_learn_run_end(at_learn_t *L, uint32_t now_ms, at_run_end_t end, float user_w);
/* Drop limit states that are no longer true (the user's range changed). */
void at_learn_check_limits(at_learn_t *L, float user_w);

/* For the status, on the given sensor: the last counted run's rise rate, the typical warm-up delay and cooling
 * time. False until known. */
bool at_learn_rate(const at_learn_t *L, at_sensor_t s, float *rate);
bool at_learn_warm_typical(const at_learn_t *L, at_sensor_t s, uint32_t *sec);
bool at_learn_cool_typical(const at_learn_t *L, at_sensor_t s, uint32_t *sec);

const char *at_learn_ev_name(at_learn_ev_t e);
const char *at_learn_end_name(at_run_end_t e);
const char *at_learn_why_name(at_learn_why_t w);
const char *at_learn_vote_name(at_vote_t v);
const char *at_learn_limit_name(at_limit_t l);   /* NULL for none */
const char *at_learn_sensor_name(at_sensor_t s);

#ifdef __cplusplus
}
#endif
