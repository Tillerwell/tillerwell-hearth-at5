/*
 * Heater Care's early stop (firmware 1.53): how far the room keeps rising after Hearth's stop at the top, so the
 * next stop can come that much sooner and the peak lands on the set temperature instead of above it.
 *
 * After the heater stops, the heat already in the heater, its cooldown fan and the room itself keep the reading
 * climbing for a while (in one cabin 19.0 at the stop, 19.3 a quarter of an hour later). Each thermostat stop at the
 * top on the room sensor is followed: the highest reading after it, less the reading at the stop, is the rise. It is
 * kept once the room has clearly started to fall (AT_COAST_FALL below the peak). The median of the last AT_COAST_KEEP
 * is the typical rise; from AT_COAST_MIN_LEARN of them Hearth stops early by it, capped at AT_COAST_MAX and at half
 * the working range, so a run still covers most of the range and the 30-minute minimum run still applies.
 *
 * Only on the external room sensor: the Comfort Controller's whole-degree reading cannot show a few tenths, and on
 * it the working range already carries a 1 C allowance (at_learn.h).
 *
 * Pure C over plain data, fed by at_ctl, so host_tests/ covers it. Times are uptime ms.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_COAST_VERSION    1
#define AT_COAST_KEEP       7         /* rises kept, oldest first */
#define AT_COAST_MIN_LEARN  3         /* rises needed before Hearth stops early */
#define AT_COAST_FALL       0.3f      /* the room this far below the peak: the rise is over */
#define AT_COAST_MAX        1.0f      /* never stop more than this early */
#define AT_COAST_MAX_KEPT   3.0f      /* a bigger rise than this is not the heater's (a door, the sun): not kept */
#define AT_COAST_WATCH_MS   (3u * 3600u * 1000u)  /* no fall within this long after the stop: not kept */

/* Kept across a restart (one small NVS blob). */
typedef struct {
    uint8_t version;
    uint8_t n;
    int16_t rise_cc[AT_COAST_KEEP];   /* hundredths of a degree C, oldest first */
} at_coast_store_t;

typedef struct {
    at_coast_store_t st;
    bool dirty;                        /* st changed: the caller saves it */
    bool on;                           /* following the room after a stop at the top */
    uint32_t stop_ms;
    float stop_temp, peak;
} at_coast_t;

void at_coast_init(at_coast_t *c);
/* Put a saved store back. False, and nothing changes, if it does not make sense. */
bool at_coast_restore(at_coast_t *c, const at_coast_store_t *st);
/* Forget every rise (Reset learning). */
void at_coast_reset(at_coast_t *c);
/* Hearth stopped the run at the top, the room reading `temp` on the external sensor (ext false: nothing is followed). */
void at_coast_stop(at_coast_t *c, uint32_t now_ms, float temp, bool ext);
/* Every usable room reading. ext false (the controller's sensor took over) ends the follow without keeping it. */
void at_coast_sample(at_coast_t *c, uint32_t now_ms, float temp, bool ext);
/* A new run started: a rise still being followed is not kept (the heater is adding heat again). */
void at_coast_cancel(at_coast_t *c);
/* The typical rise in C (median of those kept). False until AT_COAST_MIN_LEARN are known. */
bool at_coast_typical(const at_coast_t *c, float *rise);
/* How far below the top Hearth stops, in C (0.1 steps): the typical rise, capped at AT_COAST_MAX and half of
 * work_width, 0 when nothing is learned yet. */
float at_coast_lead(const at_coast_t *c, float work_width);

#ifdef __cplusplus
}
#endif
