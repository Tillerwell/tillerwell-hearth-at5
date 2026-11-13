#include "at_learn.h"

#include <math.h>
#include <string.h>

#define EPS 1e-3f

static bool whole(at_sensor_t s) { return s == AT_SENS_CTL; }

void at_learn_init(at_learn_t *L)
{
    memset(L, 0, sizeof(*L));
    L->st.version = AT_LEARN_VERSION;
}

float at_learn_round_up(float w, bool whole_deg)
{
    const float step = whole_deg ? 1.0f : 0.5f;
    if (w <= 0) return 0;
    return ceilf(w / step - EPS) * step;
}

float at_learn_working_width(float learned, float user_w, bool ctl)
{
    /* the learned width is the real swing the space needs, on either sensor. On the controller's whole-degree sensor
     * the room swings the working width less 1 C, so that sensor runs on it rounded up to a whole degree plus 1 C;
     * the 1 C is applied here only, never saved (plan §2, §3.2) */
    float l = learned;
    if (ctl && learned > EPS) l = at_learn_round_up(learned, true) + AT_LEARN_CTL_SLACK;
    float w = user_w;
    if (l > w) w = l;
    if (ctl && w < AT_LEARN_CTL_FLOOR) w = AT_LEARN_CTL_FLOOR;
    if (w > AT_LEARN_MAX_W && user_w <= AT_LEARN_MAX_W) w = AT_LEARN_MAX_W;
    if (ctl) w = at_learn_round_up(w, true);
    return w;
}

/* ---- small helpers ------------------------------------------------------------------------------------------ */

static uint32_t median_u32(const uint32_t *v, int n)
{
    uint32_t a[AT_LEARN_MED];
    for (int i = 0; i < n; i++) a[i] = v[i];
    for (int i = 1; i < n; i++) {
        uint32_t x = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > x) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
    return n % 2 ? a[n / 2] : (a[n / 2 - 1] + a[n / 2]) / 2;
}

static void push_warm(at_learn_sensor_t *s, uint16_t sec)
{
    if (s->nwarm == AT_LEARN_MED) {
        memmove(&s->warm_s[0], &s->warm_s[1], sizeof(s->warm_s[0]) * (AT_LEARN_MED - 1));
        s->nwarm--;
    }
    s->warm_s[s->nwarm++] = sec;
}

static void push_cool(at_learn_sensor_t *s, uint32_t sec)
{
    if (s->ncool == AT_LEARN_MED) {
        memmove(&s->cool_s[0], &s->cool_s[1], sizeof(s->cool_s[0]) * (AT_LEARN_MED - 1));
        s->ncool--;
    }
    s->cool_s[s->ncool++] = sec;
}

static void push_vote(at_learn_sensor_t *s, at_learn_vote_t v)
{
    if (s->n == AT_LEARN_WIN) {
        memmove(&s->v[0], &s->v[1], sizeof(s->v[0]) * (AT_LEARN_WIN - 1));
        s->n--;
    }
    s->v[s->n++] = v;
}

static void push_event(at_learn_t *L, at_learn_event_t e)
{
    e.epoch = L->epoch;
    if (L->st.nhist == AT_LEARN_HIST) {
        memmove(&L->st.hist[0], &L->st.hist[1], sizeof(L->st.hist[0]) * (AT_LEARN_HIST - 1));
        L->st.nhist--;
    }
    L->st.hist[L->st.nhist++] = e;
    L->dirty = true;
}

static void push_run(at_learn_t *L, const at_learn_run_t *r)
{
    if (L->nruns == AT_LEARN_RUNS) {
        memmove(&L->runs[0], &L->runs[1], sizeof(L->runs[0]) * (AT_LEARN_RUNS - 1));
        L->nruns--;
    }
    L->runs[L->nruns++] = *r;
}

static bool warm_typical_s(const at_learn_t *L, at_sensor_t s, uint32_t *sec)
{
    const at_learn_sensor_t *ss = &L->st.s[s];
    if (!ss->nwarm) return false;
    uint32_t v[AT_LEARN_MED];
    for (int i = 0; i < ss->nwarm; i++) v[i] = ss->warm_s[i];
    *sec = median_u32(v, ss->nwarm);
    return true;
}

bool at_learn_warm_typical(const at_learn_t *L, at_sensor_t s, uint32_t *sec) { return warm_typical_s(L, s, sec); }

