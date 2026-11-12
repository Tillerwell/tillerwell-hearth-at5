/*
 * Software thermostat ("Auto") and frost protection ("Prevent freezing"), with Heater Care
 * (docs/ADAPTIVE_THERMOSTAT.md): run locks, the ceiling and the learned range width. Pure tick functions over an
 * at_snapshot_t so they can be tested on a host.
 *
 * Auto:    keep the room in the working range [work_low, auto_high]: start at or below work_low, run until the
 *          room reaches auto_high, and never stop inside the range. The working range is the user's range widened
 *          by the learned width (at_learn), at least 2 C on the controller's whole-degree sensor.
 * Freeze:  start whenever the temperature reaches the floor, regardless of Auto or a previous Stop, even inside
 *          the minimum off; only stops runs it started itself.
 * Stops:   a stop Hearth or someone sent is sent again every 90 s while the heater still shows starting or
 *          burning. A start asked for after a stop carries the run on (one run, locks and the top stop intact).
 * Locks:   Hearth's own stops (top of the range, a timer ending, the thermostat turned off with "let it finish",
 *          every reading lost) wait until the run has burned 30 minutes from flame established (20 for frost
 *          protection). A fault, the ceiling (top + 5 C) and a stop someone asked for always go at once. A
 *          thermostat start waits until the heater has been idle 20 minutes.
 * A fault never disables either loop: start attempts back off 5, 15 and 20 minutes, then stop until the fault
 * clears.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "at_core.h"
#include "at_learn.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AT_CTL_HYSTERESIS        1.0f
#define AT_CTL_MIN_ACTION_MS     90000u
#define AT_CTL_MAX_READING_AGE_MS 10000u
#define AT_CTL_EXT_STALE_MS      90000u
#define AT_CTL_NOTE_LEN          96

#define AT_CTL_MIN_WIDTH         1.0f                 /* narrowest user range, either sensor */
#define AT_CTL_MIN_RUN_MS        (30u * 60000u)       /* thermostat and manual warnings, from flame established */
#define AT_CTL_FROST_RUN_MS      (20u * 60000u)       /* frost protection */
#define AT_CTL_MIN_OFF_MS        (20u * 60000u)       /* from idle to the next thermostat start */
#define AT_CTL_CEILING_OVER      5.0f
#define AT_CTL_STUCK_MS          (15u * 60000u)       /* external reading unchanged at least this long while burning */
#define AT_CTL_STUCK_CTL_RISE    2                    /* ...while the controller's reading rose this many whole degrees */
#define AT_CTL_START_GRACE_MS    120000u              /* a start we sent shows as the heater leaving idle within this */
#define AT_CTL_EARLY_LOG         8

/* Who asked for a stop. */
typedef enum {
    AT_SRC_UNKNOWN = 0,
    AT_SRC_APP,
    AT_SRC_WEB,
    AT_SRC_MATTER,
    AT_SRC_MQTT,
    AT_SRC_HA,
    AT_SRC_CONTROLLER,
    AT_SRC_CONSOLE,
    AT_SRC_TIMER,
} at_src_t;

/* Why a stop Hearth wants to send is waiting for the run lock. */
typedef enum {
    AT_HOLD_NONE = 0,
    AT_HOLD_TOP,
    AT_HOLD_TIMER_ENDED,
    AT_HOLD_THERMO_OFF,
    AT_HOLD_NO_READING,
} at_hold_t;

typedef enum { AT_OWNER_OTHER = 0, AT_OWNER_AUTO, AT_OWNER_FREEZE } at_owner_t;

typedef struct {
    void (*start_thermostat)(void *ctx);
    void (*stop)(void *ctx);
    void *ctx;
} at_ctl_ops_t;

typedef struct {
    bool enabled;
    float target;               /* Auto: midpoint of the user's range */
    float low, high;            /* Auto only: the user's range */
    bool has_acted;
    uint32_t last_action_ms;
    bool stop_pending;
    bool started_by_me;         /* Freeze only */
    int retry_count;
    bool retry_armed;
    uint32_t next_retry_ms;
    bool exhausted_logged;
    char note[AT_CTL_NOTE_LEN];
} at_ctl_loop_t;

typedef struct {
    uint32_t ms;                /* when it was asked for (uptime) */
    uint32_t after_s;           /* how long the run had gone, from switch-on */
    uint8_t src;                /* at_src_t */
} at_ctl_early_t;

