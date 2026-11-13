/*
 * Heater Care documents (docs/API_SPEC.md): GET /api/thermo/history and the backup's "thermo_learn" object.
 *
 * Written with a plain string builder, not a cJSON tree: twenty runs and twenty history entries are several
 * hundred nodes, and the heap has been short before (1.0045/1.0046). The backup's object goes in as one raw node.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "at_app.h"
#include "cJSON.h"

typedef struct {
    char *buf;
    size_t len, cap;
    bool bad;
} sb_t;

static void sb_add(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_add(sb_t *b, const char *fmt, ...)
{
    if (b->bad) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (n < 0) {
            b->bad = true;
            return;
        }
        if (b->len + (size_t)n < b->cap) {
            b->len += (size_t)n;
            return;
        }
        size_t cap = b->cap * 2 + (size_t)n;
        char *nb = realloc(b->buf, cap);
        if (!nb) {
            b->bad = true;
            return;
        }
        b->buf = nb;
        b->cap = cap;
    }
}

static bool sb_init(sb_t *b, size_t cap)
{
    b->buf = malloc(cap);
    b->len = 0;
    b->cap = cap;
    b->bad = !b->buf;
    if (b->buf) b->buf[0] = 0;
    return !b->bad;
}

static char *sb_done(sb_t *b)
{
    if (b->bad) {
        free(b->buf);
        return NULL;
    }
    return b->buf;
}

/* a number with up to 4 decimals, no trailing zeros */
static void sb_num(sb_t *b, const char *key, double v)
{
    if (!isfinite(v)) v = 0;
    char t[24];
    snprintf(t, sizeof(t), "%.4f", v);
    char *e = t + strlen(t) - 1;
    while (e > t && *e == '0') *e-- = 0;
    if (*e == '.') *e = 0;
    if (!strcmp(t, "-0")) strcpy(t, "0");
    sb_add(b, "\"%s\":%s", key, t);
}

static void sb_epoch(sb_t *b, const char *key, int64_t e)
{
    if (e > 0) sb_add(b, "\"%s\":%.0f", key, (double)e);   /* not %lld: this printf has no long long */
    else sb_add(b, "\"%s\":null", key);
}

static void sb_str(sb_t *b, const char *key, const char *v)
{
    if (v) sb_add(b, "\"%s\":\"%s\"", key, v);
    else sb_add(b, "\"%s\":null", key);
}

static void add_event(sb_t *b, const at_learn_event_t *e)
{
    sb_add(b, "{");
    sb_epoch(b, "at", e->epoch);
    sb_add(b, ",");
    sb_str(b, "kind", at_learn_ev_name((at_learn_ev_t)e->kind));
    sb_add(b, ",");
    sb_str(b, "sensor", at_learn_sensor_name((at_sensor_t)e->sensor));
    sb_add(b, ",");
    sb_str(b, "limit", at_learn_limit_name((at_limit_t)e->limit));
    sb_add(b, ",");
    sb_num(b, "old", e->old_w);
    sb_add(b, ",");
    sb_num(b, "new", e->new_w);
    sb_add(b, ",");
    sb_num(b, "worked_out", e->raw_w);
    sb_add(b, ",");
    sb_num(b, "working", e->work_w);
    sb_add(b, ",");
    sb_num(b, "rate1", e->rate1);
    sb_add(b, ",");
    sb_num(b, "rate2", e->rate2);
    sb_add(b, "}");
}

/* The history document. The backup's copy leaves out the early stops: they are RAM only, the apps read the last one
 * from the status, and their times are worked out from the uptime, so they would change the backup every minute. */