bool at_learn_cool_typical(const at_learn_t *L, at_sensor_t s, uint32_t *sec)
{
    const at_learn_sensor_t *ss = &L->st.s[s];
    if (!ss->ncool) return false;
    *sec = median_u32(ss->cool_s, ss->ncool);
    return true;
}

bool at_learn_rate(const at_learn_t *L, at_sensor_t s, float *rate)
{
    /* the latest run on this sensor with a good fit (1.59): counted, or rate_ok however it ended or was started, so a
     * space whose runs never count (each after more than 2 hours off) still shows how fast it heats. Then the window,
     * which survives a run log emptied by an older firmware's restart */
    const at_learn_sensor_t *ss = &L->st.s[s];
    for (int i = L->nruns - 1; i >= 0; i--) {
        const at_learn_run_t *r = &L->runs[i];
        if (r->sensor == s && r->have_rate && (r->why == AT_WHY_COUNTED || r->rate_ok)) {
            *rate = L->runs[i].rate;
            return true;
        }
    }
    if (ss->n) {
        *rate = ss->v[ss->n - 1].rate;
        return true;
    }
    return false;
}

/* ---- limits ------------------------------------------------------------------------------------------------- */

static float floor_w(float user_w, at_sensor_t s) { return at_learn_working_width(0, user_w, whole(s)); }

/* The widest learned width a sensor can use: 4 C, or 3 C on the controller's sensor (its working width is then at
 * the 4 C cap) */
static float max_learned(at_sensor_t s) { return whole(s) ? AT_LEARN_MAX_W - AT_LEARN_CTL_SLACK : AT_LEARN_MAX_W; }

static void set_limit(at_learn_t *L, at_sensor_t s, at_limit_t lim, float raw, float user_w)
{
    at_learn_sensor_t *ss = &L->st.s[s];
    if (ss->limit == lim) return;   /* already there: no write, no entry */
    at_learn_event_t e = {0};
    if (ss->limit != AT_LIMIT_NONE) {
        e.kind = AT_EV_LIMIT_CLEARED;
        e.sensor = (uint8_t)s;
        e.limit = ss->limit;
        e.old_w = e.new_w = L->st.width;
        e.work_w = at_learn_working_width(L->st.width, user_w, whole(s));
        push_event(L, e);
    }
    ss->limit = (uint8_t)lim;
    L->dirty = true;
    if (lim == AT_LIMIT_NONE) return;
    memset(&e, 0, sizeof(e));
    e.kind = AT_EV_LIMIT;
    e.sensor = (uint8_t)s;
    e.limit = (uint8_t)lim;
    e.old_w = e.new_w = L->st.width;
    e.raw_w = raw;
    e.work_w = at_learn_working_width(L->st.width, user_w, whole(s));
    push_event(L, e);
}

void at_learn_check_limits(at_learn_t *L, float user_w)
{
    for (int s = 0; s < 2; s++) {
        at_learn_sensor_t *ss = &L->st.s[s];
        bool still = true;
        if (ss->limit == AT_LIMIT_MAX) {
            still = L->st.width >= max_learned((at_sensor_t)s) - EPS;
        } else if (ss->limit == AT_LIMIT_MIN) {
            still = fabsf(at_learn_working_width(L->st.width, user_w, whole((at_sensor_t)s)) -
                          floor_w(user_w, (at_sensor_t)s)) < EPS;
        }
        if (!still) set_limit(L, (at_sensor_t)s, AT_LIMIT_NONE, 0, user_w);
    }
}

/* ---- deciding ----------------------------------------------------------------------------------------------- */

/* The two runs with the widest width needed among those voting `vote`: their rates for the history entry. */
static float widest(const at_learn_sensor_t *ss, at_vote_t vote, float *r1, float *r2)
{
    float best = -1, second = -1;
    *r1 = *r2 = 0;
    for (int i = 0; i < ss->n; i++) {
        if (ss->v[i].vote != vote) continue;
        if (ss->v[i].need > best) {
            second = best;
            *r2 = *r1;
            best = ss->v[i].need;
            *r1 = ss->v[i].rate;
        } else if (ss->v[i].need > second) {
            second = ss->v[i].need;
            *r2 = ss->v[i].rate;
        }
    }
    return best;
}

