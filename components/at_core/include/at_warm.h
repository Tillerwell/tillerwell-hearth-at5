/*
 * Warm by a set time (docs/ADAPTIVE_THERMOSTAT.md §10): how long before a thermostat timer's start the heater must
 * light so the room is at the timer's set temperature at the start time, and learning from how that went.
 *
 * The model (§10.2): the room climbs at the heater's gross heating rate less what the space is losing right now.
 *  - Gross heating rate: each run's fitted rise rate (at_learn) plus the cooling slope measured while the heater was
 *    idle just before it. Kept per sensor, median of the last 10.
 *  - Current loss: the idle cooling slope over the last 30 to 60 minutes.
 *  - Lead = light-up + warm-up + climb / (gross - loss) + margin + learned correction + any minimum off left,
 *    capped at AT_WARM_MAX_LEAD_MIN.
 * Fallbacks (§10.3): the plain typical rise rate on the sensor in use with a fixed margin, then a fixed 60 minutes.
 *
 * Kept apart from the at_learn store (its own small versioned NVS blob, "ctl_warm"), so nothing already learned is
 * touched by the update. The idle cooling tracker is RAM only.
 *
 * Pure C over plain data, fed by at_ctl, so host_tests/ covers it. Times are uptime ms; epochs come from the caller.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "at_learn.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AT_WARM_VERSION        1
#define AT_WARM_KEEP           10        /* light-up times, gross and plain rise rates kept (each sensor) */
#define AT_WARM_ERR_KEEP       5         /* arrival errors and leads kept */
#define AT_WARM_REC            5         /* warm-by runs kept for the "Why?" screen */
#define AT_WARM_MIN_LEARN      3         /* rates needed before a step of the prediction is used */
#define AT_WARM_IDLE_PTS       60        /* idle tracker: one averaged reading a minute, the last hour */
#define AT_WARM_GROSS_IDLE_MIN 15        /* idle minutes before a run for its gross rate */
#define AT_WARM_LOSS_MIN       30        /* idle minutes for the current loss */
#define AT_WARM_PEAK_FALL      0.3f      /* after a run the tracker starts once the room is this far below its peak */
#define AT_WARM_MAX_LEAD_MIN   120       /* longest early start */
#define AT_WARM_FIXED_LEAD_MIN 60        /* lead with nothing learned */
#define AT_WARM_MIN_CLIMB_MIN  30        /* a start inside the range needs a climb from flame at least this long */
#define AT_WARM_LATE_MIN       30        /* not there by start + this: counted as this late */
#define AT_WARM_CORR_MAX_MIN   30        /* the learned correction is clamped to +- this */
#define AT_WARM_DEF_LIGHT_S    300       /* light-up time before one is learned */
#define AT_WARM_DEF_WARM_S     300       /* warm-up delay before one is learned */

typedef enum { AT_WARM_EST_NONE = 0, AT_WARM_EST_MODEL, AT_WARM_EST_TYPICAL, AT_WARM_EST_FIXED } at_warm_est_t;

/* What became of a warm-by run (the history's "result"). Only COUNTED adds an arrival error. */
typedef enum {
    AT_WARM_RES_PENDING = 0,
    AT_WARM_RES_COUNTED,     /* started when the prediction asked: its error is kept */
    AT_WARM_RES_CAPPED,      /* the prediction wanted more than the longest early start */
    AT_WARM_RES_DELAYED,     /* held back (a busy heater, the minimum off, another timer) */
    AT_WARM_RES_IN_RANGE,    /* started inside the range, or not started at all because the room was warm enough */
    AT_WARM_RES_AT_START,    /* fired at the start time: the prediction never got there */
    AT_WARM_RES_FALLBACK,    /* the estimate was not the full model: its own fixed margin, no correction */
    AT_WARM_RES_STOPPED,     /* someone stopped the run, or it ended on a fault, before the room got there */
} at_warm_res_t;

typedef struct {
    int64_t start_epoch;     /* the timer's start time */
    int64_t fired_epoch;     /* when it armed the thermostat */
    int64_t predicted_epoch; /* when the prediction said the room would reach the top */
    int64_t actual_epoch;    /* when it did, 0 if not by start + 30 min */
    int16_t start_temp_dc;   /* room at the firing, tenths of a degree */
    int8_t target_c;
    uint8_t est;             /* at_warm_est_t */
    uint8_t result;          /* at_warm_res_t */
    int8_t err_min;          /* actual - predicted, minutes (COUNTED only, else 0) */
    uint16_t lead_min;       /* the lead used */
} at_warm_rec_t;

/* Kept across a restart and in the backup. */
typedef struct {
    uint8_t version;
    uint8_t nlight;
    uint8_t ngross[2];                       /* [AT_SENS_EXT], [AT_SENS_CTL] */
    uint8_t nrise[2];
    uint8_t nerr;
    uint8_t nlead;
    uint8_t nrec;
    uint16_t light_s[AT_WARM_KEEP];          /* oldest first */
    float gross[2][AT_WARM_KEEP];            /* C per minute */
    float rise[2][AT_WARM_KEEP];
    int8_t err_min[AT_WARM_ERR_KEEP];
    uint16_t lead_min[AT_WARM_ERR_KEEP];
    at_warm_rec_t rec[AT_WARM_REC];          /* oldest first */
} at_warm_store_t;