/* What at_app saves in NVS so the minimum off survives a restart. */
typedef struct {
    bool run_mark;              /* a run started and its end was never seen */
    bool have_stop;             /* a run's end was recorded */
    int64_t stop_epoch;         /* wall clock of the last run's end (idle); 0: stopped, time unknown (no clock) */
} at_ctl_saved_t;

typedef struct {
    bool running;               /* the heater has left idle (state_raw != 0) */
    uint32_t on_ms;
    bool flame;                 /* burning reported in this run */
    uint32_t flame_ms;
    uint8_t owner;              /* at_owner_t */
    bool stopping;              /* a stop was sent, or the heater is shutting down */
    uint32_t stop_ms;
    bool hearth_stop;           /* Hearth sent its own stop */
    bool resend;                /* a stop Hearth or someone sent: send it again while the heater still shows 0x02/0x03 */
    bool carry;                 /* a start was asked for after the stop: the run carries on once it shows 0x02/0x03 */
    uint32_t resend_ms;         /* the last time it was sent */
    uint32_t stop_seq;          /* the controller's control count when it was sent */
    uint8_t end;                /* at_run_end_t decided so far */
    bool user_stop;
    uint8_t user_src;
    uint32_t panel_seq;         /* the controller's control count at the run's start */
    bool off_known;
    uint32_t off_ms;            /* how long it had been idle before this run */
} at_ctl_run_t;

typedef struct {
    at_ctl_loop_t autot;
    at_ctl_loop_t freeze;
    bool use_external;          /* prefer the pushed external temperature when fresh */
    bool have_external;
    float external_temp;
    uint32_t external_ms;
    uint32_t ext_change_ms;     /* the external reading last changed value */

    /* stuck external sensor: flat while the controller's reading climbs */
    bool ext_stuck;
    bool stuck_ref_ok;          /* a flat period is being watched */
    uint32_t stuck_ref_ms;      /* it started here (last change, or flame on) */
    int stuck_ref_cabin;        /* the controller's reading when it started */

    /* room reading filter: middle of the last 3 from the sensor in use */
    bool filt_have;
    uint8_t filt_sensor;        /* at_sensor_t */
    uint32_t filt_ext_ms;
    int filt_n;
    float filt[3];
    bool room_ok;
    float room;                 /* filtered */
    uint8_t sensor;             /* sensor in use (last known) */

    /* runs and locks */
    bool seen_status;
    uint32_t panel_seq_seen;    /* the controller's control count on the last status (for stops sent between ticks) */
    at_ctl_run_t run;
    bool idle_known;            /* idle_since_ms is real (not boot) */
    uint32_t idle_since_ms;
    bool off_lock;
    uint32_t off_until_ms;
    uint8_t held;               /* at_hold_t */
    float held_high;            /* AT_HOLD_TIMER_ENDED / AT_HOLD_THERMO_OFF: the top of the range the run was for */
    uint8_t last_end;           /* at_run_end_t of the last run */
    float last_ceiling;         /* the ceiling the last ceiling stop fired at (frost's or Auto's), 0 if none */
    uint8_t pending_owner;      /* a start we sent, waiting to show */
    uint32_t pending_ms;

    /* rest across a restart */
    uint32_t boot_ms;
    bool boot_pending;          /* restore decided on the first status seen */
    at_ctl_saved_t saved;
    bool clock_fix_pending;     /* clock was not set at boot: switch to the saved stop time once it is */
    int64_t build_epoch;
    bool clock_ok;
    int64_t epoch;
    /* NVS writes asked of at_app (it clears them after saving) */
    bool save_run_mark;         /* true: set the mark */
    bool save_stop;             /* write have_stop = 1 with stop_epoch, and clear the mark */
    int64_t save_stop_epoch;    /* 0: the clock was not set, the stop time is unknown */

    /* early stops (stops someone asked for before the minimum run was up) */
    at_ctl_early_t early[AT_CTL_EARLY_LOG];
    int n_early;
    uint16_t early_unseen;

    at_learn_t learn;
} at_ctl_t;

