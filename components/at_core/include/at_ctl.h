/*
 * Software thermostat ("Auto") and frost protection ("Prevent freezing").
 * Port of AutoThermostat / PreventFreezing from the base project, as pure
 * tick functions over an at_snapshot_t so they can be tested on a host.
 *
 * Auto:    keep the cabin in a band [low, high] like the Comfort Controller's thermostat set to
 *          high - 1: run until the cabin reaches high, then stay off until it falls to low.
 *          Before the first time the cabin reaches high, it starts below high - 1 (the set
 *          temperature) instead of waiting for low.  Setting a single target gives the band
 *          target-1 .. target+1.  hyst_min/hyst_max only set how narrow the band may be.
 * Freeze:  start whenever the temperature reaches the floor, regardless of
 *          Auto or a previous Stop; only stops runs it started itself.
 * A fault never disables either loop: start attempts back off 5, 15 and 20
 * minutes, then stop until the fault clears.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "at_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AT_CTL_HYSTERESIS        1.0f
#define AT_CTL_MIN_ACTION_MS     90000u
#define AT_CTL_MAX_READING_AGE_MS 10000u
#define AT_CTL_EXT_STALE_MS      90000u
#define AT_CTL_NOTE_LEN          96

typedef struct {
    void (*start_thermostat)(void *ctx);
    void (*stop)(void *ctx);
    void *ctx;
} at_ctl_ops_t;

typedef struct {
    bool enabled;
    float target;               /* Auto: midpoint of the band */
    float low, high;            /* Auto only */
    float hyst_min, hyst_max;   /* Auto only: the band must be at least both wide (at_app) */
    bool has_acted;
    uint32_t last_action_ms;
    bool stop_pending;          /* Auto only */
    bool reached_high;          /* Auto only: the cabin reached high since arming, so starts wait for low */
    bool started_by_me;         /* Freeze only */
    int retry_count;
    bool retry_armed;
    uint32_t next_retry_ms;
    bool exhausted_logged;
    char note[AT_CTL_NOTE_LEN];
} at_ctl_loop_t;

typedef struct {
    at_ctl_loop_t autot;
    at_ctl_loop_t freeze;
    bool use_external;          /* prefer the pushed external temperature when fresh */
    bool have_external;
    float external_temp;
    uint32_t external_ms;
} at_ctl_t;

void at_ctl_init(at_ctl_t *t, float auto_target, float freeze_target);

void at_ctl_set_external(at_ctl_t *t, float temp, uint32_t now_ms);

/* The temperature the loops act on: the external reading if enabled and
 * fresh, else the controller's own cabin sensor. False if none usable. */
bool at_ctl_effective_temp(const at_ctl_t *t, const at_snapshot_t *s, uint32_t now_ms,
                           float *temp, uint32_t *age_ms, bool *external);

/* Turn Auto on/off. On starts the heater straight away if the cabin is below
 * the set temperature (high - 1), otherwise waits for it; off sends stop. */
void at_ctl_set_auto(at_ctl_t *t, bool enabled, const at_snapshot_t *s, uint32_t now_ms, const at_ctl_ops_t *ops);
void at_ctl_disarm_auto(at_ctl_t *t, const char *note);
/* The cabin temperature Auto's next start happens at: high - 1 before the first run, low after it. */
float at_ctl_auto_start_at(const at_ctl_t *t);
void at_ctl_set_auto_target(at_ctl_t *t, float target);
void at_ctl_set_auto_range(at_ctl_t *t, float low, float high);
void at_ctl_set_hysteresis(at_ctl_t *t, float hyst_min, float hyst_max);
void at_ctl_set_freeze(at_ctl_t *t, bool enabled);
void at_ctl_set_freeze_target(at_ctl_t *t, float target);

/* Call every ~3 s. */
void at_ctl_tick(at_ctl_t *t, const at_snapshot_t *s, uint32_t now_ms, const at_ctl_ops_t *ops);

#ifdef __cplusplus
}
#endif