/* What the predictor is told about now. */
typedef struct {
    bool have_temp;
    float temp;              /* the room now */
    uint8_t sensor;          /* at_sensor_t in use */
    float top;               /* the timer's set temperature */
    uint32_t off_left_s;     /* minimum off still to run */
    bool have_warm;
    uint32_t warm_s;         /* typical warm-up delay on that sensor (at_learn) */
} at_warm_in_t;

typedef struct {
    uint8_t est;             /* at_warm_est_t */
    uint32_t lead_s;         /* capped at AT_WARM_MAX_LEAD_MIN */
    uint32_t raw_lead_s;     /* before the cap */
    bool capped;
    float rate;              /* predicted rise rate, C per minute (0 for fixed) */
    float loss;              /* current loss (model only) */
    float flame_climb_min;   /* flame established to the top: warm-up + climb, no margin */
    float corr_min;          /* learned correction added (model only) */
} at_warm_pred_t;

typedef struct {
    at_warm_store_t st;
    bool dirty;              /* st changed: the caller saves it */

    /* idle cooling tracker (RAM only) */
    bool idle_on;            /* collecting */
    bool wait_peak;          /* after a run: waiting for the room to pass its peak */
    float peak;
    uint8_t idle_sensor;
    bool bucket_on;
    uint32_t bucket_ms;      /* the current minute started here */
    float bucket_sum;
    int bucket_n;
    int npts;                /* oldest first */
    uint32_t pt_ms[AT_WARM_IDLE_PTS];
    float pt_v[AT_WARM_IDLE_PTS];

    /* the run in progress: the idle loss measured just before it */
    bool run_loss_ok;
    float run_loss;
    uint8_t run_sensor;

    /* the warm-by run being followed */
    bool armed;
    bool run_seen;           /* the heater started after the firing */
    uint8_t hint;            /* at_warm_res_t: PENDING while it may still be counted */
    at_warm_rec_t cur;
} at_warm_t;

void at_warm_init(at_warm_t *w);
/* Put a saved store back. False, and nothing changes, if it does not make sense. */
bool at_warm_restore(at_warm_t *w, const at_warm_store_t *st);
/* Forget everything learned (Reset learning). The idle tracker carries on. */
void at_warm_reset(at_warm_t *w);

/* Every usable (filtered) room reading. running: the heater is not idle. */
void at_warm_sample(at_warm_t *w, uint32_t now_ms, float temp, uint8_t sensor, bool running);
/* The idle cooling slope (C lost per minute, never below 0) on `sensor` over at least min_span minutes of idle
 * readings. False if there are not enough. */
bool at_warm_idle_loss(const at_warm_t *w, uint8_t sensor, int min_span, float *loss);
/* A run started: the idle loss before it is kept for its gross rate, and the tracker stops. */
void at_warm_run_start(at_warm_t *w);
/* The run ended: r is its entry in the run log, rate_ok whether its fitted rise rate can be used. */
void at_warm_run_end(at_warm_t *w, const at_learn_run_t *r, bool rate_ok);

/* The lead for a warm-by timer starting now + lead. */
void at_warm_predict(const at_warm_t *w, const at_warm_in_t *in, at_warm_pred_t *out);
/* The median lead of the last warm-by runs, minutes; false if none yet. */
bool at_warm_typical_lead(const at_warm_t *w, uint32_t *min);

/* A warm-by timer armed the thermostat. result_hint: AT_WARM_RES_PENDING if the start was made when the prediction
 * asked for it (it may still be counted), else why it will not be. lead_s is the lead used (0 at the start time). */
void at_warm_fired(at_warm_t *w, int64_t fired_epoch, int64_t start_epoch, const at_warm_pred_t *p, float top,
                   bool have_temp, float temp, at_warm_res_t result_hint, uint32_t lead_s);
/* Every tick while a warm-by run is followed: the room, Hearth's stop point, and whether the heater is running.
 * epoch 0 (no clock): nothing is decided. */
void at_warm_watch(at_warm_t *w, int64_t epoch, bool have_temp, float temp, float stop_at, bool running);
/* The heater went idle; end is how the run ended. */
void at_warm_run_ended(at_warm_t *w, int64_t epoch, at_run_end_t end);
/* The timer that armed it ended, or Auto was switched off by someone: the follow ends (recorded as it stands). */
void at_warm_abandon(at_warm_t *w, int64_t epoch);

/* Median of a float list (n > 0). */
float at_warm_median(const float *v, int n);

const char *at_warm_est_name(at_warm_est_t e);   /* NULL for none */
const char *at_warm_res_name(at_warm_res_t r);

#ifdef __cplusplus
}
#endif
