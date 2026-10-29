#include "at_ctl.h"

#include <stdio.h>
#include <string.h>

static const uint32_t RETRY_DELAYS_MS[] = {5u * 60000u, 15u * 60000u, 20u * 60000u};
#define N_RETRIES ((int)(sizeof(RETRY_DELAYS_MS) / sizeof(RETRY_DELAYS_MS[0])))

static void set_note(at_ctl_loop_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void set_note(at_ctl_loop_t *l, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->note, sizeof(l->note), fmt, ap);
    va_end(ap);
}

static float set_temp(const at_ctl_loop_t *l);

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
    t->autot.low = auto_target - AT_CTL_HYSTERESIS;
    t->autot.high = auto_target + AT_CTL_HYSTERESIS;
    /* Auto restored at boot is mid-cycle: wait for low rather than risk a short run. */
    t->autot.reached_high = true;
    t->freeze.target = freeze_target;
    t->use_external = true;
}

void at_ctl_set_external(at_ctl_t *t, float temp, uint32_t now_ms)
{
    t->have_external = true;
    t->external_temp = temp;
    t->external_ms = now_ms;
}

bool at_ctl_effective_temp(const at_ctl_t *t, const at_snapshot_t *s, uint32_t now_ms,
                           float *temp, uint32_t *age_ms, bool *external)
{
    if (t->use_external && t->have_external && now_ms - t->external_ms <= AT_CTL_EXT_STALE_MS) {
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

/* A reading the loops may act on.  The controller's own sensor reports every few
 * seconds, so anything older than AT_CTL_MAX_READING_AGE_MS is suspect; a pushed
 * external reading (Home Assistant sends one on change or every 30 s) counts
 * until it goes stale at AT_CTL_EXT_STALE_MS. */
static bool usable_temp(const at_ctl_t *t, const at_snapshot_t *s, uint32_t now, float *temp)
{
    uint32_t age;
    bool ext;
    return at_ctl_effective_temp(t, s, now, temp, &age, &ext) && (ext || age <= AT_CTL_MAX_READING_AGE_MS);
}

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

void at_ctl_set_auto(at_ctl_t *t, bool enabled, const at_snapshot_t *s, uint32_t now, const at_ctl_ops_t *ops)
{
    at_ctl_loop_t *l = &t->autot;
    bool was = l->enabled;
    l->enabled = enabled;
    if (enabled == was) {
        return;
    }
    if (enabled) {
        float temp;
        const float set = set_temp(l);
        l->reached_high = false;
        if (!usable_temp(t, s, now, &temp)) {
            /* never start blind: with no reading nothing could stop the run */
            set_note(l, "Auto thermostat enabled -> no temperature reading yet, waiting");
        } else if (temp > set) {
            set_note(l, "Auto thermostat enabled -> temperature %.1f above set %.1f, waiting", temp, set);
        } else {
            set_note(l, "Auto thermostat enabled -> sending start thermostat");
            ops->start_thermostat(ops->ctx);
        }
        reset_retry(l);
    } else {
        set_note(l, "Auto thermostat disabled -> sending stop");
        ops->stop(ops->ctx);
        l->stop_pending = false;
    }
    l->has_acted = true;
    l->last_action_ms = now;
}

void at_ctl_disarm_auto(at_ctl_t *t, const char *note)
{
    t->autot.enabled = false;
    t->autot.stop_pending = false;
    t->autot.has_acted = false;
    reset_retry(&t->autot);
    set_note(&t->autot, "%s", note ? note : "physical controller took control");
}

void at_ctl_set_auto_target(at_ctl_t *t, float target)
{
    t->autot.target = target;
    t->autot.low = target - AT_CTL_HYSTERESIS;
    t->autot.high = target + AT_CTL_HYSTERESIS;
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
}

void at_ctl_set_hysteresis(at_ctl_t *t, float hyst_min, float hyst_max)
{
    t->autot.hyst_min = hyst_min < 0 ? 0 : hyst_min;
    t->autot.hyst_max = hyst_max < 0 ? 0 : hyst_max;
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

/* Like the Comfort Controller's set temperature: its thermostat runs to set + 1
 * (high) and restarts at set - 2 (low). */
static float set_temp(const at_ctl_loop_t *l)
{
    float set = l->high - AT_CTL_HYSTERESIS;
    return set < l->low ? l->low : set;
}

float at_ctl_auto_start_at(const at_ctl_t *t)
{
    return t->autot.reached_high ? t->autot.low : set_temp(&t->autot);
}

static bool is_idle(const at_snapshot_t *s)
{
    return s->have_status && s->status.state_raw == 0x00;
}

static void tick_auto(at_ctl_t *t, const at_snapshot_t *s, uint32_t now, const at_ctl_ops_t *ops)
{
    at_ctl_loop_t *l = &t->autot;
    if (!l->enabled) {
        return;
    }
    bool fault = s->have_status && s->status.fault;
    if (!fault) {
        reset_retry(l);
    }
    if (l->has_acted && now - l->last_action_ms < AT_CTL_MIN_ACTION_MS) {
        return;
    }
    float temp;
    if (!usable_temp(t, s, now, &temp) || !s->have_status) {
        return;
    }
    bool idle = is_idle(s);
    if (idle) {
        l->stop_pending = false;
    }
    /* The run only ends at the top of the band; the heater is never stopped
     * and restarted inside it, which would short-cycle it. */
    if (temp >= l->high) {
        l->reached_high = true;
    }
    const float stop_at = l->high;
    const float start_at = l->reached_high ? l->low : set_temp(l);
    if (temp >= stop_at && !idle && !l->stop_pending) {
        set_note(l, "temperature %.1f >= %.1f -> stop", temp, stop_at);
        ops->stop(ops->ctx);
        l->last_action_ms = now;
        l->has_acted = true;
        l->stop_pending = true;
    } else if (temp <= start_at && idle) {
        if (fault && !retry_ready(l, s, now)) {
            return;
        }
        if (fault) {
            set_note(l, "temperature %.1f <= %.1f -> start thermostat (fault retry %d/%d)",
                     temp, start_at, l->retry_count, N_RETRIES);
        } else {
            set_note(l, "temperature %.1f <= %.1f -> start thermostat", temp, start_at);
        }
        ops->start_thermostat(ops->ctx);
        l->last_action_ms = now;
        l->has_acted = true;
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
    float temp;
    if (!usable_temp(t, s, now, &temp) || !s->have_status) {
        return;
    }
    bool idle = is_idle(s);
    if (idle) {
        l->started_by_me = false;
    }
    if (temp <= l->target && idle) {
        if (fault && !retry_ready(l, s, now)) {
            return;
        }
        if (fault) {
            set_note(l, "temperature %.1f <= %.1f (frost floor) -> start thermostat (fault retry %d/%d)",
                     temp, l->target, l->retry_count, N_RETRIES);
        } else {
            set_note(l, "temperature %.1f <= %.1f (frost floor) -> start thermostat", temp, l->target);
        }
        ops->start_thermostat(ops->ctx);
        l->last_action_ms = now;
        l->has_acted = true;
        l->started_by_me = true;
    } else if (l->started_by_me && temp >= l->target + AT_CTL_HYSTERESIS && !idle) {
        set_note(l, "temperature %.1f >= %.1f -> stop (frost-protection run ending)",
                 temp, l->target + AT_CTL_HYSTERESIS);
        ops->stop(ops->ctx);
        l->last_action_ms = now;
        l->has_acted = true;
        l->started_by_me = false;
    }
}

void at_ctl_tick(at_ctl_t *t, const at_snapshot_t *s, uint32_t now_ms, const at_ctl_ops_t *ops)
{
    tick_auto(t, s, now_ms, ops);
    tick_freeze(t, s, now_ms, ops);
}