static char *history_json(bool early_stops)
{
    at_learn_t *L = malloc(sizeof(*L));
    at_ctl_early_t *early = malloc(sizeof(at_ctl_early_t) * AT_CTL_EARLY_LOG);
    if (!L || !early) {
        free(L);
        free(early);
        return NULL;
    }
    int n_early = 0;
    uint32_t now_ms;
    at_app_learn_snapshot(L, early, &n_early, &now_ms);
    at_view_t *v = malloc(sizeof(*v));
    if (v) at_app_view(v);
    const int64_t epoch = v && v->clock_ok ? v->epoch : 0;
    const uint8_t sensor = v ? v->thermo.sensor : AT_SENS_EXT;
    free(v);

    sb_t b;
    if (!sb_init(&b, 4096)) {
        free(L);
        free(early);
        return NULL;
    }
    sb_add(&b, "{");
    sb_num(&b, "learned_width", L->st.width);
    sb_add(&b, ",");
    sb_str(&b, "sensor", at_learn_sensor_name((at_sensor_t)sensor));
    sb_add(&b, ",");
    sb_str(&b, "limit_ext", at_learn_limit_name((at_limit_t)L->st.s[AT_SENS_EXT].limit));
    sb_add(&b, ",");
    sb_str(&b, "limit_ctl", at_learn_limit_name((at_limit_t)L->st.s[AT_SENS_CTL].limit));
    sb_add(&b, ",\"history\":[");
    for (int i = L->st.nhist - 1; i >= 0; i--) {   /* newest first */
        if (i != L->st.nhist - 1) sb_add(&b, ",");
        add_event(&b, &L->st.hist[i]);
    }
    sb_add(&b, "],\"runs\":[");
    for (int i = L->nruns - 1; i >= 0; i--) {
        const at_learn_run_t *r = &L->runs[i];
        if (i != L->nruns - 1) sb_add(&b, ",");
        sb_add(&b, "{");
        sb_epoch(&b, "at", r->start_epoch);
        sb_add(&b, ",");
        sb_str(&b, "sensor", at_learn_sensor_name((at_sensor_t)r->sensor));
        sb_add(&b, ",");
        sb_str(&b, "end", at_learn_end_name((at_run_end_t)r->end));
        sb_add(&b, ",\"counted\":%s,", r->why == AT_WHY_COUNTED ? "true" : "false");
        sb_str(&b, "why", at_learn_why_name((at_learn_why_t)r->why));
        sb_add(&b, ",");
        sb_str(&b, "vote", r->why == AT_WHY_COUNTED || r->why == AT_WHY_COLD_SPELL
                               ? at_learn_vote_name((at_vote_t)r->vote) : NULL);
        sb_add(&b, ",\"light_s\":%u,\"burn_s\":%u,", (unsigned)r->light_s, (unsigned)r->burn_s);
        if (r->warm_s != 0xffff) sb_add(&b, "\"warmup_s\":%u,", (unsigned)r->warm_s);
        else sb_add(&b, "\"warmup_s\":null,");
        if (r->have_rate) {
            sb_num(&b, "rate", r->rate);
        } else {
            sb_add(&b, "\"rate\":null");
        }
        sb_add(&b, ",");
        if (r->why == AT_WHY_COUNTED || r->why == AT_WHY_COLD_SPELL) {
            sb_num(&b, "need", r->need);
            sb_add(&b, ",");
            sb_num(&b, "climb_min", r->climb_min);
        } else {
            sb_add(&b, "\"need\":null,\"climb_min\":null");
        }
        sb_add(&b, ",");
        sb_num(&b, "start_temp", r->start_temp);
        sb_add(&b, ",");
        sb_num(&b, "peak_temp", r->peak_temp);
        sb_add(&b, ",");
        if (r->have_cool) sb_add(&b, "\"cooling_s\":%u,", (unsigned)r->cool_s);
        else sb_add(&b, "\"cooling_s\":null,");
        sb_num(&b, "dip", r->dip);
        sb_add(&b, "}");
    }
    sb_add(&b, "]");
    /* warm-by runs (1.56), newest first: fixed once written (epochs, never ages), so the backup does not churn */
    at_warm_store_t *ws = malloc(sizeof(*ws));
    sb_add(&b, ",\"warm_by\":[");
    if (ws && at_app_warm_snapshot(ws)) {
        for (int i = ws->nrec - 1; i >= 0; i--) {
            const at_warm_rec_t *r = &ws->rec[i];
            if (i != ws->nrec - 1) sb_add(&b, ",");
            sb_add(&b, "{");
            sb_epoch(&b, "start", r->start_epoch);
            sb_add(&b, ",");
            sb_epoch(&b, "fired", r->fired_epoch);
            sb_add(&b, ",");
            sb_epoch(&b, "predicted", r->predicted_epoch);
            sb_add(&b, ",");
            sb_epoch(&b, "actual", r->actual_epoch);
            sb_add(&b, ",");
            sb_num(&b, "start_temp", r->start_temp_dc / 10.0);
            sb_add(&b, ",\"target\":%d,\"lead_min\":%u,", (int)r->target_c, (unsigned)r->lead_min);
            sb_str(&b, "estimate", at_warm_est_name((at_warm_est_t)r->est));
            sb_add(&b, ",");
            sb_str(&b, "result", at_warm_res_name((at_warm_res_t)r->result));
            if (r->result == AT_WARM_RES_COUNTED) sb_add(&b, ",\"error_min\":%d}", (int)r->err_min);
            else sb_add(&b, ",\"error_min\":null}");
        }
    }
    free(ws);
    sb_add(&b, "]");
    if (early_stops) sb_add(&b, ",\"early_stops\":[");
    for (int i = n_early - 1; early_stops && i >= 0; i--) {
        if (i != n_early - 1) sb_add(&b, ",");
        sb_add(&b, "{");
        sb_epoch(&b, "at", epoch ? epoch - (int64_t)((now_ms - early[i].ms) / 1000u) : 0);
        sb_add(&b, ",\"after_s\":%u,", (unsigned)early[i].after_s);
        sb_str(&b, "source", at_ctl_src_name((at_src_t)early[i].src));
        sb_add(&b, "}");
    }
    sb_add(&b, early_stops ? "]}" : "}");
    free(L);
    free(early);
    return sb_done(&b);
}

