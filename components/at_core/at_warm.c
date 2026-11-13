#include "at_warm.h"

#include <math.h>
#include <string.h>

#define EPS 1e-3f

void at_warm_init(at_warm_t *w)
{
    memset(w, 0, sizeof(*w));
    w->st.version = AT_WARM_VERSION;
}

static bool finite_f(float f) { return f == f && f > -1e6f && f < 1e6f; }

bool at_warm_restore(at_warm_t *w, const at_warm_store_t *st)
{
    if (st->version != AT_WARM_VERSION || st->nlight > AT_WARM_KEEP || st->nerr > AT_WARM_ERR_KEEP ||
        st->nlead > AT_WARM_ERR_KEEP || st->nrec > AT_WARM_REC)
        return false;
    for (int s = 0; s < 2; s++) {
        if (st->ngross[s] > AT_WARM_KEEP || st->nrise[s] > AT_WARM_KEEP) return false;
        for (int i = 0; i < st->ngross[s]; i++) {
            if (!finite_f(st->gross[s][i]) || st->gross[s][i] <= 0 || st->gross[s][i] > 10) return false;
        }
        for (int i = 0; i < st->nrise[s]; i++) {
            if (!finite_f(st->rise[s][i]) || st->rise[s][i] <= 0 || st->rise[s][i] > 10) return false;
        }
    }
    for (int i = 0; i < st->nerr; i++) {
        if (st->err_min[i] < -120 || st->err_min[i] > AT_WARM_LATE_MIN) return false;
    }
    for (int i = 0; i < st->nlead; i++) {
        if (st->lead_min[i] > AT_WARM_MAX_LEAD_MIN) return false;
    }
    for (int i = 0; i < st->nrec; i++) {
        if (st->rec[i].est > AT_WARM_EST_FIXED || st->rec[i].result > AT_WARM_RES_STOPPED) return false;
    }
    /* the tracker and a run being followed are RAM only and carry on */
    w->st = *st;
    w->dirty = false;
    return true;
}

void at_warm_reset(at_warm_t *w)
{
    memset(&w->st, 0, sizeof(w->st));
    w->st.version = AT_WARM_VERSION;
    w->armed = false;
    w->dirty = true;
}

/* ---- lists ------------------------------------------------------------------------------------------------- */

static void push_f(float *v, uint8_t *n, float x)
{
    if (*n == AT_WARM_KEEP) {
        memmove(&v[0], &v[1], (AT_WARM_KEEP - 1) * sizeof(v[0]));
        (*n)--;
    }
    v[(*n)++] = x;
}