static void change_width(at_learn_t *L, at_sensor_t s, float target, at_learn_ev_t kind, float raw, float r1, float r2,
                         float user_w)
{
    at_learn_event_t e = {0};
    e.kind = (uint8_t)kind;
    e.sensor = (uint8_t)s;
    e.old_w = L->st.width;
    e.new_w = target;
    e.raw_w = raw;
    e.work_w = at_learn_working_width(target, user_w, whole(s));
    e.rate1 = r1;
    e.rate2 = r2;
    L->st.width = target;
    L->st.last_change = (uint8_t)kind;
    /* the next decision uses runs at the new width */
    L->st.s[0].n = 0;
    L->st.s[1].n = 0;
    push_event(L, e);
    /* a decision from this sensor clears its own limit; the other's is dropped if no longer true */
    set_limit(L, s, AT_LIMIT_NONE, 0, user_w);
    at_learn_check_limits(L, user_w);
}

static void decide(at_learn_t *L, at_sensor_t s, float user_w)
{
    at_learn_sensor_t *ss = &L->st.s[s];
    int nf = 0, ns = 0;
    for (int i = 0; i < ss->n; i++) {
        nf += ss->v[i].vote == AT_VOTE_FAST;
        ns += ss->v[i].vote == AT_VOTE_SLOW;
    }
    const bool ctl = whole(s);
    const float cur = at_learn_working_width(L->st.width, user_w, ctl);
    float r1, r2;
    if (nf >= 2) {
        float raw = widest(ss, AT_VOTE_FAST, &r1, &r2);
        /* the learned width keeps 0.5 C steps on either sensor; the controller's working width rounds it up again.
         * At 3 C the controller's working width is at the 4 C cap, so that is as far as its runs can take it */
        float target = at_learn_round_up(raw, false);
        if (target > max_learned(s)) target = max_learned(s);
        if (target <= L->st.width + EPS) {
            set_limit(L, s, AT_LIMIT_MAX, raw, user_w);
        } else {
            change_width(L, s, target, AT_EV_WIDENED, raw, r1, r2, user_w);
        }
        return;
    }
    if (ns >= 2) {
        float raw = widest(ss, AT_VOTE_SLOW, &r1, &r2);
        /* the coarser sensor cannot narrow below what it can see: its 2 C working floor is a learned width of 1 C, so
         * at 1 C or under it leaves the width alone */
        if (ctl && L->st.width <= AT_LEARN_CTL_SLACK + EPS) {
            set_limit(L, s, AT_LIMIT_MIN, raw, user_w);
            return;
        }
        float target;
        if (ctl) {
            /* a whole degree, and never below 1 C (the controller's 2 C floor) */
            target = at_learn_round_up(raw, true);
            if (target < AT_LEARN_CTL_SLACK) target = AT_LEARN_CTL_SLACK;
        } else {
            target = at_learn_round_up(raw, false);
            float fl = floor_w(user_w, s);
            if (target < fl) target = fl;
        }
        if (at_learn_working_width(target, user_w, ctl) < cur - EPS) {
            change_width(L, s, target, AT_EV_NARROWED, raw, r1, r2, user_w);
        } else if (fabsf(cur - floor_w(user_w, s)) < EPS) {
            set_limit(L, s, AT_LIMIT_MIN, raw, user_w);   /* the working width is at its floor */
        } else if (ss->limit != AT_LIMIT_NONE) {
            /* rounding keeps the working width where it is, above its floor: no change, and not a limit */
            set_limit(L, s, AT_LIMIT_NONE, 0, user_w);
        }
        return;
    }
    /* the latest runs no longer ask to go past the limit */
    if (ss->limit != AT_LIMIT_NONE) set_limit(L, s, AT_LIMIT_NONE, 0, user_w);
}

/* ---- the run ------------------------------------------------------------------------------------------------ */

void at_learn_run_start(at_learn_t *L, uint32_t now, at_sensor_t sensor, bool by_thermo, bool off_known,
                        uint32_t off_ms)
{
    /* the off period before this run ends here */
    L->last_have_cool = L->cool_on && !L->cool_bad && L->reached_bottom;
    L->last_cool_s = L->last_have_cool ? L->cool_ms / 1000u : 0;
    L->last_cool_sensor = L->cool_sensor;
    L->last_dip = L->dip;
    L->cool_on = false;   /* recorded at the run's end, once it has been compared with the typical time */
    L->in_run = true;
    L->on_ms = now;
    L->flame = false;
    L->sensor = (uint8_t)sensor;
    L->sensor_changed = false;
    L->by_thermo = by_thermo;
    L->off_ok = off_known && off_ms < AT_LEARN_MAX_OFF_MS;
    L->have_start_temp = false;
    L->have_base = false;
    L->warm_done = false;
    L->have_last = false;
    L->fit_frozen = false;
    L->sx = L->sy = L->sxx = L->sxy = 0;
    L->n = 0;
    L->lost_heat = false;
    L->resumed = false;
}