/* Thermostat status for the apps (the "thermo" object). */
typedef struct {
    float work_low, work_high, learned_width, min_width, ceiling;
    uint8_t sensor;             /* at_sensor_t */
    uint8_t at_limit;           /* at_limit_t on the sensor in use */
    const char *reason;         /* "user" | "widened" | "narrowed" | "controller_sensor" */
    uint32_t min_run_left_s, min_off_left_s;
    uint8_t holding;            /* at_hold_t */
    bool running;
    uint32_t run_on_ago_s;      /* valid while running */
    bool flame;
    uint32_t flame_on_ago_s;
    bool sensor_stuck;
    bool have_rate;
    float rise_rate;
    bool have_warm;
    uint32_t warmup_typical_s;
    bool have_cool;
    uint32_t cool_typical_s;
    float dip_below;
    uint16_t early_stop_unseen;
    uint8_t last_run_end;       /* at_run_end_t */
    bool have_last_early;
    at_ctl_early_t last_early;
    uint32_t last_early_ago_s;
    bool no_reading;            /* no usable temperature at all */
} at_ctl_status_t;

void at_ctl_init(at_ctl_t *t, float auto_target, float freeze_target);

/* At boot, after init: what was saved, and the firmware's build time (a clock earlier than that is wrong). */
void at_ctl_boot(at_ctl_t *t, const at_ctl_saved_t *saved, int64_t build_epoch, uint32_t now_ms);
/* The wall clock, before each tick (ok false until it is set). */
void at_ctl_set_clock(at_ctl_t *t, bool ok, int64_t epoch);

void at_ctl_set_external(at_ctl_t *t, float temp, uint32_t now_ms);

/* The raw temperature the loops act on: the external reading if enabled, fresh and not stuck, else the
 * controller's own cabin sensor. False if none usable. */
bool at_ctl_effective_temp(const at_ctl_t *t, const at_snapshot_t *s, uint32_t now_ms,
                           float *temp, uint32_t *age_ms, bool *external);

/* Turn Auto on/off. On starts the heater straight away if the room is at or below the bottom of the working
 * range and the minimum off is over; a run already going is taken over. Off stops the heater; inside the minimum
 * run it lets the run finish (at_ctl_auto_off gives the choice). */
void at_ctl_set_auto(at_ctl_t *t, bool enabled, const at_snapshot_t *s, uint32_t now_ms, const at_ctl_ops_t *ops);
/* Auto off. finish: let a run inside its minimum run reach it (hold says why: AT_HOLD_THERMO_OFF or
 * AT_HOLD_TIMER_ENDED); otherwise stop now, logged as an early stop from src if inside the minimum run. */
void at_ctl_auto_off(at_ctl_t *t, bool finish, at_hold_t hold, at_src_t src, const at_snapshot_t *s, uint32_t now_ms,
                     const at_ctl_ops_t *ops);
void at_ctl_disarm_auto(at_ctl_t *t, const char *note);
/* The room temperature Auto's next start happens at: the bottom of the working range. */
float at_ctl_auto_start_at(const at_ctl_t *t);
void at_ctl_set_auto_target(at_ctl_t *t, float target);
void at_ctl_set_auto_range(at_ctl_t *t, float low, float high);
void at_ctl_set_freeze(at_ctl_t *t, bool enabled);
void at_ctl_set_freeze_target(at_ctl_t *t, float target);

/* A stop someone asked for (the caller sends it): logged as an early stop if inside the minimum run, and sent
 * again every 90 s while the heater still shows starting or burning. */
void at_ctl_user_stop(at_ctl_t *t, at_src_t src, uint32_t now_ms);
/* Someone asked for a start (the caller sends it): a run flagged as stopping is no longer, Hearth stops sending
 * the stop again, and if the heater relights from its purge it is the same run carrying on (locks, stop at the
 * top). */
void at_ctl_user_start(at_ctl_t *t, uint32_t now_ms);
/* The apps showed the short-cycle explainer. */
void at_ctl_early_seen(at_ctl_t *t);
/* Would a stop now be inside the minimum run? (the "Stop anyway" question) */
uint32_t at_ctl_min_run_left_ms(const at_ctl_t *t, uint32_t now_ms);
uint32_t at_ctl_min_off_left_ms(const at_ctl_t *t, uint32_t now_ms);

/* The working range on the sensor in use. */
void at_ctl_working_range(const at_ctl_t *t, float *low, float *high);

void at_ctl_status(const at_ctl_t *t, uint32_t now_ms, at_ctl_status_t *out);

/* Call every ~3 s. */
void at_ctl_tick(at_ctl_t *t, const at_snapshot_t *s, uint32_t now_ms, const at_ctl_ops_t *ops);

const char *at_ctl_src_name(at_src_t s);
at_src_t at_ctl_src_parse(const char *name);  /* unknown names: AT_SRC_UNKNOWN */
const char *at_ctl_hold_name(at_hold_t h);    /* NULL for none */

#ifdef __cplusplus
}
#endif
