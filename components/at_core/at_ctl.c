#include "at_ctl.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "at_proto.h"

static const uint32_t RETRY_DELAYS_MS[] = {5u * 60000u, 15u * 60000u, 20u * 60000u};
#define N_RETRIES ((int)(sizeof(RETRY_DELAYS_MS) / sizeof(RETRY_DELAYS_MS[0])))
#define EPS 1e-3f

static void set_note(at_ctl_loop_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_note(at_ctl_loop_t *l, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->note, sizeof(l->note), fmt, ap);
    va_end(ap);
}

static unsigned mins_up(uint32_t ms) { return (unsigned)((ms + 59999u) / 60000u); }

/* a is at or after b, on a wrapping clock */
static bool after_eq(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

static void reset_retry(at_ctl_loop_t *l)
{
    l->retry_count = 0;
    l->retry_armed = false;
    l->exhausted_logged = false;
}

void at_ctl_init(at_ctl_t *t, float auto_target, float freeze_target)
{
    memset(t, 0, sizeof(*t));
    t->autot.target = auto_target;
    t->autot.low = auto_target - 0.5f;
    t->autot.high = auto_target + 0.5f;
    t->freeze.target = freeze_target;
    t->use_external = true;
    t->sensor = AT_SENS_CTL;
    at_learn_init(&t->learn);
}

void at_ctl_boot(at_ctl_t *t, const at_ctl_saved_t *saved, int64_t build_epoch, uint32_t now_ms)
{
    t->saved = *saved;
    t->build_epoch = build_epoch;
    t->boot_ms = now_ms;
    t->boot_pending = true;
}

void at_ctl_set_clock(at_ctl_t *t, bool ok, int64_t epoch)
{
    t->clock_ok = ok;
    t->epoch = ok ? epoch : 0;
    t->learn.epoch = t->epoch;
}

void at_ctl_set_external(at_ctl_t *t, float temp, uint32_t now_ms)
{
    if (!t->have_external || temp != t->external_temp) {
        t->ext_change_ms = now_ms;
        if (t->ext_stuck && t->have_external) {
            /* a new value is proof it is alive: trust it again at once */
            t->ext_stuck = false;
            set_note(&t->autot, "room sensor moving again -> using it");
        }
    }
    t->have_external = true;
    t->external_temp = temp;
    t->external_ms = now_ms;
}

static bool ext_fresh(const at_ctl_t *t, uint32_t now)
{
    return t->use_external && t->have_external && now - t->external_ms <= AT_CTL_EXT_STALE_MS;
}

bool at_ctl_effective_temp(const at_ctl_t *t, const at_snapshot_t *s, uint32_t now_ms,
                           float *temp, uint32_t *age_ms, bool *external)
{
    if (ext_fresh(t, now_ms) && !t->ext_stuck) {
        *temp = t->external_temp;
        *age_ms = now_ms - t->external_ms;
        *external = true;
        return true;
    }
    *external = false;
    if (s->have_cabin) {
        *temp = (float)s->cabin_temp;
        *age_ms = s->cabin_age_ms;
        return true;
    }
    return false;
}

/* ---- the room reading ------------------------------------------------------------------------------------- */

static float median3(const float *v, int n)
{
    if (n == 1) return v[0];
    if (n == 2) return (v[0] + v[1]) / 2;
    float a = v[0], b = v[1], c = v[2];
    if (a > b) { float x = a; a = b; b = x; }
    if (b > c) { float x = b; b = c; c = x; }
    if (a > b) { float x = a; a = b; b = x; }
    return b;
}

static bool burning(const at_ctl_t *t) { return t->run.running && t->run.flame && !t->run.stopping; }

/* Stuck check, sensor choice and the middle-of-3 filter. Sets room_ok/room/sensor. */
static void update_room(at_ctl_t *t, const at_snapshot_t *s, uint32_t now)
{
    bool fresh = ext_fresh(t, now);
    /* Stuck: the external reading has not moved for 15 min of burning AND the controller's own reading has
     * climbed 2 C meanwhile. A room that is simply holding steady (a slow heat-up, a big space) is not a fault:
     * a flat reading alone is never enough. */
    bool cabin_ok = s->have_cabin && s->cabin_age_ms <= AT_CTL_MAX_READING_AGE_MS;
    if (fresh && !t->ext_stuck && burning(t) && cabin_ok) {
        uint32_t since = after_eq(t->ext_change_ms, t->run.flame_ms) ? t->ext_change_ms : t->run.flame_ms;
        if (!t->stuck_ref_ok || t->stuck_ref_ms != since) {
            t->stuck_ref_ok = true;
            t->stuck_ref_ms = since;
            t->stuck_ref_cabin = s->cabin_temp;
        } else if (s->cabin_temp < t->stuck_ref_cabin) {
            t->stuck_ref_cabin = s->cabin_temp;     /* measure the climb from its lowest point */
        }
        if (now - since >= AT_CTL_STUCK_MS && s->cabin_temp - t->stuck_ref_cabin >= AT_CTL_STUCK_CTL_RISE) {
            t->ext_stuck = true;
            t->stuck_ref_ok = false;
            set_note(&t->autot, "room sensor unchanged while the controller's reading rose 2 C -> using the controller's reading");
        }
    } else {
        t->stuck_ref_ok = false;
    }
    bool have = true;
    uint8_t sensor;
    float raw = 0;
    if (fresh && !t->ext_stuck) {
        sensor = AT_SENS_EXT;
        raw = t->external_temp;
    } else if (s->have_cabin && s->cabin_age_ms <= AT_CTL_MAX_READING_AGE_MS) {
        sensor = AT_SENS_CTL;
        raw = (float)s->cabin_temp;
    } else {
        have = false;
        sensor = t->sensor;
    }
    if (!have) {
        t->room_ok = false;
        return;
    }
    if (!t->filt_have || t->filt_sensor != sensor) {
        if (t->filt_have) at_learn_sensor_changed(&t->learn);
        t->filt_have = true;
        t->filt_sensor = sensor;
        t->filt_n = 0;
        t->filt_ext_ms = t->external_ms - 1;
    }
    bool push = sensor == AT_SENS_CTL || t->external_ms != t->filt_ext_ms;
    if (push) {
        if (t->filt_n == 3) {
            t->filt[0] = t->filt[1];
            t->filt[1] = t->filt[2];
            t->filt_n = 2;
        }
        t->filt[t->filt_n++] = raw;
        t->filt_ext_ms = t->external_ms;
    }
    t->sensor = sensor;
    t->room = median3(t->filt, t->filt_n);
    t->room_ok = true;
}

/* ---- ranges ------------------------------------------------------------------------------------------------ */

static float user_width(const at_ctl_t *t) { return t->autot.high - t->autot.low; }

void at_ctl_working_range(const at_ctl_t *t, float *low, float *high)
{
    float w = at_learn_working_width(t->learn.st.width, user_width(t), t->sensor == AT_SENS_CTL);
    *high = t->autot.high;
    *low = t->autot.high - w;
}

float at_ctl_auto_start_at(const at_ctl_t *t)
{
    float lo, hi;
    at_ctl_working_range(t, &lo, &hi);
    return lo;
}

void at_ctl_set_auto_target(at_ctl_t *t, float target)
{
    t->autot.target = target;
    t->autot.low = target - 0.5f;
    t->autot.high = target + 0.5f;
    at_learn_check_limits(&t->learn, user_width(t));
}

void at_ctl_set_auto_range(at_ctl_t *t, float low, float high)
{
    if (high < low) {
        float x = low;
        low = high;
        high = x;
    }
    t->autot.low = low;
    t->autot.high = high;
    t->autot.target = (low + high) / 2;
    at_learn_check_limits(&t->learn, user_width(t));
}

void at_ctl_set_freeze(at_ctl_t *t, bool enabled)
{
    t->freeze.enabled = enabled;
    if (!enabled) {
        t->freeze.started_by_me = false;
    }
}

void at_ctl_set_freeze_target(at_ctl_t *t, float target)
{
    if (target < 0) target = 0;
    if (target > 10) target = 10;
    t->freeze.target = target;
}

/* ---- locks ------------------------------------------------------------------------------------------------- */

static uint32_t run_len(const at_ctl_t *t)
{
    return t->run.owner == AT_OWNER_FREEZE ? AT_CTL_FROST_RUN_MS : AT_CTL_MIN_RUN_MS;
}

uint32_t at_ctl_min_run_left_ms(const at_ctl_t *t, uint32_t now)
{
    if (!t->run.running || (t->run.stopping && (t->run.hearth_stop || t->run.user_stop))) return 0;
    uint32_t len = run_len(t);
    if (!t->run.flame) return len;   /* the whole of light-up: the minimum run has not started counting */
    uint32_t el = now - t->run.flame_ms;
    return el >= len ? 0 : len - el;
}

uint32_t at_ctl_min_off_left_ms(const at_ctl_t *t, uint32_t now)
{
    if (!t->off_lock || t->run.running) return 0;
    return after_eq(now, t->off_until_ms) ? 0 : t->off_until_ms - now;
}

static void record_early(at_ctl_t *t, at_src_t src, uint32_t now)
{
    if (t->n_early == AT_CTL_EARLY_LOG) {
        memmove(&t->early[0], &t->early[1], sizeof(t->early[0]) * (AT_CTL_EARLY_LOG - 1));
        t->n_early--;
    }
    at_ctl_early_t e = {now, (now - t->run.on_ms) / 1000u, (uint8_t)src};
    t->early[t->n_early++] = e;
    if (t->early_unseen < 0xffff) t->early_unseen++;
}

static at_run_end_t end_for_src(at_src_t src)
{
    switch (src) {
    case AT_SRC_MATTER:
    case AT_SRC_MQTT:
    case AT_SRC_HA:
    case AT_SRC_CONTROLLER: return AT_END_EXTERNAL;
    default: return AT_END_USER;
    }
}

static void arm_resend(at_ctl_t *t, const at_snapshot_t *s, uint32_t now);

void at_ctl_user_stop(at_ctl_t *t, at_src_t src, uint32_t now)
{
    if (t->run.running && !t->run.stopping) {
        if (at_ctl_min_run_left_ms(t, now) > 0) record_early(t, src, now);
        t->run.user_stop = true;
        t->run.user_src = (uint8_t)src;
        t->run.end = (uint8_t)end_for_src(src);
        t->run.stopping = true;
        t->run.stop_ms = now;
        t->run.carry = false;
        at_learn_stop(&t->learn, now, false);
        /* sent again like Hearth's own if it is lost on the bus */
        arm_resend(t, NULL, now);
    }
    t->held = AT_HOLD_NONE;
}

void at_ctl_early_seen(at_ctl_t *t) { t->early_unseen = 0; }

/* A stop Hearth or someone sent: watch that the heater acts on it. Without a snapshot (a stop asked for between
 * ticks) the controller's count is the one on the last status. */
static void arm_resend(at_ctl_t *t, const at_snapshot_t *s, uint32_t now)
{
    if (!t->run.running) return;
    t->run.resend = true;
    t->run.resend_ms = now;
    t->run.stop_seq = s ? s->panel_control_seq : t->panel_seq_seen;
}

/* A start was asked for after a stop (someone, the controller, or Auto taking the run over): the run is no longer
 * stopping, the stop is not sent again, and the stops waiting on it are dropped. The end decided so far is kept
 * until the heater shows starting or burning again (track_run), so a heater that purges anyway still ends as it
 * was stopped. */
static void carry_on(at_ctl_t *t, at_owner_t owner)
{
    t->run.resend = false;
    t->run.stopping = false;
    t->run.hearth_stop = false;
    t->run.user_stop = false;
    t->run.carry = true;
    t->run.owner = (uint8_t)owner;
    t->autot.stop_pending = false;
    t->freeze.stop_pending = false;
    t->freeze.started_by_me = false;
    at_learn_resume(&t->learn);
}

static void hearth_stop(at_ctl_t *t, const at_ctl_ops_t *ops, const at_snapshot_t *s, uint32_t now, at_run_end_t end,
                        bool at_top)
{
    ops->stop(ops->ctx);
    t->held = AT_HOLD_NONE;
    if (t->run.running && !t->run.stopping) {
        t->run.stopping = true;
        t->run.stop_ms = now;
        t->run.hearth_stop = true;
        t->run.end = (uint8_t)end;
        at_learn_stop(&t->learn, now, at_top);
    }
    arm_resend(t, s, now);
}

static bool starting_or_burning(const at_snapshot_t *s)
{
    return s->have_status && !s->stale && (s->status.state_raw == 0x02 || s->status.state_raw == 0x03);
}

/* A stop Hearth or someone sent that the heater did not act on (lost on the bus, or ignored): send it again every
 * 90 s while the heater still shows starting or burning, until it purges or goes idle. A start someone asked for
 * since (the controller, or at_ctl_user_start) ends the retries. */
static void retry_stop(at_ctl_t *t, const at_snapshot_t *s, uint32_t now, const at_ctl_ops_t *ops)
{
    if (!t->run.running || !t->run.stopping || !t->run.resend || !s->have_status || s->stale) return;
    if (s->panel_control_seq != t->run.stop_seq && s->panel_last_control_type != AT_TYPE_STOP) {
        /* the controller started it again: its run now, carried on as one run */
        carry_on(t, AT_OWNER_OTHER);
        return;
    }
    if (!starting_or_burning(s) || now - t->run.resend_ms < AT_CTL_MIN_ACTION_MS) return;
    at_ctl_loop_t *l = t->run.owner == AT_OWNER_FREEZE ? &t->freeze : &t->autot;
    set_note(l, "heater still running %us after the stop -> sending stop again",
             (unsigned)((now - t->run.resend_ms) / 1000u));
    ops->stop(ops->ctx);
    t->run.resend_ms = now;
}

void at_ctl_user_start(at_ctl_t *t, uint32_t now)
{
    (void)now;
    if (t->run.running && (t->run.stopping || t->run.resend)) carry_on(t, AT_OWNER_OTHER);
}

/* ---- fault back-off ---------------------------------------------------------------------------------------- */

/* True once enough back-off time has passed to retry a start while faulted. */
static bool retry_ready(at_ctl_loop_t *l, const at_snapshot_t *s, uint32_t now)
{
    const char *fn = s->fault_name ? s->fault_name : "unknown";
    if (l->retry_count >= N_RETRIES) {
        if (!l->exhausted_logged) {
            set_note(l, "fault %u (%s) -> %d retries exhausted, giving up until the fault clears",
                     s->status.fault, fn, N_RETRIES);
            l->exhausted_logged = true;
        }
        return false;
    }
    if (!l->retry_armed) {
        uint32_t d = RETRY_DELAYS_MS[l->retry_count];
        l->next_retry_ms = now + d;
        l->retry_armed = true;
        set_note(l, "fault %u (%s) -> waiting %umin before retry %d/%d",
                 s->status.fault, fn, (unsigned)(d / 60000u), l->retry_count + 1, N_RETRIES);
        return false;
    }
    if ((int32_t)(now - l->next_retry_ms) < 0) {
        return false;
    }
    l->retry_count++;
    l->retry_armed = false;
    return true;
}

static bool is_idle(const at_snapshot_t *s)
{
    return s->have_status && s->status.state_raw == 0x00;
}

static void send_start(at_ctl_t *t, at_ctl_loop_t *l, const at_ctl_ops_t *ops, uint32_t now, at_owner_t owner)
{
    ops->start_thermostat(ops->ctx);
    t->pending_owner = (uint8_t)owner;
    t->pending_ms = now;
    l->last_action_ms = now;
    l->has_acted = true;
}

/* A start we sent that has not shown yet. */
static bool start_pending(const at_ctl_t *t, uint32_t now)
{
    return t->pending_owner != AT_OWNER_OTHER && now - t->pending_ms <= AT_CTL_START_GRACE_MS;
}

/* Auto is on and wants a run now: at or below the bottom of its working range, the minimum off over, and not given
 * up on a fault. Frost protection leaves the start to it (Auto's own 90 s pacing may hold it a moment). */
static bool auto_wants_start(const at_ctl_t *t, const at_snapshot_t *s, uint32_t now)
{
    if (!t->autot.enabled || !t->room_ok || t->boot_pending) return false;
    float lo, hi;
    at_ctl_working_range(t, &lo, &hi);
    if (t->room > lo + EPS || at_ctl_min_off_left_ms(t, now) > 0) return false;
    if (s->have_status && s->status.fault && t->autot.retry_count >= N_RETRIES) return false;
    return true;
}

/* A run frost protection started, wanted by Auto: Auto's from here (30 min lock, Auto's ceiling, stopped at the top
 * of Auto's range, never at the frost target + 1). */
static void take_from_frost(at_ctl_t *t)
{
    t->run.owner = AT_OWNER_AUTO;
    t->freeze.started_by_me = false;
    if (t->held == AT_HOLD_TOP) t->held = AT_HOLD_NONE;
}

/* ---- Auto on / off ----------------------------------------------------------------------------------------- */

void at_ctl_set_auto(at_ctl_t *t, bool enabled, const at_snapshot_t *s, uint32_t now, const at_ctl_ops_t *ops)
{
    if (!enabled) {
        at_ctl_auto_off(t, true, AT_HOLD_THERMO_OFF, AT_SRC_UNKNOWN, s, now, ops);
        return;
    }
    at_ctl_loop_t *l = &t->autot;
    if (l->enabled) return;
    l->enabled = true;
    update_room(t, s, now);
    if (t->held == AT_HOLD_TIMER_ENDED || t->held == AT_HOLD_THERMO_OFF) t->held = AT_HOLD_NONE;
    float lo, hi;
    at_ctl_working_range(t, &lo, &hi);
    uint32_t off_left = at_ctl_min_off_left_ms(t, now);
    if (t->boot_pending) {
        /* just restarted: whether a rest is still owed is decided on the heater's first status */
        set_note(l, "Auto thermostat enabled -> waiting for the heater's status");
    } else if (s->have_status && !is_idle(s)) {
        /* a run already going (a timer's overrun, the controller, a manual start): Auto takes it over */
        set_note(l, "Auto thermostat enabled -> run in progress, taking it over");
        if (t->run.running && t->run.stopping && (t->run.hearth_stop || t->run.user_stop)) {
            /* a stop is still going out (lost, or the heater purging): Auto's run from here, stopped at the top */
            carry_on(t, AT_OWNER_AUTO);
        } else if (t->run.running && t->run.owner == AT_OWNER_FREEZE) {
            take_from_frost(t);
        }
    } else if (!t->room_ok) {
        /* never start blind: with no reading nothing could stop the run */
        set_note(l, "Auto thermostat enabled -> no temperature reading yet, waiting");
    } else if (t->room > lo) {
        set_note(l, "Auto thermostat enabled -> temperature %.1f above %.1f, waiting", t->room, lo);
    } else if (off_left > 0) {
        set_note(l, "Auto thermostat enabled -> at %.1f, waiting: minimum off %u min left", t->room, mins_up(off_left));
    } else {
        set_note(l, "Auto thermostat enabled -> sending start thermostat");
        send_start(t, l, ops, now, AT_OWNER_AUTO);
    }
    reset_retry(l);
    l->has_acted = true;
    l->last_action_ms = now;
}

void at_ctl_auto_off(at_ctl_t *t, bool finish, at_hold_t hold, at_src_t src, const at_snapshot_t *s, uint32_t now,
                     const at_ctl_ops_t *ops)
{
    at_ctl_loop_t *l = &t->autot;
    if (!l->enabled) return;
    l->enabled = false;
    l->stop_pending = false;
    uint32_t left = at_ctl_min_run_left_ms(t, now);
    bool in_run = t->run.running && !t->run.stopping;
    bool fault = s->have_status && s->status.fault;
    if (finish && in_run && left > 0 && !fault) {
        t->held = (uint8_t)hold;
        t->held_high = t->autot.high;   /* a timer's range is put back straight after: the ceiling stays the timer's */
        set_note(l, "%s -> finishing minimum run: %u min left",
                 hold == AT_HOLD_TIMER_ENDED ? "timer ended" : "Auto thermostat disabled", mins_up(left));
    } else if (in_run && left > 0 && !fault) {
        set_note(l, "Auto thermostat disabled -> stopping now (inside the minimum run)");
        at_ctl_user_stop(t, src, now);
        ops->stop(ops->ctx);
        arm_resend(t, s, now);
    } else {
        set_note(l, "Auto thermostat disabled -> sending stop");
        hearth_stop(t, ops, s, now, hold == AT_HOLD_TIMER_ENDED ? AT_END_TIMER : AT_END_THERMO_OFF, false);
    }
    l->has_acted = true;
    l->last_action_ms = now;
}

void at_ctl_disarm_auto(at_ctl_t *t, const char *note)
{
    t->autot.enabled = false;
    t->autot.stop_pending = false;
    t->autot.has_acted = false;
    if (t->held == AT_HOLD_TOP || t->held == AT_HOLD_NO_READING) t->held = AT_HOLD_NONE;
    reset_retry(&t->autot);
    set_note(&t->autot, "%s", note ? note : "physical controller took control");
}

/* ---- runs -------------------------------------------------------------------------------------------------- */

static void off_lock_from(at_ctl_t *t, uint32_t start_ms, uint32_t now)
{
    t->off_lock = true;
    t->off_until_ms = start_ms + AT_CTL_MIN_OFF_MS;
    if (after_eq(now, t->off_until_ms)) t->off_lock = false;
}

/* The saved stop time, if it can be trusted now: seconds since it. */
static bool saved_rest(const at_ctl_t *t, int64_t *since_s)
{
    /* stop_epoch 0: stopped while the clock was not set, so when is not known */
    if (!t->saved.have_stop || t->saved.stop_epoch <= 0 || !t->clock_ok || t->epoch < t->build_epoch || t->saved.stop_epoch > t->epoch) return false;
    *since_s = t->epoch - t->saved.stop_epoch;
    return true;
}

static void apply_saved_rest(at_ctl_t *t, uint32_t now)
{
    int64_t since;
    if (!saved_rest(t, &since)) return;
    int64_t left_ms = (int64_t)AT_CTL_MIN_OFF_MS - since * 1000;
    t->off_lock = left_ms > 0;
    t->off_until_ms = now + (uint32_t)(left_ms > 0 ? left_ms : 0);
    if (since < 24 * 3600) {
        t->idle_known = true;
        t->idle_since_ms = now - (uint32_t)(since * 1000);
    }
}

static void boot_decide(at_ctl_t *t, const at_snapshot_t *s, uint32_t now)
{
    t->boot_pending = false;
    if (!is_idle(s)) {
        /* already running at boot: start time unknown, so the run lock counts from boot */
        t->run = (at_ctl_run_t){0};
        t->run.running = true;
        t->run.on_ms = t->boot_ms;
        t->run.owner = t->autot.enabled ? AT_OWNER_AUTO : AT_OWNER_OTHER;
        t->run.panel_seq = s->panel_control_seq;
        if (s->status.state_raw == 0x03 || s->status.burner_active) {
            t->run.flame = true;
            t->run.flame_ms = t->boot_ms;
        }
        at_learn_run_start(&t->learn, now, (at_sensor_t)t->sensor, false, false, 0);
        if (t->run.flame) at_learn_flame(&t->learn, now);
        return;
    }
    if (t->saved.run_mark) {
        /* a run ended without Hearth seeing it (power cut): rest 20 min from boot */
        off_lock_from(t, t->boot_ms, now);
        t->save_stop = true;
        t->save_stop_epoch = 0;
        return;
    }
    if (!t->saved.have_stop) return;   /* never run: no rest owed */
    int64_t since;
    if (saved_rest(t, &since)) {
        apply_saved_rest(t, now);
    } else {
        /* a stop was recorded but its time cannot be trusted: rest 20 min from boot (plan §3.1). A clock arriving
         * later can only shorten it when the stop time itself is known. */
        off_lock_from(t, t->boot_ms, now);
        t->clock_fix_pending = !t->clock_ok && t->saved.stop_epoch > 0;
    }
}

static void run_begin(at_ctl_t *t, const at_snapshot_t *s, uint32_t now)
{
    t->run = (at_ctl_run_t){0};
    t->run.running = true;
    t->run.on_ms = now;
    t->run.owner = (t->pending_owner != AT_OWNER_OTHER && now - t->pending_ms <= AT_CTL_START_GRACE_MS)
                       ? t->pending_owner : AT_OWNER_OTHER;
    t->pending_owner = AT_OWNER_OTHER;
    t->run.panel_seq = s->panel_control_seq;
    t->run.off_known = t->idle_known;
    t->run.off_ms = now - t->idle_since_ms;
    t->off_lock = false;
    t->clock_fix_pending = false;
    t->save_run_mark = true;
    at_learn_run_start(&t->learn, now, (at_sensor_t)t->sensor, t->run.owner == AT_OWNER_AUTO, t->run.off_known,
                       t->run.off_ms);
    if (t->run.owner == AT_OWNER_FREEZE && t->autot.enabled) take_from_frost(t);
}

static void run_end(at_ctl_t *t, const at_snapshot_t *s, uint32_t now)
{
    at_run_end_t end = (at_run_end_t)t->run.end;
    if (end == AT_END_NONE) {
        uint32_t stop_at = t->run.stopping ? t->run.stop_ms : now;
        if (s->status.fault) {
            end = AT_END_FAULT;
        } else if (s->panel_control_seq != t->run.panel_seq && s->panel_last_control_type == AT_TYPE_STOP) {
            end = AT_END_EXTERNAL;
            /* the Comfort Controller stopped it: an early stop if the minimum run was not up */
            bool early = !t->run.flame || stop_at - t->run.flame_ms < run_len(t);
            if (early) {
                record_early(t, AT_SRC_CONTROLLER, stop_at);
            }
        } else {
            end = AT_END_HEATER;
        }
    }
    at_learn_run_end(&t->learn, now, end, user_width(t));
    t->last_end = (uint8_t)end;
    t->run.running = false;
    t->idle_known = true;
    t->idle_since_ms = now;
    off_lock_from(t, now, now);
    t->held = AT_HOLD_NONE;
    t->autot.stop_pending = false;
    t->freeze.started_by_me = false;
    t->save_stop = true;
    t->save_stop_epoch = t->clock_ok ? t->epoch : 0;
}

static void track_run(at_ctl_t *t, const at_snapshot_t *s, uint32_t now)
{
    if (!s->have_status || s->stale) return;
    t->seen_status = true;
    t->panel_seq_seen = s->panel_control_seq;
    if (t->boot_pending) {
        boot_decide(t, s, now);
        return;
    }
    if (t->clock_fix_pending && t->clock_ok && t->off_lock && !t->run.running) {
        /* the clock arrived during a rest counted from boot: the saved stop time is the true rest */
        t->clock_fix_pending = false;
        apply_saved_rest(t, now);
    }
    uint8_t st = s->status.state_raw;
    bool idle = st == 0x00;
    if (!t->run.running && !idle) run_begin(t, s, now);
    if (!t->run.running) {
        if (t->off_lock && after_eq(now, t->off_until_ms)) t->off_lock = false;
        return;
    }
    if (!t->run.flame && (st == 0x03 || s->status.burner_active)) {
        t->run.flame = true;
        t->run.flame_ms = now;
        at_learn_flame(&t->learn, now);
    }
    if (!t->run.stopping && (st == 0x04 || st == 0x05)) {
        t->run.stopping = true;
        t->run.stop_ms = now;
    } else if (t->run.stopping && !t->run.hearth_stop && !t->run.user_stop && (st == 0x02 || st == 0x03)) {
        /* the heater paused its own burner and relit, or relit from a purge on a start asked for after a stop:
         * one run, the locks carry straight through */
        t->run.stopping = false;
    }
    if (t->run.carry && !t->run.stopping && (st == 0x02 || st == 0x03)) {
        /* started again after a stop and showing it: the run's end is decided afresh */
        t->run.carry = false;
        t->run.end = AT_END_NONE;
    }
    if (idle) run_end(t, s, now);
}

/* ---- ceiling and held stops -------------------------------------------------------------------------------- */

static bool auto_run(const at_ctl_t *t)
{
    return t->autot.enabled || t->run.owner == AT_OWNER_AUTO || t->held == AT_HOLD_TIMER_ENDED ||
           t->held == AT_HOLD_THERMO_OFF;
}

static bool ceiling_of(const at_ctl_t *t, float *c, at_ctl_loop_t **who)
{
    if (t->run.owner == AT_OWNER_FREEZE) {
        *c = t->freeze.target + AT_CTL_HYSTERESIS + AT_CTL_CEILING_OVER;
        *who = (at_ctl_loop_t *)&t->freeze;
        return true;
    }
    if (auto_run(t)) {
        bool held = !t->autot.enabled && (t->held == AT_HOLD_TIMER_ENDED || t->held == AT_HOLD_THERMO_OFF);
        *c = (held ? t->held_high : t->autot.high) + AT_CTL_CEILING_OVER;
        *who = (at_ctl_loop_t *)&t->autot;
        return true;
    }
    return false;
}

static void check_ceiling(at_ctl_t *t, const at_snapshot_t *s, uint32_t now, const at_ctl_ops_t *ops)
{
    float c;
    at_ctl_loop_t *who;
    if (!t->run.running || !t->room_ok || !ceiling_of(t, &c, &who)) return;
    if (t->run.stopping) {
        /* a stop was sent but the heater is still starting or burning: the ceiling sends it again at once (once;
         * after that retry_stop carries on every 90 s) */
        if (t->run.end == AT_END_CEILING || !starting_or_burning(s) || t->room < c - EPS) return;
        if (!t->run.hearth_stop && !t->run.user_stop) return;
        set_note(who, "temperature %.1f reached the ceiling %.1f, heater still running -> stop", t->room, c);
        ops->stop(ops->ctx);
        t->run.end = AT_END_CEILING;
        t->last_ceiling = c;
        arm_resend(t, s, now);
        return;
    }
    if (t->room >= c - EPS) {
        set_note(who, "temperature %.1f reached the ceiling %.1f -> stop", t->room, c);
        hearth_stop(t, ops, s, now, AT_END_CEILING, false);
        t->last_ceiling = c;
        who->last_action_ms = now;
        who->has_acted = true;
        who->stop_pending = true;
    }
}

static void service_held(at_ctl_t *t, const at_snapshot_t *s, uint32_t now, const at_ctl_ops_t *ops)
{
    if (t->held == AT_HOLD_NONE) return;
    if (!t->run.running || t->run.stopping) {
        t->held = AT_HOLD_NONE;
        return;
    }
    bool fault = s->have_status && s->status.fault;
    uint32_t left = at_ctl_min_run_left_ms(t, now);
    switch (t->held) {
    case AT_HOLD_TIMER_ENDED:
    case AT_HOLD_THERMO_OFF:
        if (t->autot.enabled) {
            t->held = AT_HOLD_NONE;
        } else if (left == 0 || fault) {
            bool timer = t->held == AT_HOLD_TIMER_ENDED;
            set_note(&t->autot, "%s -> minimum run done, sending stop", timer ? "timer ended" : "Auto off");
            hearth_stop(t, ops, s, now, timer ? AT_END_TIMER : AT_END_THERMO_OFF, false);
        }
        break;
    case AT_HOLD_NO_READING:
        if (t->room_ok) {
            t->held = AT_HOLD_NONE;
        } else if (left == 0 || fault) {
            at_ctl_loop_t *l = t->run.owner == AT_OWNER_FREEZE ? &t->freeze : &t->autot;
            set_note(l, "no temperature reading -> minimum run done, sending stop");
            hearth_stop(t, ops, s, now, AT_END_NO_READING, false);
            l->last_action_ms = now;
            l->has_acted = true;
            l->stop_pending = true;
        }
        break;
    default:
        break;
    }
}

/* ---- the loops --------------------------------------------------------------------------------------------- */

static void tick_auto(at_ctl_t *t, const at_snapshot_t *s, uint32_t now, const at_ctl_ops_t *ops)
{
    at_ctl_loop_t *l = &t->autot;
    if (!l->enabled) {
        return;
    }
    if (t->run.running && !t->run.stopping && t->run.owner == AT_OWNER_FREEZE) take_from_frost(t);
    bool fault = s->have_status && s->status.fault;
    if (!fault) {
        reset_retry(l);
    }
    if (l->has_acted && now - l->last_action_ms < AT_CTL_MIN_ACTION_MS) {
        return;
    }
    if (!s->have_status) {
        return;
    }
    bool idle = is_idle(s);
    if (idle) {
        l->stop_pending = false;
    }
    bool in_run = t->run.running && !t->run.stopping;
    if (!t->room_ok) {
        /* every reading lost: a run carries on to the end of its minimum run, then stops; nothing starts */
        if (in_run && !l->stop_pending && t->held != AT_HOLD_NO_READING) {
            t->held = AT_HOLD_NO_READING;
            set_note(l, "no temperature reading -> holding: minimum run %u min left",
                     mins_up(at_ctl_min_run_left_ms(t, now)));
        } else if (!in_run) {
            set_note(l, "no temperature reading, waiting");
        }
        return;
    }
    const float temp = t->room;
    float lo, hi;
    at_ctl_working_range(t, &lo, &hi);
    /* The run only ends at the top of the range; the heater is never stopped and restarted inside it, which
     * would short-cycle it. */
    if (temp >= hi - EPS && !idle && !l->stop_pending && in_run) {
        uint32_t left = at_ctl_min_run_left_ms(t, now);
        if (left > 0 && !fault) {
            t->held = AT_HOLD_TOP;
            set_note(l, "at %.1f, above %.1f, holding: minimum run %u min left", temp, hi, mins_up(left));
        } else {
            set_note(l, "temperature %.1f >= %.1f -> stop", temp, hi);
            hearth_stop(t, ops, s, now, AT_END_TOP, true);
            l->last_action_ms = now;
            l->has_acted = true;
            l->stop_pending = true;
        }
    } else if (in_run && t->held == AT_HOLD_TOP && temp < hi - EPS) {
        t->held = AT_HOLD_NONE;
    } else if (temp <= lo + EPS && idle) {
        uint32_t off_left = at_ctl_min_off_left_ms(t, now);
        if (off_left > 0) {
            set_note(l, "at %.1f, below %.1f, waiting: minimum off %u min left", temp, lo, mins_up(off_left));
            return;
        }
        if (fault && !retry_ready(l, s, now)) {
            return;
        }
        if (fault) {
            set_note(l, "temperature %.1f <= %.1f -> start thermostat (fault retry %d/%d)",
                     temp, lo, l->retry_count, N_RETRIES);
        } else {
            set_note(l, "temperature %.1f <= %.1f -> start thermostat", temp, lo);
        }
        send_start(t, l, ops, now, AT_OWNER_AUTO);
    }
}

static void tick_freeze(at_ctl_t *t, const at_snapshot_t *s, uint32_t now, const at_ctl_ops_t *ops)
{
    at_ctl_loop_t *l = &t->freeze;
    if (!l->enabled) {
        l->started_by_me = false;
        return;
    }
    bool fault = s->have_status && s->status.fault;
    if (!fault) {
        reset_retry(l);
    }
    if (l->has_acted && now - l->last_action_ms < AT_CTL_MIN_ACTION_MS) {
        return;
    }
    if (!s->have_status) {
        return;
    }
    bool idle = is_idle(s);
    bool mine = t->run.running && !t->run.stopping && t->run.owner == AT_OWNER_FREEZE;
    if (idle && !t->run.running) {
        l->started_by_me = false;
        l->stop_pending = false;
    }
    if (!t->room_ok) {
        if (mine && t->held != AT_HOLD_NO_READING) {
            t->held = AT_HOLD_NO_READING;
            set_note(l, "no temperature reading -> holding: minimum run %u min left",
                     mins_up(at_ctl_min_run_left_ms(t, now)));
        }
        return;
    }
    const float temp = t->room;
    const float stop_at = l->target + AT_CTL_HYSTERESIS;
    if (mine && t->held == AT_HOLD_TOP && temp < stop_at - EPS) t->held = AT_HOLD_NONE;
    if (temp <= l->target + EPS && idle) {
        /* Auto starts it (or already has): one start, and the run is Auto's */
        if (start_pending(t, now) || auto_wants_start(t, s, now)) return;
        /* the only loop allowed to start inside the minimum off */
        if (fault && !retry_ready(l, s, now)) {
            return;
        }
        if (fault) {
            set_note(l, "temperature %.1f <= %.1f (frost floor) -> start thermostat (fault retry %d/%d)",
                     temp, l->target, l->retry_count, N_RETRIES);
        } else {
            set_note(l, "temperature %.1f <= %.1f (frost floor) -> start thermostat", temp, l->target);
        }
        send_start(t, l, ops, now, AT_OWNER_FREEZE);
        l->started_by_me = true;
    } else if (mine && temp >= stop_at - EPS && !l->stop_pending) {
        uint32_t left = at_ctl_min_run_left_ms(t, now);
        if (left > 0 && !fault) {
            t->held = AT_HOLD_TOP;
            set_note(l, "at %.1f, above %.1f, holding: minimum run %u min left", temp, stop_at, mins_up(left));
        } else {
            set_note(l, "temperature %.1f >= %.1f -> stop (frost-protection run ending)", temp, stop_at);
            hearth_stop(t, ops, s, now, AT_END_TOP, false);
            l->last_action_ms = now;
            l->has_acted = true;
            l->started_by_me = false;
            l->stop_pending = true;
        }
    }
}

void at_ctl_tick(at_ctl_t *t, const at_snapshot_t *s, uint32_t now_ms, const at_ctl_ops_t *ops)
{
    update_room(t, s, now_ms);
    track_run(t, s, now_ms);
    /* before the loops act: the reading that makes Hearth stop the run at the top is part of the run's fit (on the
     * controller's whole-degree sensor it is the last tick-over). Readings after a stop are kept out by the stop
     * itself (at_learn_stop). */
    if (t->room_ok) {
        float lo, hi;
        at_ctl_working_range(t, &lo, &hi);
        at_learn_sample(&t->learn, now_ms, t->room, (at_sensor_t)t->sensor, lo,
                        at_ctl_min_off_left_ms(t, now_ms) > 0);
    }
    check_ceiling(t, s, now_ms, ops);
    retry_stop(t, s, now_ms, ops);
    service_held(t, s, now_ms, ops);
    tick_auto(t, s, now_ms, ops);
    tick_freeze(t, s, now_ms, ops);
}

/* ---- status ------------------------------------------------------------------------------------------------ */

void at_ctl_status(const at_ctl_t *t, uint32_t now, at_ctl_status_t *o)
{
    memset(o, 0, sizeof(*o));
    const bool ctl = t->sensor == AT_SENS_CTL;
    const float uw = user_width(t), lw = t->learn.st.width;
    at_ctl_working_range(t, &o->work_low, &o->work_high);
    const float w = o->work_high - o->work_low;
    o->learned_width = lw;
    o->min_width = ctl ? AT_LEARN_CTL_FLOOR : AT_CTL_MIN_WIDTH;
    o->sensor = t->sensor;
    at_ctl_loop_t *who;
    if (t->run.running && ceiling_of(t, &o->ceiling, &who)) {
        /* the live run's own ceiling */
    } else if (!t->run.running && t->last_end == AT_END_CEILING && t->last_ceiling > 0) {
        o->ceiling = t->last_ceiling;   /* the one the last run actually stopped at (frost's is not Auto's) */
    } else {
        o->ceiling = t->autot.high + AT_CTL_CEILING_OVER;
    }
    o->at_limit = t->learn.st.s[t->sensor].limit;
    /* "widened"/"narrowed" when the learned width takes the range past the user's own; otherwise a working range
     * wider than the user's comes from the controller's sensor alone (its 2 C floor, or a learned width no wider than
     * the user's range put into whole degrees plus 1 C) */
    if (w <= uw + EPS) o->reason = "user";
    else if (ctl && lw <= uw + EPS) o->reason = "controller_sensor";
    else o->reason = t->learn.st.last_change == AT_EV_NARROWED ? "narrowed" : "widened";
    o->min_run_left_s = (at_ctl_min_run_left_ms(t, now) + 999u) / 1000u;
    o->min_off_left_s = (at_ctl_min_off_left_ms(t, now) + 999u) / 1000u;
    o->holding = t->held;
    o->running = t->run.running;
    if (t->run.running) {
        o->run_on_ago_s = (now - t->run.on_ms) / 1000u;
        o->flame = t->run.flame;
        if (t->run.flame) o->flame_on_ago_s = (now - t->run.flame_ms) / 1000u;
    }
    o->sensor_stuck = t->ext_stuck;
    o->have_rate = at_learn_rate(&t->learn, (at_sensor_t)t->sensor, &o->rise_rate);
    o->have_warm = at_learn_warm_typical(&t->learn, (at_sensor_t)t->sensor, &o->warmup_typical_s);
    o->have_cool = at_learn_cool_typical(&t->learn, (at_sensor_t)t->sensor, &o->cool_typical_s);
    o->dip_below = t->learn.dip > 0 ? t->learn.dip : (t->run.running ? t->learn.last_dip : 0);
    o->early_stop_unseen = t->early_unseen;
    o->last_run_end = t->last_end;
    if (t->n_early) {
        o->have_last_early = true;
        o->last_early = t->early[t->n_early - 1];
        o->last_early_ago_s = (now - o->last_early.ms) / 1000u;
    }
    o->no_reading = !t->room_ok;
}

/* ---- names ------------------------------------------------------------------------------------------------- */

static const char *const SRC_NAMES[] = {"unknown", "app", "web", "matter", "mqtt", "home_assistant", "controller",
                                        "console", "timer"};

const char *at_ctl_src_name(at_src_t s)
{
    return (unsigned)s < sizeof(SRC_NAMES) / sizeof(SRC_NAMES[0]) ? SRC_NAMES[s] : "unknown";
}

at_src_t at_ctl_src_parse(const char *name)
{
    if (!name) return AT_SRC_UNKNOWN;
    for (unsigned i = 0; i < sizeof(SRC_NAMES) / sizeof(SRC_NAMES[0]); i++) {
        if (!strcmp(name, SRC_NAMES[i])) return (at_src_t)i;
    }
    if (!strcmp(name, "ha")) return AT_SRC_HA;
    return AT_SRC_UNKNOWN;
}

const char *at_ctl_hold_name(at_hold_t h)
{
    switch (h) {
    case AT_HOLD_TOP: return "top";
    case AT_HOLD_TIMER_ENDED: return "timer_ended";
    case AT_HOLD_THERMO_OFF: return "thermostat_off";
    case AT_HOLD_NO_READING: return "no_reading";
    default: return NULL;
    }
}