void at_learn_flame(at_learn_t *L, uint32_t now)
{
    if (!L->in_run || L->flame) return;
    L->flame = true;
    L->flame_ms = now;
    L->have_base = false;
}

void at_learn_sensor_changed(at_learn_t *L)
{
    if (L->in_run) L->sensor_changed = true;
    if (L->cool_on) L->cool_bad = true;
}

static void add_point(at_learn_t *L, uint32_t now, float temp)
{
    double x = (double)(now - L->flame_ms) / 60000.0;
    L->sx += x;
    L->sy += temp;
    L->sxx += x * x;
    L->sxy += x * temp;
    if (!L->n) L->fit_first_ms = now;
    L->fit_last_ms = now;
    L->n++;
}

void at_learn_sample(at_learn_t *L, uint32_t now, float temp, at_sensor_t sensor, float work_low, bool off_lock)
{
    /* cooling: from Hearth's stop at the top, purge included (the room often still rises then), the peak, then the
     * time from it down to the working bottom */
    if (L->cool_on) {
        if ((uint8_t)sensor != L->cool_sensor) L->cool_bad = true;
        if (!L->reached_bottom) {
            if (!L->have_peak || temp > L->peak) {
                L->peak = temp;
                L->peak_ms = now;
                L->have_peak = true;
            }
            if (temp <= work_low + EPS) {
                L->reached_bottom = true;
                L->cool_ms = now - L->peak_ms;
            }
        }
    }
    if (L->in_run) {
        if ((uint8_t)sensor != L->sensor) L->sensor_changed = true;
        if (!L->have_start_temp) {
            L->have_start_temp = true;
            L->start_temp = L->peak_temp = temp;
        }
        if (temp > L->peak_temp) L->peak_temp = temp;
        if (L->flame && !L->fit_frozen && !L->sensor_changed) {
            const bool w = whole(sensor);
            if (!L->warm_done) {
                if (!L->have_base || temp < L->base) {
                    L->base = temp;
                    L->have_base = true;
                }
                if (temp >= L->base + (w ? 1.0f : 0.2f) - EPS) {
                    L->warm_done = true;
                    L->warm_ms = now;
                    L->maxv = temp;
                    add_point(L, now, temp);
                }
            } else {
                if (!w || !L->have_last || temp != L->last) add_point(L, now, temp);
                if (temp > L->maxv) L->maxv = temp;
                if (temp <= L->maxv - AT_LEARN_LOST_HEAT + EPS) L->lost_heat = true;
            }
            L->last = temp;
            L->have_last = true;
        }
        return;
    }
    if (off_lock && temp < work_low && work_low - temp > L->dip) L->dip = work_low - temp;
}

void at_learn_stop(at_learn_t *L, uint32_t now, bool at_top)
{
    if (!L->in_run) return;
    if (!L->fit_frozen) {
        L->fit_frozen = true;
        L->stop_ms = now;
    }
    if (at_top) {
        L->cool_on = true;
        L->cool_sensor = L->sensor;
        L->cool_bad = L->sensor_changed;
        L->have_peak = false;
        L->reached_bottom = false;
        L->cool_ms = 0;
    }
}

void at_learn_resume(at_learn_t *L)
{
    if (!L->in_run || !L->fit_frozen) return;
    L->resumed = true;
    L->cool_on = false;
}

