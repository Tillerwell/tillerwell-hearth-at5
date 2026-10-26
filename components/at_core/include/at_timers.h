/*
 * Heater timers: the rules a schedule must obey, the "is anything due" decision and the run it starts.
 * Pure C over plain data (no ESP-IDF, no libc time zone), so host_tests/ covers it with a fake clock.
 * The firmware layer (at_app/at_app_timers.c) supplies local time, storage and the real heater commands.
 *
 * Mirrors TimerRules in the iOS app (Hearth/Sources/Hearth/HeaterTimer.swift); the box does not trust the
 * app, so every rule is enforced here again. Contract: docs/TIMERS.md.
 *
 * All times are the box's LOCAL time. A "local minute" is days since 1970-01-01 (local calendar) * 1440
 * plus the minute of the day, so a timer's start is simply compared with it and daylight-saving changes
 * need no special cases beyond the ones documented at at_sched_poll.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "at_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AT_TIMERS_MAX        32
#define AT_TIMER_ID_LEN      37          /* UUID text + NUL */
#define AT_TIMER_MIN_DUR     20
#define AT_TIMER_MAX_DUR     480
#define AT_TIMER_GAP_MIN     30          /* a timer may not start sooner than this after another ended */

typedef enum { AT_TMODE_PREHEAT = 0, AT_TMODE_THERMOSTAT = 1 } at_timer_mode_t;

typedef struct {
    char id[AT_TIMER_ID_LEN];
    bool enabled;
    uint8_t mode;               /* at_timer_mode_t */
    uint16_t start_minute;      /* minutes after local midnight, 0..1439 */
    uint16_t duration_min;      /* 20..480 */
    uint8_t days;               /* repeating: bit 0 = Monday ... bit 6 = Sunday */
    bool one_off;
    int32_t date_day;           /* one-off: local date as days since 1970-01-01 */
    int8_t low_c, high_c;       /* thermostat only */
} at_timer_t;

/* The box's local time at one instant. ok is false until the clock and time zone can be trusted. */
typedef struct {
    bool ok;
    int64_t epoch_s;
    int32_t day;                /* local date, days since 1970-01-01 */
    uint16_t minute;            /* local minute of the day */
} at_localtime_t;

static inline int64_t at_local_minutes(const at_localtime_t *t) { return (int64_t)t->day * 1440 + t->minute; }

/* ---- calendar ------------------------------------------------------------------------------------- */

int32_t at_days_from_civil(int y, int m, int d);
void at_civil_from_days(int32_t days, int *y, int *m, int *d);
/* 1 = Monday ... 7 = Sunday. */
int at_iso_weekday(int32_t days);
/* Strict "yyyy-MM-dd" of a real calendar date. */
bool at_parse_date(const char *s, int32_t *days);
void at_format_date(int32_t days, char out[11]);

/* ---- validation ----------------------------------------------------------------------------------- */

typedef enum {
    AT_TERR_NONE = 0,
    AT_TERR_TOO_MANY,
    AT_TERR_BAD_ID,
    AT_TERR_DUPLICATE_ID,
    AT_TERR_BAD_MODE,
    AT_TERR_BAD_START,
    AT_TERR_BAD_DURATION,
    AT_TERR_NO_DAYS,
    AT_TERR_BAD_DATE,
    AT_TERR_BAD_RANGE,
    AT_TERR_IN_PAST,
    AT_TERR_TOO_CLOSE,
} at_timer_err_t;

typedef struct {
    at_timer_err_t err;
    int index;                  /* offending timer, or -1 */
    int other;                  /* TOO_CLOSE: the timer it clashes with, else -1 */
} at_timer_problem_t;

const char *at_timer_err_text(at_timer_err_t e);

/* A one-off whose window has already ended at `now`. False when the clock is not ok (nothing can be said). */
bool at_timer_expired(const at_timer_t *t, const at_localtime_t *now);

/* Check a whole replacement list. `existing` is what is stored now: a one-off that has already finished is
 * only accepted if it is unchanged from there (the app may be saving a list it read just before the window
 * ended); it is then ignored, and at_timers_prune_expired drops it. min_gap_c is the thermostat's hysteresis
 * minimum (hyst_min + hyst_max). Disabled timers and finished one-offs are left out of the 30 minute check. */
at_timer_problem_t at_timers_validate(const at_timer_t *list, int n, const at_timer_t *existing, int n_existing,
                                      const at_localtime_t *now, float min_gap_c);

/* Remove finished one-offs (except `keep_id`, the run in progress) in place; returns the new count. */
int at_timers_prune_expired(at_timer_t *list, int n, const at_localtime_t *now, const char *keep_id);

/* ---- firing ---------------------------------------------------------------------------------------- */

#define AT_SCHED_RECENT 8

typedef struct {
    bool have_prev;
    int64_t prev_lm;            /* local minute at the previous poll */
    struct { char id[AT_TIMER_ID_LEN]; int64_t epoch_s; } recent[AT_SCHED_RECENT];
    int recent_next;
} at_sched_t;

/* Which timer (index into list) starts now, or -1. Call about once a second with the current local time.
 *
 * A timer is due when its start minute has passed since the previous poll. Nothing fires on the first poll
 * after the clock became ok (a missed firing is not run late), nor across a clock jump of more than 3
 * hours. Local time moving forward by an hour (spring DST change) fires a timer that fell in the skipped
 * hour at the new time; local time moving back (autumn) re-walks an hour, and a timer that already fired
 * in the last 3 hours is not fired twice. If several are due in one poll only the latest is returned. */
int at_sched_poll(at_sched_t *s, const at_timer_t *list, int n, const at_localtime_t *now);

/* The next enabled start after `now` within the next week: index and its local minute. False if none. */
bool at_sched_next(const at_timer_t *list, int n, const at_localtime_t *now, int *index, int64_t *start_lm);

/* ---- the run a timer starts ------------------------------------------------------------------------ */

/* What the runner needs from the heater side. Each returns false if nothing was started. */
typedef struct {
    bool (*start_preheat)(void *ctx, int minutes);
    bool (*thermostat_begin)(void *ctx, int low_c, int high_c);
    /* disarm=false when the user already switched Auto off by hand: only put things back. */
    void (*thermostat_end)(void *ctx, bool disarm);
    void *ctx;
} at_timer_ops_t;

#define AT_RUN_START_GRACE_MS 120000u   /* a started heater may still report idle for this long */
#define AT_TIMER_NOTE_LEN     64

typedef struct {
    at_sched_t sched;
    bool active;
    char id[AT_TIMER_ID_LEN];
    uint8_t mode;
    uint32_t start_ms;
    uint32_t length_ms;
    char note[AT_TIMER_NOTE_LEN];   /* the last firing and what came of it, for status and the console */
} at_runner_t;

void at_runner_init(at_runner_t *r);

/* Advance the schedule and the run in progress. Call about once a second.
 *
 * Firing needs the heater idle with fresh status and no fault, not in bypass, and no other run going;
 * otherwise that firing is skipped (never queued). For a thermostat timer Auto must also be off, so the
 * user's own Auto always wins. A preheat run ends with its duration, or earlier if the heater went back to
 * idle (stopped by hand, or at the coolant limit); a thermostat run ends with its window, or earlier if
 * the user switched Auto off. */
void at_runner_tick(at_runner_t *r, const at_timer_t *list, int n, const at_localtime_t *now, uint32_t now_ms,
                    const at_snapshot_t *snap, bool auto_enabled, const at_timer_ops_t *ops);

#ifdef __cplusplus
}
#endif
