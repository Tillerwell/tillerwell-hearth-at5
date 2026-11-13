/*
 * Early start inside the range (firmware 1.60, docs/EARLY_START.md): where Auto's next thermostat start happens.
 *
 * Before 1.60 every thermostat start waited for the room to fall to the bottom of the working range. In a space that
 * heats slowly that swings the room across the whole range for nothing: the 30 minute minimum run would be served
 * long before the room reached the top even from much higher up. Now, on the external room sensor, a run may start
 * as high as the top less AT_EARLY_MIN_BAND, as long as Hearth is sure it will still last its 30 minutes:
 *
 *   climb_needed = net_rate x max(0, 30 min - warm-up) x AT_EARLY_MARGIN
 *   start_at     = clamp(stop_at - climb_needed, lo, min(hi - AT_EARLY_MIN_BAND - busy_w, stop_at - 0.1))
 *
 * net_rate is warm by's live model (at_warm): the gross heating rate less the room's current idle cooling. With no
 * current cooling figure the gross rate alone is used (a faster climb than the real one, so an earlier, lower start:
 * the safe side). With no gross rate either, the start is halfway: the top less AT_EARLY_HALFWAY. On the controller's
 * whole-degree sensor the start stays at the bottom of the working range, as before.
 *
 * The busy rule: an Auto start that makes AT_EARLY_BUSY_N Auto starts inside AT_EARLY_BUSY_MS widens the band by
 * AT_EARLY_WIDEN (busy_w, up to the whole working range less AT_EARLY_MIN_BAND, which is the bottom again); an Auto
 * start more than AT_EARLY_BUSY_MS after the previous one narrows it by AT_EARLY_CALM, down to 0. busy_w is kept in
 * its own small NVS blob ("ctl_early") and in the backup; the start times are RAM only.
 *
 * The working range itself (lo, hi), the stop point and the width learning are not changed by any of this.
 *
 * Pure C over plain data, fed by at_ctl, so host_tests/ covers it. Times are uptime ms.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_EARLY_VERSION     1
#define AT_EARLY_MIN_BAND    0.3f                     /* the earliest start: the top less this */
#define AT_EARLY_HALFWAY     0.5f                     /* nothing learned: the top less this */
#define AT_EARLY_MARGIN      1.2f                     /* on the climb, so a faster day still lasts its 30 minutes */
#define AT_EARLY_RUN_MIN     30                       /* the minimum run, minutes from flame established */
#define AT_EARLY_BELOW_STOP  0.1f                     /* a start is always at least this far below the stop point */
#define AT_EARLY_BUSY_N      3                        /* this many Auto starts... */
#define AT_EARLY_BUSY_MS     (2u * 3600u * 1000u)     /* ...inside this long: busy */
#define AT_EARLY_WIDEN       0.2f                     /* busy: the band widens by this */
#define AT_EARLY_CALM        0.1f                     /* calm: the band narrows by this */
#define AT_EARLY_MAX_BUSY    4.0f                     /* the most busy_w is ever kept at (the widest working range) */

typedef enum {
    AT_EARLY_EST_OFF = 0,    /* on the controller's sensor: the start is the bottom of the working range */
    AT_EARLY_EST_MODEL,      /* gross heating rate less the current cooling */
    AT_EARLY_EST_GROSS,      /* gross heating rate, no current cooling figure yet */
    AT_EARLY_EST_HALFWAY,    /* nothing learned: the top less AT_EARLY_HALFWAY */
} at_early_est_t;

/* Kept across a restart (one small NVS blob) and in the backup. */
typedef struct {
    uint8_t version;
    uint8_t pad;
    int16_t busy_cc;         /* busy_w in hundredths of a degree C */
} at_early_store_t;

typedef struct {
    at_early_store_t st;
    bool dirty;              /* st changed: the caller saves it */
    int nstarts;             /* the last Auto starts, oldest first (RAM only) */
    uint32_t start_ms[AT_EARLY_BUSY_N];
} at_early_t;

/* What the start point is worked out from. */
typedef struct {
    bool ext;                /* the external room sensor is in use */
    float lo, hi;            /* the working range */
    float stop_at;           /* where Hearth stops (hi less the coast lead) */
    bool have_gross;
    float gross;             /* the gross heating rate on the external sensor, C per minute */
    bool have_loss;
    float loss;              /* the room's current idle cooling, C per minute */
    bool have_warm;
    uint32_t warm_s;         /* the typical warm-up delay (flame to the room rising) */
} at_early_in_t;

typedef struct {
    float start_at;          /* Auto starts at or below this reading */
    uint8_t est;             /* at_early_est_t */
    bool have_rate;
    float net_rate;          /* the rate used, C per minute (model and gross only) */
    float climb;             /* the climb a run must have left to last its 30 minutes, C (model and gross only) */
    float busy_w;
} at_early_out_t;

void at_early_init(at_early_t *e);
/* Put a saved store back. False, and nothing changes, if it does not make sense. */
bool at_early_restore(at_early_t *e, const at_early_store_t *st);
/* Forget the busy widening and the start times (Reset learning). */
void at_early_reset(at_early_t *e);
/* The busy widening now, C. */
float at_early_busy(const at_early_t *e);
/* An Auto thermostat start (not manual, frost, a timer's fixed run or warm by's start inside the range) was sent.
 * work_width is the working range's width now (busy_w never goes past it less AT_EARLY_MIN_BAND). */
void at_early_auto_start(at_early_t *e, uint32_t now_ms, float work_width);
/* Where Auto's next start happens. */
void at_early_point(const at_early_t *e, const at_early_in_t *in, at_early_out_t *out);

const char *at_early_est_name(at_early_est_t est);

#ifdef __cplusplus
}
#endif