void at_learn_run_end(at_learn_t *L, uint32_t now, at_run_end_t end, float user_w)
{
    if (!L->in_run) return;
    if (!L->fit_frozen) {
        L->fit_frozen = true;
        L->stop_ms = now;
    }
    if (end != AT_END_TOP) L->cool_on = false;   /* the cooling record only follows Hearth's stop at the top */
    L->in_run = false;
    L->dip = 0;                                   /* a new off period starts */

    at_learn_run_t r;
    memset(&r, 0, sizeof(r));
    r.start_epoch = L->epoch ? L->epoch - (int64_t)((now - L->on_ms) / 1000u) : 0;
    r.light_s = L->flame ? (L->flame_ms - L->on_ms) / 1000u : 0;
    r.burn_s = L->flame ? (L->stop_ms - L->flame_ms) / 1000u : 0;
    r.sensor = L->sensor;
    r.end = (uint8_t)end;
    r.start_temp = L->have_start_temp ? L->start_temp : 0;
    r.peak_temp = L->have_start_temp ? L->peak_temp : 0;
    r.warm_s = 0xffff;
    r.have_cool = L->last_have_cool;
    r.cool_s = L->last_cool_s;
    r.dip = L->last_dip;

    const at_sensor_t s = (at_sensor_t)L->sensor;
    at_learn_sensor_t *ss = &L->st.s[s];
    const uint32_t warm_ms = L->warm_done ? L->warm_ms - L->flame_ms : 0;
    if (L->flame && L->warm_done && !L->sensor_changed) {
        r.warm_s = (uint16_t)(warm_ms / 1000u);
        if (warm_ms <= AT_LEARN_MAX_WARM_MS) {
            push_warm(ss, r.warm_s);
            L->dirty = true;
        }
    }

    /* the fit, when there is one */
    double denom = L->n * L->sxx - L->sx * L->sx;
    bool fit_ok = L->warm_done && L->n >= 2 && fabs(denom) > 1e-9;
    if (fit_ok) {
        double slope = (L->n * L->sxy - L->sx * L->sy) / denom;
        r.rate = (float)slope;
        r.have_rate = true;
    }
    /* on the whole-degree sensor a run over a 2 C working range has 2 tick-overs (the warm-up end and one more):
     * that is enough (plan §3.2: fewer than 2 is not) */
    bool enough = fit_ok && warm_ms <= AT_LEARN_MAX_WARM_MS &&
                  (whole(s) ? L->n >= 2 : L->fit_last_ms - L->fit_first_ms >= AT_LEARN_MIN_FIT_MS);

    /* a good fit, however the run ended or was started: what warm by (at_warm) learns its rates from */
    r.rate_ok = L->flame && enough && !L->sensor_changed && !L->lost_heat && !L->resumed && r.rate >= AT_LEARN_MIN_RATE;

    if (!L->by_thermo) r.why = AT_WHY_NOT_THERMOSTAT;
    else if (!L->off_ok) r.why = AT_WHY_LONG_OFF;
    else if (end != AT_END_TOP || L->resumed) r.why = AT_WHY_ENDED;
    else if (L->sensor_changed) r.why = AT_WHY_SENSOR_CHANGED;
    else if (!L->flame || !enough) r.why = AT_WHY_NOT_ENOUGH_DATA;
    else if (L->lost_heat || r.rate < AT_LEARN_MIN_RATE) r.why = AT_WHY_LOST_HEAT;
    else r.why = AT_WHY_COUNTED;

    if (r.why == AT_WHY_COUNTED) {
        uint32_t delay_s = 0;
        warm_typical_s(L, s, &delay_s);   /* includes this run */
        float delay_min = delay_s / 60.0f;
        if (delay_min > AT_LEARN_MAX_WARM_MS / 60000.0f) delay_min = AT_LEARN_MAX_WARM_MS / 60000.0f;
        float cur = at_learn_working_width(L->st.width, user_w, whole(s));
        /* on the controller's whole-degree sensor the start fires as the reading falls into the bottom degree and
         * the stop as it enters the top one, so the room really swings the working width less 1 C: judge the run
         * on that. The width needed is the real swing for the target time on either sensor; the controller's 1 C
         * is added only where its working width is worked out (plan §3.2) */
        const float slack = whole(s) ? AT_LEARN_CTL_SLACK : 0.0f;
        r.need = r.rate * (AT_LEARN_TARGET_MIN - delay_min);
        r.climb_min = delay_min + (cur - slack) / r.rate;
        r.vote = r.climb_min < AT_LEARN_FAST_MIN ? AT_VOTE_FAST : r.climb_min > AT_LEARN_SLOW_MIN ? AT_VOTE_SLOW : AT_VOTE_OK;
        /* cold-night guard: a slow run after an off period that cooled in under half the typical time */
        uint32_t typ;
        if (r.vote == AT_VOTE_SLOW && r.have_cool && L->last_cool_sensor == L->sensor &&
            at_learn_cool_typical(L, s, &typ) && r.cool_s * 2 < typ) {
            r.why = AT_WHY_COLD_SPELL;
        }
    }
    /* the off period's cooling joins the typical time after it has been compared with it */
    if (L->last_have_cool) {
        push_cool(&L->st.s[L->last_cool_sensor < 2 ? L->last_cool_sensor : 0], L->last_cool_s);
        L->dirty = true;
        L->last_have_cool = false;
    }
    if (r.why == AT_WHY_COUNTED) {
        at_learn_vote_t v = {r.need, r.rate, r.vote};
        push_vote(ss, v);
        L->dirty = true;
        decide(L, s, user_w);
    }
    push_run(L, &r);
}

