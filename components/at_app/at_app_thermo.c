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
    return ok;
}
