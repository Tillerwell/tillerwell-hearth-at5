#include "at_early.h"

#include <math.h>
#include <string.h>

#include "at_learn.h"

#define EPS 1e-3f

void at_early_init(at_early_t *e)
{
    memset(e, 0, sizeof(*e));
    e->st.version = AT_EARLY_VERSION;
}

bool at_early_restore(at_early_t *e, const at_early_store_t *st)
{
    if (st->version != AT_EARLY_VERSION || st->busy_cc < 0 ||
        st->busy_cc > (int16_t)(AT_EARLY_MAX_BUSY * 100.0f + 0.5f)) return false;
    e->st = *st;
    e->dirty = false;
    return true;
}

void at_early_reset(at_early_t *e)
{
    at_early_init(e);
    e->dirty = true;
}

float at_early_busy(const at_early_t *e) { return e->st.busy_cc / 100.0f; }

static void set_busy(at_early_t *e, float w)
{
    int16_t cc = (int16_t)lroundf(w * 100.0f);
    if (cc < 0) cc = 0;
    if (cc > (int16_t)(AT_EARLY_MAX_BUSY * 100.0f + 0.5f)) cc = (int16_t)(AT_EARLY_MAX_BUSY * 100.0f + 0.5f);
    if (cc != e->st.busy_cc) {
        e->st.busy_cc = cc;
        e->dirty = true;
    }
}

void at_early_auto_start(at_early_t *e, uint32_t now_ms, float work_width)
{
    /* calm: this start comes long after the one before (only judged when there was one since the restart) */
    if (e->nstarts > 0 && now_ms - e->start_ms[e->nstarts - 1] > AT_EARLY_BUSY_MS) {
        set_busy(e, at_early_busy(e) - AT_EARLY_CALM);
    }
    if (e->nstarts == AT_EARLY_BUSY_N) {
        memmove(&e->start_ms[0], &e->start_ms[1], (AT_EARLY_BUSY_N - 1) * sizeof(e->start_ms[0]));
        e->nstarts--;
    }
    e->start_ms[e->nstarts++] = now_ms;
    /* busy: this start makes AT_EARLY_BUSY_N inside AT_EARLY_BUSY_MS */
    if (e->nstarts == AT_EARLY_BUSY_N && now_ms - e->start_ms[0] <= AT_EARLY_BUSY_MS) {
        float cap = work_width - AT_EARLY_MIN_BAND;
        if (cap < 0) cap = 0;
        float w = at_early_busy(e) + AT_EARLY_WIDEN;
        if (w > cap) w = cap;
        if (w > at_early_busy(e)) set_busy(e, w);
        /* each busy step needs AT_EARLY_BUSY_N new starts: one fast burst widens once, not on every start after */
        e->nstarts = 0;
        e->start_ms[e->nstarts++] = now_ms;
    }
}

void at_early_point(const at_early_t *e, const at_early_in_t *in, at_early_out_t *o)
{
    memset(o, 0, sizeof(*o));
    o->busy_w = at_early_busy(e);
    if (!in->ext) {
        o->est = AT_EARLY_EST_OFF;
        o->start_at = in->lo;
        return;
    }
    float early;
    if (in->have_gross) {
        o->est = in->have_loss ? AT_EARLY_EST_MODEL : AT_EARLY_EST_GROSS;
        o->net_rate = in->gross - (in->have_loss ? in->loss : 0);
        o->have_rate = true;
        uint32_t warm_s = in->have_warm ? in->warm_s : 0;   /* not known: no warm-up, the longest climb */
        if (warm_s > AT_LEARN_MAX_WARM_MS / 1000u) warm_s = AT_LEARN_MAX_WARM_MS / 1000u;
        float mins = AT_EARLY_RUN_MIN - warm_s / 60.0f;
        if (mins < 0) mins = 0;
        /* the heater cannot be shown to beat the loss: the run will last its 30 minutes from anywhere */
        o->climb = o->net_rate < AT_LEARN_MIN_RATE ? 0 : o->net_rate * mins * AT_EARLY_MARGIN;
        early = in->stop_at - o->climb;
    } else {
        o->est = AT_EARLY_EST_HALFWAY;
        early = in->hi - AT_EARLY_HALFWAY;
    }
    float upper = in->hi - AT_EARLY_MIN_BAND - o->busy_w;
    if (upper > in->stop_at - AT_EARLY_BELOW_STOP) upper = in->stop_at - AT_EARLY_BELOW_STOP;
    if (early > upper) early = upper;
    /* 0.1 steps, rounded down: a start a little lower is a run a little longer, the safe side */
    early = floorf(early * 10.0f + EPS) / 10.0f;
    if (early < in->lo) early = in->lo;
    o->start_at = early;
}

const char *at_early_est_name(at_early_est_t est)
{
    switch (est) {
    case AT_EARLY_EST_MODEL: return "model";
    case AT_EARLY_EST_GROSS: return "gross";
    case AT_EARLY_EST_HALFWAY: return "halfway";
    default: return "off";
    }
}