char *at_app_thermo_history_json(void) { return history_json(true); }

cJSON *at_app_thermo_care_backup(void)
{
    char *js = history_json(false);
    if (!js) return NULL;
    cJSON *raw = cJSON_CreateRaw(js);
    free(js);
    return raw;
}

/* ---- backup ------------------------------------------------------------------------------------------------- */

static void add_window(sb_t *b, const char *key, const at_learn_sensor_t *s)
{
    sb_add(b, "\"%s\":[", key);
    for (int i = 0; i < s->n; i++) {
        if (i) sb_add(b, ",");
        sb_add(b, "{");
        sb_num(b, "need", s->v[i].need);
        sb_add(b, ",");
        sb_num(b, "rate", s->v[i].rate);
        sb_add(b, ",");
        sb_str(b, "vote", at_learn_vote_name((at_vote_t)s->v[i].vote));
        sb_add(b, "}");
    }
    sb_add(b, "]");
}

cJSON *at_app_thermo_learn_backup(void)
{
    at_learn_t *L = malloc(sizeof(*L));
    if (!L) return NULL;
    uint32_t now_ms;
    at_app_learn_snapshot(L, NULL, NULL, &now_ms);
    sb_t b;
    if (!sb_init(&b, 2048)) {
        free(L);
        return NULL;
    }
    const at_learn_store_t *st = &L->st;
    static const char *const sfx[2] = {"ext", "ctl"};
    char key[24];
    sb_add(&b, "{");
    sb_num(&b, "width", st->width);
    sb_add(&b, ",");
    sb_str(&b, "last_change", st->last_change ? at_learn_ev_name((at_learn_ev_t)st->last_change) : NULL);
    for (int s = 0; s < 2; s++) {
        sb_add(&b, ",");
        snprintf(key, sizeof(key), "window_%s", sfx[s]);
        add_window(&b, key, &st->s[s]);
        sb_add(&b, ",\"warmup_%s\":[", sfx[s]);
        for (int i = 0; i < st->s[s].nwarm; i++) sb_add(&b, "%s%u", i ? "," : "", (unsigned)st->s[s].warm_s[i]);
        sb_add(&b, "],\"cooling_%s\":[", sfx[s]);
        for (int i = 0; i < st->s[s].ncool; i++) sb_add(&b, "%s%u", i ? "," : "", (unsigned)st->s[s].cool_s[i]);
        sb_add(&b, "],");
        snprintf(key, sizeof(key), "limit_%s", sfx[s]);
        sb_str(&b, key, at_learn_limit_name((at_limit_t)st->s[s].limit));
    }
    /* warm by's lists and the early stop's rises (1.56): they change once per run at most */
    at_warm_store_t *ws = malloc(sizeof(*ws));
    if (ws && at_app_warm_snapshot(ws)) {
        sb_add(&b, ",\"light\":[");
        for (int i = 0; i < ws->nlight; i++) sb_add(&b, "%s%u", i ? "," : "", (unsigned)ws->light_s[i]);
        for (int s = 0; s < 2; s++) {
            sb_add(&b, "],\"gross_%s\":[", sfx[s]);
            for (int i = 0; i < ws->ngross[s]; i++) sb_add(&b, "%s%.4f", i ? "," : "", (double)ws->gross[s][i]);
            sb_add(&b, "],\"rise_%s\":[", sfx[s]);
            for (int i = 0; i < ws->nrise[s]; i++) sb_add(&b, "%s%.4f", i ? "," : "", (double)ws->rise[s][i]);
        }
        sb_add(&b, "],\"warm_by_err\":[");
        for (int i = 0; i < ws->nerr; i++) sb_add(&b, "%s%d", i ? "," : "", (int)ws->err_min[i]);
        sb_add(&b, "],\"warm_by_lead\":[");
        for (int i = 0; i < ws->nlead; i++) sb_add(&b, "%s%u", i ? "," : "", (unsigned)ws->lead_min[i]);
        /* the warm-by runs themselves (1.58), oldest first, compact: [start, fired, predicted, actual, start temp in
         * tenths, target, estimate, result, error min, lead min] with the at_warm enum numbers */
        sb_add(&b, "],\"warm_by_runs\":[");
        for (int i = 0; i < ws->nrec; i++) {
            const at_warm_rec_t *r = &ws->rec[i];
            sb_add(&b, "%s[%.0f,%.0f,%.0f,%.0f,%d,%d,%u,%u,%d,%u]", i ? "," : "", (double)r->start_epoch,
                   (double)r->fired_epoch, (double)r->predicted_epoch, (double)r->actual_epoch, (int)r->start_temp_dc,
                   (int)r->target_c, (unsigned)r->est, (unsigned)r->result, (int)r->err_min, (unsigned)r->lead_min);
        }
        sb_add(&b, "]");
    }
    free(ws);
    at_coast_store_t cs;
    if (at_app_coast_snapshot(&cs)) {
        sb_add(&b, ",\"coast\":[");
        for (int i = 0; i < cs.n; i++) sb_add(&b, "%s%.2f", i ? "," : "", cs.rise_cc[i] / 100.0);
        sb_add(&b, "]");
    }
    /* early start's busy widening (1.60) */
    at_early_store_t es;
    if (at_app_early_snapshot(&es)) sb_add(&b, ",\"early\":{\"busy_w\":%.2f}", es.busy_cc / 100.0);
    /* the heater's cooldown lengths (1.52) and Heater Care's run log (1.55), so a replacement box keeps them (1.58).
     * Runs oldest first, compact: [at, light_s, burn_s, sensor, end, why, vote, rate|null, need, climb_min,
     * start_temp, peak_temp, warmup_s|null, cooling_s|null, dip, rate_ok] with the at_learn enum numbers */
    at_cd_store_t cd;
    if (at_app_cd_snapshot(&cd)) {
        sb_add(&b, ",\"cooldown\":[");
        for (int i = 0; i < cd.n; i++) sb_add(&b, "%s%u", i ? "," : "", (unsigned)cd.s[i]);
        sb_add(&b, "]");
    }
    sb_add(&b, ",\"runs\":[");
    for (int i = 0; i < L->nruns; i++) {
        const at_learn_run_t *r = &L->runs[i];
        sb_add(&b, "%s[%.0f,%u,%u,%u,%u,%u,%u,", i ? "," : "", (double)r->start_epoch, (unsigned)r->light_s,
               (unsigned)r->burn_s, (unsigned)r->sensor, (unsigned)r->end, (unsigned)r->why, (unsigned)r->vote);
        if (r->have_rate) sb_add(&b, "%.4f,", (double)r->rate);
        else sb_add(&b, "null,");
        sb_add(&b, "%.3f,%.1f,%.2f,%.2f,", (double)r->need, (double)r->climb_min, (double)r->start_temp,
               (double)r->peak_temp);
        if (r->warm_s != 0xffff) sb_add(&b, "%u,", (unsigned)r->warm_s);
        else sb_add(&b, "null,");
        if (r->have_cool) sb_add(&b, "%u,", (unsigned)r->cool_s);
        else sb_add(&b, "null,");
        sb_add(&b, "%.2f,%u]", (double)r->dip, r->rate_ok ? 1u : 0u);
    }
    sb_add(&b, "]");
    sb_add(&b, ",\"history\":[");
    for (int i = 0; i < st->nhist; i++) {   /* oldest first, as stored */
        if (i) sb_add(&b, ",");
        add_event(&b, &st->hist[i]);
    }
    sb_add(&b, "]}");
    free(L);
    char *js = sb_done(&b);
    if (!js) return NULL;
    cJSON *raw = cJSON_CreateRaw(js);
    free(js);
    return raw;
}