void at_learn_reset(at_learn_t *L)
{
    float old = L->st.width;
    memset(&L->st, 0, sizeof(L->st));
    L->st.version = AT_LEARN_VERSION;
    at_learn_event_t e = {0};
    e.kind = AT_EV_RESET;
    e.old_w = old;
    push_event(L, e);
    L->dirty = true;
}

static bool finite_f(float f) { return f == f && f > -1e6f && f < 1e6f; }

bool at_learn_restore(at_learn_t *L, const at_learn_store_t *st)
{
    if (st->version != AT_LEARN_VERSION) return false;
    if (!finite_f(st->width) || st->width < 0 || st->width > AT_LEARN_MAX_W + EPS) return false;
    if (fabsf(st->width * 2 - roundf(st->width * 2)) > EPS) return false;
    if (st->last_change > AT_EV_NARROWED || st->nhist > AT_LEARN_HIST) return false;
    for (int s = 0; s < 2; s++) {
        const at_learn_sensor_t *ss = &st->s[s];
        if (ss->n > AT_LEARN_WIN || ss->nwarm > AT_LEARN_MED || ss->ncool > AT_LEARN_MED || ss->limit > AT_LIMIT_MIN)
            return false;
        for (int i = 0; i < ss->n; i++) {
            if (!finite_f(ss->v[i].need) || !finite_f(ss->v[i].rate) || ss->v[i].vote > AT_VOTE_SLOW) return false;
        }
    }
    for (int i = 0; i < st->nhist; i++) {
        const at_learn_event_t *e = &st->hist[i];
        if (e->kind < AT_EV_WIDENED || e->kind > AT_EV_RESET || e->sensor > AT_SENS_CTL || e->limit > AT_LIMIT_MIN)
            return false;
        if (!finite_f(e->old_w) || !finite_f(e->new_w) || !finite_f(e->raw_w) || !finite_f(e->work_w) ||
            !finite_f(e->rate1) || !finite_f(e->rate2))
            return false;
    }
    L->st = *st;
    L->dirty = false;
    return true;
}

/* ---- names -------------------------------------------------------------------------------------------------- */

const char *at_learn_ev_name(at_learn_ev_t e)
{
    switch (e) {
    case AT_EV_WIDENED: return "widened";
    case AT_EV_NARROWED: return "narrowed";
    case AT_EV_LIMIT: return "limit_reached";
    case AT_EV_LIMIT_CLEARED: return "limit_cleared";
    case AT_EV_RESET: return "reset";
    default: return "none";
    }
}

const char *at_learn_end_name(at_run_end_t e)
{
    switch (e) {
    case AT_END_TOP: return "top";
    case AT_END_USER: return "user";
    case AT_END_EXTERNAL: return "external";
    case AT_END_CEILING: return "ceiling";
    case AT_END_FAULT: return "fault";
    case AT_END_NO_READING: return "no_reading";
    case AT_END_HEATER: return "heater";
    case AT_END_TIMER: return "timer_ended";
    case AT_END_THERMO_OFF: return "thermostat_off";
    default: return "none";
    }
}

const char *at_learn_why_name(at_learn_why_t w)
{
    switch (w) {
    case AT_WHY_COUNTED: return "counted";
    case AT_WHY_NOT_THERMOSTAT: return "not_thermostat";
    case AT_WHY_LONG_OFF: return "long_off";
    case AT_WHY_ENDED: return "not_at_top";
    case AT_WHY_SENSOR_CHANGED: return "sensor_changed";
    case AT_WHY_LOST_HEAT: return "lost_heat";
    case AT_WHY_NOT_ENOUGH_DATA: return "not_enough_data";
    case AT_WHY_COLD_SPELL: return "cold_spell";
    default: return "unknown";
    }
}

const char *at_learn_vote_name(at_vote_t v)
{
    return v == AT_VOTE_FAST ? "fast" : v == AT_VOTE_SLOW ? "slow" : "ok";
}

const char *at_learn_limit_name(at_limit_t l)
{
    return l == AT_LIMIT_MAX ? "max" : l == AT_LIMIT_MIN ? "min" : NULL;
}

const char *at_learn_sensor_name(at_sensor_t s) { return s == AT_SENS_CTL ? "controller" : "external"; }