float at_warm_median(const float *src, int n)
{
    float v[AT_WARM_KEEP];
    if (n > AT_WARM_KEEP) n = AT_WARM_KEEP;
    memcpy(v, src, (size_t)n * sizeof(v[0]));
    for (int i = 1; i < n; i++) {
        float x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
}

static float median_u16(const uint16_t *src, int n)
{
    float v[AT_WARM_KEEP];
    for (int i = 0; i < n && i < AT_WARM_KEEP; i++) v[i] = src[i];
    return at_warm_median(v, n);
}

static float median_i8(const int8_t *src, int n)
{
    float v[AT_WARM_KEEP];
    for (int i = 0; i < n && i < AT_WARM_KEEP; i++) v[i] = src[i];
    return at_warm_median(v, n);
}

/* ---- the idle cooling tracker ------------------------------------------------------------------------------ */

static void clear_points(at_warm_t *w)
{
    w->npts = 0;
    w->bucket_on = false;
}

static void push_point(at_warm_t *w, uint32_t ms, float v)
{
    if (w->npts == AT_WARM_IDLE_PTS) {
        memmove(&w->pt_ms[0], &w->pt_ms[1], (AT_WARM_IDLE_PTS - 1) * sizeof(w->pt_ms[0]));
        memmove(&w->pt_v[0], &w->pt_v[1], (AT_WARM_IDLE_PTS - 1) * sizeof(w->pt_v[0]));
        w->npts--;
    }
    w->pt_ms[w->npts] = ms;
    w->pt_v[w->npts] = v;
    w->npts++;
}

void at_warm_sample(at_warm_t *w, uint32_t now, float temp, uint8_t sensor, bool running)
{
    if (running) return;
    if (!w->idle_on || sensor != w->idle_sensor) {
        /* a fresh start, or the other sensor took over: its readings are not comparable */
        w->idle_on = true;
        w->idle_sensor = sensor;
        w->peak = temp;
        clear_points(w);
    }
    if (w->wait_peak) {
        /* after a run the room keeps rising for a while on the heat still in the heater: that is not a loss of 0 */
        if (temp > w->peak) w->peak = temp;
        if (temp > w->peak - AT_WARM_PEAK_FALL + EPS) return;
        w->wait_peak = false;
    }
    if (!w->bucket_on) {
        w->bucket_on = true;
        w->bucket_ms = now;
        w->bucket_sum = 0;
        w->bucket_n = 0;
    } else if (now - w->bucket_ms >= 60000u) {
        if (w->bucket_n) push_point(w, w->bucket_ms + 30000u, w->bucket_sum / (float)w->bucket_n);
        w->bucket_ms = now;
        w->bucket_sum = 0;
        w->bucket_n = 0;
    }
    w->bucket_sum += temp;
    w->bucket_n++;
}

bool at_warm_idle_loss(const at_warm_t *w, uint8_t sensor, int min_span, float *loss)
{
    if (!w->idle_on || w->wait_peak || w->idle_sensor != sensor || w->npts < 3) return false;
    const uint32_t last = w->pt_ms[w->npts - 1];
    int first = 0;
    while (first < w->npts && last - w->pt_ms[first] > (uint32_t)AT_WARM_IDLE_PTS * 60000u) first++;
    int n = w->npts - first;
    if (n < 3) return false;
    /* minutes of readings, counting each one-minute average as a minute */
    uint32_t span_min = (last - w->pt_ms[first]) / 60000u + 1u;
    if (span_min < (uint32_t)min_span) return false;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = first; i < w->npts; i++) {
        double x = (double)(w->pt_ms[i] - w->pt_ms[first]) / 60000.0, y = w->pt_v[i];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    double den = n * sxx - sx * sx;
    if (fabs(den) < 1e-9) return false;
    double slope = (n * sxy - sx * sy) / den;
    /* a room warming with the heater off (sun, a stove) is a loss of 0, never a negative one */
    *loss = slope < 0 ? (float)-slope : 0.0f;
    return true;
}

void at_warm_run_start(at_warm_t *w)
{
    w->run_loss_ok = at_warm_idle_loss(w, w->idle_sensor, AT_WARM_GROSS_IDLE_MIN, &w->run_loss);
    w->run_sensor = w->idle_sensor;
    w->idle_on = false;
    w->wait_peak = true;
    clear_points(w);
}

void at_warm_run_end(at_warm_t *w, const at_learn_run_t *r, bool rate_ok)
{
    if (r->light_s > 0 && r->light_s < 1800) {
        if (w->st.nlight == AT_WARM_KEEP) {
            memmove(&w->st.light_s[0], &w->st.light_s[1], (AT_WARM_KEEP - 1) * sizeof(w->st.light_s[0]));
            w->st.nlight--;
        }
        w->st.light_s[w->st.nlight++] = (uint16_t)r->light_s;
        w->dirty = true;
    }
    const int s = r->sensor == AT_SENS_CTL ? AT_SENS_CTL : AT_SENS_EXT;
    if (rate_ok && r->have_rate && r->rate >= AT_LEARN_MIN_RATE && r->rate <= 10) {
        push_f(w->st.rise[s], &w->st.nrise[s], r->rate);
        if (w->run_loss_ok && w->run_sensor == r->sensor) {
            push_f(w->st.gross[s], &w->st.ngross[s], r->rate + w->run_loss);
        }
        w->dirty = true;
    }
    w->run_loss_ok = false;
}

/* ---- the prediction ---------------------------------------------------------------------------------------- */

void at_warm_predict(const at_warm_t *w, const at_warm_in_t *in, at_warm_pred_t *o)
{
    memset(o, 0, sizeof(*o));
    const float light = (w->st.nlight ? median_u16(w->st.light_s, w->st.nlight) : AT_WARM_DEF_LIGHT_S) / 60.0f;
    uint32_t warm_s = in->have_warm ? in->warm_s : AT_WARM_DEF_WARM_S;
    if (warm_s > AT_LEARN_MAX_WARM_MS / 1000u) warm_s = AT_LEARN_MAX_WARM_MS / 1000u;
    const float warm = warm_s / 60.0f;
    const float off = in->off_left_s / 60.0f;
    const int s = in->sensor == AT_SENS_CTL ? AT_SENS_CTL : AT_SENS_EXT;

    float lead = AT_WARM_FIXED_LEAD_MIN + off;
    o->est = AT_WARM_EST_FIXED;
    o->flame_climb_min = -1;   /* not known: no start inside the range on a fixed lead */
    if (in->have_temp) {
        float climb_c = in->top - in->temp;
        if (climb_c < 0) climb_c = 0;
        float loss;
        if (s == AT_SENS_EXT && w->st.ngross[s] >= AT_WARM_MIN_LEARN &&
            at_warm_idle_loss(w, (uint8_t)s, AT_WARM_LOSS_MIN, &loss)) {
            o->est = AT_WARM_EST_MODEL;
            o->loss = loss;
            o->rate = at_warm_median(w->st.gross[s], w->st.ngross[s]) - loss;
            if (o->rate < AT_LEARN_MIN_RATE) {
                /* the heater cannot be shown to win against the loss: start as early as allowed */
                lead = AT_WARM_MAX_LEAD_MIN + 1 + off;
                o->flame_climb_min = 1e4f;
            } else {
                float climb = climb_c / o->rate;
                float margin = climb * 0.1f;
                if (margin < 5) margin = 5;
                if (w->st.nerr) {
                    o->corr_min = median_i8(w->st.err_min, w->st.nerr);
                    if (o->corr_min > AT_WARM_CORR_MAX_MIN) o->corr_min = AT_WARM_CORR_MAX_MIN;
                    if (o->corr_min < -AT_WARM_CORR_MAX_MIN) o->corr_min = -AT_WARM_CORR_MAX_MIN;
                }
                lead = light + warm + climb + margin + o->corr_min + off;
                o->flame_climb_min = warm + climb;
            }
        } else if (w->st.nrise[s] >= AT_WARM_MIN_LEARN) {
            o->est = AT_WARM_EST_TYPICAL;
            o->rate = at_warm_median(w->st.rise[s], w->st.nrise[s]);
            float climb = climb_c / o->rate;
            lead = light + warm + climb * (s == AT_SENS_CTL ? 1.25f : 1.2f) + off;
            o->flame_climb_min = warm + climb;
        }
    }
    if (lead < 0) lead = 0;
    o->raw_lead_s = (uint32_t)lroundf(lead * 60.0f);
    o->capped = o->raw_lead_s > AT_WARM_MAX_LEAD_MIN * 60u;
    o->lead_s = o->capped ? AT_WARM_MAX_LEAD_MIN * 60u : o->raw_lead_s;
}

bool at_warm_typical_lead(const at_warm_t *w, uint32_t *min)
{
    if (!w->st.nlead) return false;
    *min = (uint32_t)lroundf(median_u16(w->st.lead_min, w->st.nlead));
    return true;
}

/* ---- following a warm-by run ------------------------------------------------------------------------------- */

void at_warm_fired(at_warm_t *w, int64_t fired_epoch, int64_t start_epoch, const at_warm_pred_t *p, float top,
                   bool have_temp, float temp, at_warm_res_t hint, uint32_t lead_s)
{
    memset(&w->cur, 0, sizeof(w->cur));
    w->cur.start_epoch = start_epoch;
    w->cur.fired_epoch = fired_epoch;
    w->cur.predicted_epoch = fired_epoch + (int64_t)p->raw_lead_s;
    w->cur.start_temp_dc = have_temp ? (int16_t)lroundf(temp * 10.0f) : 0;
    w->cur.target_c = (int8_t)lroundf(top);
    w->cur.est = p->est;
    w->cur.result = AT_WARM_RES_PENDING;
    w->cur.lead_min = (uint16_t)((lead_s + 30u) / 60u);
    w->hint = (uint8_t)hint;
    w->armed = true;
    w->run_seen = false;
    if (lead_s > 0) {
        if (w->st.nlead == AT_WARM_ERR_KEEP) {
            memmove(&w->st.lead_min[0], &w->st.lead_min[1], (AT_WARM_ERR_KEEP - 1) * sizeof(w->st.lead_min[0]));
            w->st.nlead--;
        }
        w->st.lead_min[w->st.nlead++] = w->cur.lead_min > AT_WARM_MAX_LEAD_MIN ? AT_WARM_MAX_LEAD_MIN : w->cur.lead_min;
        w->dirty = true;
    }
}

static void finish(at_warm_t *w)
{
    at_warm_rec_t r = w->cur;
    if (w->hint != AT_WARM_RES_PENDING) r.result = w->hint;
    else if (!w->run_seen) r.result = AT_WARM_RES_IN_RANGE;
    else if (r.est != AT_WARM_EST_MODEL) r.result = AT_WARM_RES_FALLBACK;
    else r.result = AT_WARM_RES_COUNTED;
    if (r.result == AT_WARM_RES_COUNTED) {
        int64_t e = r.actual_epoch ? (r.actual_epoch - r.predicted_epoch) : (int64_t)AT_WARM_LATE_MIN * 60;
        int64_t m = e >= 0 ? (e + 30) / 60 : -((-e + 30) / 60);
        if (m > AT_WARM_LATE_MIN) m = AT_WARM_LATE_MIN;
        if (m < -120) m = -120;
        r.err_min = (int8_t)m;
        if (w->st.nerr == AT_WARM_ERR_KEEP) {
            memmove(&w->st.err_min[0], &w->st.err_min[1], (AT_WARM_ERR_KEEP - 1) * sizeof(w->st.err_min[0]));
            w->st.nerr--;
        }
        w->st.err_min[w->st.nerr++] = r.err_min;
    }
    if (w->st.nrec == AT_WARM_REC) {
        memmove(&w->st.rec[0], &w->st.rec[1], (AT_WARM_REC - 1) * sizeof(w->st.rec[0]));
        w->st.nrec--;
    }
    w->st.rec[w->st.nrec++] = r;
    w->armed = false;
    w->dirty = true;
}

void at_warm_watch(at_warm_t *w, int64_t epoch, bool have_temp, float temp, float stop_at, bool running)
{
    if (!w->armed) return;
    if (running) w->run_seen = true;
    if (epoch <= 0) return;
    if (have_temp && temp >= stop_at - EPS) {
        w->cur.actual_epoch = epoch;
        finish(w);
        return;
    }
    if (epoch >= w->cur.start_epoch + (int64_t)AT_WARM_LATE_MIN * 60) finish(w);
}

void at_warm_run_ended(at_warm_t *w, int64_t epoch, at_run_end_t end)
{
    (void)epoch;
    (void)end;
    if (!w->armed || !w->run_seen) return;
    /* the room did not get there in this run (it would have finished already): someone stopped it, a fault, the
     * ceiling, a lost reading. It says nothing about the prediction */
    if (w->hint == AT_WARM_RES_PENDING) w->hint = AT_WARM_RES_STOPPED;
    finish(w);
}

void at_warm_abandon(at_warm_t *w, int64_t epoch)
{
    (void)epoch;
    if (!w->armed) return;
    if (w->hint == AT_WARM_RES_PENDING) w->hint = w->run_seen ? AT_WARM_RES_STOPPED : AT_WARM_RES_IN_RANGE;
    finish(w);
}

/* ---- names ------------------------------------------------------------------------------------------------- */

const char *at_warm_est_name(at_warm_est_t e)
{
    switch (e) {
    case AT_WARM_EST_MODEL: return "model";
    case AT_WARM_EST_TYPICAL: return "typical";
    case AT_WARM_EST_FIXED: return "fixed";
    default: return NULL;
    }
}

const char *at_warm_res_name(at_warm_res_t r)
{
    switch (r) {
    case AT_WARM_RES_COUNTED: return "counted";
    case AT_WARM_RES_CAPPED: return "capped";
    case AT_WARM_RES_DELAYED: return "delayed";
    case AT_WARM_RES_IN_RANGE: return "in_range";
    case AT_WARM_RES_AT_START: return "at_start";
    case AT_WARM_RES_FALLBACK: return "fallback";
    case AT_WARM_RES_STOPPED: return "stopped";
    default: return "pending";
    }
}