static int name_index(const char *v, const char *const *names, int n)
{
    if (!v) return 0;
    for (int i = 0; i < n; i++) {
        if (names[i] && !strcmp(v, names[i])) return i;
    }
    return -1;
}

static double num(const cJSON *o, const char *k, double def)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(x) ? x->valuedouble : def;
}

static const char *str(const cJSON *o, const char *k)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(x) ? x->valuestring : NULL;
}

static const char *const LIMITS[] = {NULL, "max", "min"};
static const char *const VOTES[] = {"ok", "fast", "slow"};
static const char *const SENSORS[] = {"external", "controller"};
static const char *const KINDS[] = {"none", "widened", "narrowed", "limit_reached", "limit_cleared", "reset"};

/* A number list into v (at most max); false if anything in it is not a number in [lo, hi]. Absent: n stays. */
static bool num_list(const cJSON *o, const char *key, double lo, double hi, double *v, int max, int *n)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, key), *it;
    if (!a) return true;
    if (!cJSON_IsArray(a)) return false;
    *n = 0;
    cJSON_ArrayForEach(it, a)
    {
        if (!cJSON_IsNumber(it) || it->valuedouble < lo || it->valuedouble > hi) return false;
        if (*n < max) v[(*n)++] = it->valuedouble;
        else {
            memmove(&v[0], &v[1], (size_t)(max - 1) * sizeof(v[0]));   /* keep the newest */
            v[max - 1] = it->valuedouble;
        }
    }
    return true;
}

/* A JSON array row of n_min..n_max numbers (null reads as NAN) into f; false if it is anything else. */
static bool num_row(const cJSON *row, double *f, int n_min, int n_max)
{
    if (!cJSON_IsArray(row)) return false;
    int n = 0;
    const cJSON *x;
    cJSON_ArrayForEach(x, row)
    {
        if (n >= n_max) return false;
        if (cJSON_IsNull(x)) f[n++] = NAN;
        else if (cJSON_IsNumber(x) && isfinite(x->valuedouble)) f[n++] = x->valuedouble;
        else return false;
    }
    return n >= n_min;
}

static const char *const END_NAMES[] = {"none", "top", "user", "external", "ceiling", "fault", "no_reading", "heater",
                                        "timer_ended", "thermostat_off"};
static const char *const WHY_NAMES[] = {"counted", "not_thermostat", "long_off", "not_at_top", "sensor_changed",
                                        "lost_heat", "not_enough_data", "cold_spell"};

/* One run from the backup: the compact row the backup writes, or an object in the GET /api/thermo/history "runs"
 * shape (names as printed there), so runs rebuilt from elsewhere can be put back with a plain backup round trip. */
static bool run_from_json(const cJSON *it, at_learn_run_t *r)
{
    memset(r, 0, sizeof(*r));
    r->warm_s = 0xffff;
    if (cJSON_IsArray(it)) {
        double f[16];
        /* [at, light_s, burn_s, sensor, end, why, vote, rate|null, need, climb_min, start_temp, peak_temp,
         *  warmup_s|null, cooling_s|null, dip, rate_ok] */
        if (!num_row(it, f, 16, 16)) return false;
        const int must[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 14, 15};
        for (unsigned k = 0; k < sizeof(must) / sizeof(must[0]); k++) {
            if (isnan(f[must[k]])) return false;
        }
        if (f[0] < 0 || f[1] < 0 || f[2] < 0 || f[1] > 4e9 || f[2] > 4e9) return false;
        for (int k = 3; k <= 6; k++) {
            if (f[k] < 0 || f[k] > 255) return false;
        }
        r->start_epoch = (int64_t)f[0];
        r->light_s = (uint32_t)f[1];
        r->burn_s = (uint32_t)f[2];
        r->sensor = (uint8_t)f[3];
        r->end = (uint8_t)f[4];
        r->why = (uint8_t)f[5];
        r->vote = (uint8_t)f[6];
        r->have_rate = !isnan(f[7]);
        r->rate = r->have_rate ? (float)f[7] : 0;
        r->need = (float)f[8];
        r->climb_min = (float)f[9];
        r->start_temp = (float)f[10];
        r->peak_temp = (float)f[11];
        if (!isnan(f[12]) && f[12] >= 0 && f[12] < 65535) r->warm_s = (uint16_t)f[12];
        r->have_cool = !isnan(f[13]) && f[13] >= 0 && f[13] < 4e9;
        r->cool_s = r->have_cool ? (uint32_t)f[13] : 0;
        r->dip = (float)f[14];
        r->rate_ok = f[15] != 0;
        return true;
    }
    if (!cJSON_IsObject(it)) return false;
    int sensor = name_index(str(it, "sensor"), SENSORS, 2);
    int end = name_index(str(it, "end"), END_NAMES, 10);
    int why = name_index(str(it, "why"), WHY_NAMES, 8);
    int vote = str(it, "vote") ? name_index(str(it, "vote"), VOTES, 3) : 0;
    const double at = num(it, "at", -1);
    if (sensor < 0 || end < 0 || why < 0 || vote < 0 || at < 0) return false;
    r->start_epoch = (int64_t)at;
    r->sensor = (uint8_t)sensor;
    r->end = (uint8_t)end;
    r->why = (uint8_t)why;
    r->vote = (uint8_t)vote;
    double light = num(it, "light_s", 0), burn = num(it, "burn_s", 0);
    if (light < 0 || burn < 0 || light > 4e9 || burn > 4e9) return false;
    r->light_s = (uint32_t)light;
    r->burn_s = (uint32_t)burn;
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(it, "rate");
    r->have_rate = cJSON_IsNumber(x);
    r->rate = r->have_rate ? (float)x->valuedouble : 0;
    r->need = (float)num(it, "need", 0);
    r->climb_min = (float)num(it, "climb_min", 0);
    r->start_temp = (float)num(it, "start_temp", 0);
    r->peak_temp = (float)num(it, "peak_temp", 0);
    double w = num(it, "warmup_s", -1), c = num(it, "cooling_s", -1);
    if (w >= 0 && w < 65535) r->warm_s = (uint16_t)w;
    r->have_cool = c >= 0 && c < 4e9;
    r->cool_s = r->have_cool ? (uint32_t)c : 0;
    r->dip = (float)num(it, "dip", 0);
    r->rate_ok = false;   /* not known from the history shape: it never feeds warm by */
    return true;
}

/* the cooldown lengths and the run log (1.58): optional, so an older backup still restores */
static bool restore_heater_logs(const cJSON *o)
{
    bool ok = true;
    const cJSON *cd = cJSON_GetObjectItemCaseSensitive(o, "cooldown");
    if (cd) {
        at_cd_store_t st = {0};
        st.version = AT_CD_VERSION;
        double v[AT_CD_KEEP];
        int n = 0;
        ok = num_list(o, "cooldown", 0, 65535, v, AT_CD_KEEP, &n);
        st.n = (uint8_t)n;
        for (int i = 0; i < n; i++) st.s[i] = (uint16_t)v[i];
        ok = ok && at_app_cd_put(&st);
    }
    const cJSON *runs = cJSON_GetObjectItemCaseSensitive(o, "runs");
    if (ok && runs) {
        if (!cJSON_IsArray(runs)) return false;
        at_learn_run_t *in = calloc(AT_LEARN_RUNS, sizeof(*in));
        if (!in) return false;
        int n = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, runs)
        {
            at_learn_run_t r;
            if (!run_from_json(it, &r)) {
                ok = false;
                break;
            }
            if (n == AT_LEARN_RUNS) {   /* keep the newest */
                memmove(&in[0], &in[1], (AT_LEARN_RUNS - 1) * sizeof(in[0]));
                n--;
            }
            in[n++] = r;
        }
        /* merged with the box's own log by `at`; at_runlog_merge checks the enums and the floats. Display only */
        ok = ok && at_app_runlog_merge(in, n);
        free(in);
    }
    return ok;
}

/* warm by's lists and coast (1.56): optional, so an older backup still restores; only the keys present change */
static bool restore_warm(const cJSON *o)
{
    at_warm_store_t *ws = malloc(sizeof(*ws));
    if (!ws) return false;
    bool ok = at_app_warm_snapshot(ws);
    double v[AT_WARM_KEEP];
    int n;
    static const char *const sfx[2] = {"ext", "ctl"};
    char key[24];
    n = ws->nlight;
    ok = ok && num_list(o, "light", 1, 1799, v, AT_WARM_KEEP, &n);
    if (ok && cJSON_GetObjectItemCaseSensitive(o, "light")) {
        ws->nlight = (uint8_t)n;
        for (int i = 0; i < n; i++) ws->light_s[i] = (uint16_t)v[i];
    }
    for (int s = 0; s < 2 && ok; s++) {
        snprintf(key, sizeof(key), "gross_%s", sfx[s]);
        if (cJSON_GetObjectItemCaseSensitive(o, key)) {
            ok = num_list(o, key, 1e-4, 10, v, AT_WARM_KEEP, &n);
            ws->ngross[s] = ok ? (uint8_t)n : 0;
            for (int i = 0; ok && i < n; i++) ws->gross[s][i] = (float)v[i];
        }
        snprintf(key, sizeof(key), "rise_%s", sfx[s]);
        if (ok && cJSON_GetObjectItemCaseSensitive(o, key)) {
            ok = num_list(o, key, 1e-4, 10, v, AT_WARM_KEEP, &n);
            ws->nrise[s] = ok ? (uint8_t)n : 0;
            for (int i = 0; ok && i < n; i++) ws->rise[s][i] = (float)v[i];
        }
    }
    if (ok && cJSON_GetObjectItemCaseSensitive(o, "warm_by_err")) {
        ok = num_list(o, "warm_by_err", -120, AT_WARM_LATE_MIN, v, AT_WARM_ERR_KEEP, &n);
        ws->nerr = ok ? (uint8_t)n : 0;
        for (int i = 0; ok && i < n; i++) ws->err_min[i] = (int8_t)v[i];
    }
    if (ok && cJSON_GetObjectItemCaseSensitive(o, "warm_by_lead")) {
        ok = num_list(o, "warm_by_lead", 0, AT_WARM_MAX_LEAD_MIN, v, AT_WARM_ERR_KEEP, &n);
        ws->nlead = ok ? (uint8_t)n : 0;
        for (int i = 0; ok && i < n; i++) ws->lead_min[i] = (uint16_t)v[i];
    }
    const cJSON *recs = cJSON_GetObjectItemCaseSensitive(o, "warm_by_runs");
    if (ok && recs) {
        if (!cJSON_IsArray(recs)) ok = false;
        ws->nrec = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, recs)
        {
            double f[10];
            if (!num_row(it, f, 10, 10)) {
                ok = false;
                break;
            }
            if (ws->nrec == AT_WARM_REC) {   /* keep the newest */
                memmove(&ws->rec[0], &ws->rec[1], (AT_WARM_REC - 1) * sizeof(ws->rec[0]));
                ws->nrec--;
            }
            at_warm_rec_t *r = &ws->rec[ws->nrec++];
            memset(r, 0, sizeof(*r));
            r->start_epoch = (int64_t)f[0];
            r->fired_epoch = (int64_t)f[1];
            r->predicted_epoch = (int64_t)f[2];
            r->actual_epoch = (int64_t)f[3];
            r->start_temp_dc = (int16_t)f[4];
            r->target_c = (int8_t)f[5];
            r->est = (uint8_t)f[6];
            r->result = (uint8_t)f[7];
            r->err_min = (int8_t)f[8];
            r->lead_min = (uint16_t)f[9];
            if (f[4] < -400 || f[4] > 600 || f[5] < -40 || f[5] > 60 || f[8] < -120 || f[8] > 30 || f[9] < 0 ||
                f[9] > 65535)
                ok = false;
        }
    }
    ok = ok && at_app_warm_put(ws);
    free(ws);
    if (ok && cJSON_GetObjectItemCaseSensitive(o, "coast")) {
        at_coast_store_t cs = {0};
        double c[AT_COAST_KEEP];
        n = 0;
        ok = num_list(o, "coast", 0, AT_COAST_MAX_KEPT, c, AT_COAST_KEEP, &n);
        cs.version = AT_COAST_VERSION;
        cs.n = (uint8_t)n;
        for (int i = 0; i < n; i++) cs.rise_cc[i] = (int16_t)lround(c[i] * 100.0);
        ok = ok && at_app_coast_put(&cs);
    }
    /* early start's busy widening (1.60): optional, an older backup has none */
    const cJSON *eo = cJSON_GetObjectItemCaseSensitive(o, "early");
    if (ok && eo) {
        const cJSON *bw = cJSON_GetObjectItemCaseSensitive(eo, "busy_w");
        if (!cJSON_IsNumber(bw) || bw->valuedouble < 0 || bw->valuedouble > AT_EARLY_MAX_BUSY) {
            ok = false;
        } else {
            at_early_store_t es = {.version = AT_EARLY_VERSION, .busy_cc = (int16_t)lround(bw->valuedouble * 100.0)};
            ok = at_app_early_put(&es);
        }
    }
    return ok;
}

bool at_app_thermo_learn_restore(const cJSON *o)
{
    if (!cJSON_IsObject(o)) return false;
    at_learn_store_t *st = calloc(1, sizeof(*st));
    if (!st) return false;
    bool ok = true;
    st->version = AT_LEARN_VERSION;
    st->width = (float)num(o, "width", 0);
    int lc = name_index(str(o, "last_change"), KINDS, 3);
    st->last_change = lc > 0 ? (uint8_t)lc : 0;
    static const char *const sfx[2] = {"ext", "ctl"};
    char key[24];
    for (int s = 0; s < 2 && ok; s++) {
        at_learn_sensor_t *ss = &st->s[s];
        snprintf(key, sizeof(key), "window_%s", sfx[s]);
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(o, key), *it;
        cJSON_ArrayForEach(it, w)
        {
            if (ss->n >= AT_LEARN_WIN) break;
            int vote = name_index(str(it, "vote"), VOTES, 3);
            if (vote < 0) ok = false;
            ss->v[ss->n].need = (float)num(it, "need", 0);
            ss->v[ss->n].rate = (float)num(it, "rate", 0);
            ss->v[ss->n].vote = (uint8_t)(vote < 0 ? 0 : vote);
            ss->n++;
        }
        snprintf(key, sizeof(key), "warmup_%s", sfx[s]);
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(o, key))
        {
            if (ss->nwarm >= AT_LEARN_MED) break;
            if (!cJSON_IsNumber(it) || it->valuedouble < 0 || it->valuedouble > 65535) ok = false;
            else ss->warm_s[ss->nwarm++] = (uint16_t)it->valuedouble;
        }
        snprintf(key, sizeof(key), "cooling_%s", sfx[s]);
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(o, key))
        {
            if (ss->ncool >= AT_LEARN_MED) break;
            if (!cJSON_IsNumber(it) || it->valuedouble < 0 || it->valuedouble > 7 * 86400) ok = false;
            else ss->cool_s[ss->ncool++] = (uint32_t)it->valuedouble;
        }
        snprintf(key, sizeof(key), "limit_%s", sfx[s]);
        int lim = name_index(str(o, key), LIMITS, 3);
        if (lim < 0) ok = false;
        ss->limit = (uint8_t)(lim < 0 ? 0 : lim);
    }
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(o, "history"), *it;
    cJSON_ArrayForEach(it, h)
    {
        if (st->nhist >= AT_LEARN_HIST) break;
        at_learn_event_t *e = &st->hist[st->nhist];
        int kind = name_index(str(it, "kind"), KINDS, 6);
        int sensor = name_index(str(it, "sensor"), SENSORS, 2);
        int lim = name_index(str(it, "limit"), LIMITS, 3);
        if (kind <= 0 || sensor < 0 || lim < 0) {
            ok = false;
            break;
        }
        e->epoch = (int64_t)num(it, "at", 0);
        e->kind = (uint8_t)kind;
        e->sensor = (uint8_t)sensor;
        e->limit = (uint8_t)lim;
        e->old_w = (float)num(it, "old", 0);
        e->new_w = (float)num(it, "new", 0);
        e->raw_w = (float)num(it, "worked_out", 0);
        e->work_w = (float)num(it, "working", 0);
        e->rate1 = (float)num(it, "rate1", 0);
        e->rate2 = (float)num(it, "rate2", 0);
        st->nhist++;
    }
    ok = ok && at_app_learn_put(st);
    free(st);
    if (ok) ok = restore_warm(o);
    if (ok) ok = restore_heater_logs(o);
    return ok;
}
