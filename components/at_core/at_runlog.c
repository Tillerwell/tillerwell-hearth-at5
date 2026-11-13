#include "at_runlog.h"

#include <stdlib.h>
#include <string.h>

static bool finite_f(float f) { return f == f && f > -1e6f && f < 1e6f; }

void at_runlog_save(const at_learn_t *L, at_runlog_store_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = AT_RUNLOG_VERSION;
    out->size = (uint16_t)sizeof(at_learn_run_t);
    int n = L->nruns < 0 ? 0 : (L->nruns > AT_LEARN_RUNS ? AT_LEARN_RUNS : L->nruns);
    out->n = (uint8_t)n;
    memcpy(out->r, L->runs, sizeof(L->runs[0]) * (size_t)n);
}

bool at_runlog_changed(const at_learn_t *L, const at_runlog_store_t *saved)
{
    /* the whole log (1.3 kB once a tick): once full a new run shifts every entry, and two runs can be byte for byte
     * the same (no clock, no flame) */
    if (L->nruns != saved->n) return true;
    return L->nruns > 0 && memcmp(L->runs, saved->r, sizeof(L->runs[0]) * (size_t)L->nruns) != 0;
}

bool at_runlog_run_ok(const at_learn_run_t *r)
{
    if (r->start_epoch < 0 || r->sensor > AT_SENS_CTL || r->end > AT_END_THERMO_OFF || r->why > AT_WHY_COLD_SPELL ||
        r->vote > AT_VOTE_SLOW)
        return false;
    return finite_f(r->start_temp) && finite_f(r->peak_temp) && finite_f(r->rate) && finite_f(r->need) &&
           finite_f(r->climb_min) && finite_f(r->dip);
}

bool at_runlog_merge(at_learn_t *L, const at_learn_run_t *in, int n)
{
    if (n < 0) return false;
    for (int i = 0; i < n; i++) {
        if (!at_runlog_run_ok(&in[i])) return false;
    }
    at_learn_run_t *all = malloc(sizeof(at_learn_run_t) * 2 * AT_LEARN_RUNS);   /* 2.6 kB: not on a task's stack */
    if (!all) return false;
    int m = 0;
    for (int i = 0; i < L->nruns && i < AT_LEARN_RUNS; i++) all[m++] = L->runs[i];
    const int own = m;
    /* only the newest AT_LEARN_RUNS incoming can matter */
    for (int i = n > AT_LEARN_RUNS ? n - AT_LEARN_RUNS : 0; i < n; i++) {
        bool dup = false;
        for (int k = 0; k < own && !dup; k++) dup = in[i].start_epoch > 0 && all[k].start_epoch == in[i].start_epoch;
        if (!dup) all[m++] = in[i];
    }
    /* oldest first, stable (insertion sort keeps the box's own order for equal times) */
    for (int i = 1; i < m; i++) {
        at_learn_run_t x = all[i];
        int j = i - 1;
        while (j >= 0 && all[j].start_epoch > x.start_epoch) {
            all[j + 1] = all[j];
            j--;
        }
        all[j + 1] = x;
    }
    int from = m > AT_LEARN_RUNS ? m - AT_LEARN_RUNS : 0;
    L->nruns = m - from;
    memcpy(L->runs, &all[from], sizeof(all[0]) * (size_t)L->nruns);
    free(all);
    return true;
}

bool at_runlog_restore(at_learn_t *L, const at_runlog_store_t *st)
{
    if (st->version != AT_RUNLOG_VERSION || st->size != sizeof(at_learn_run_t) || st->n > AT_LEARN_RUNS) return false;
    for (int i = 0; i < st->n; i++) {
        if (!at_runlog_run_ok(&st->r[i])) return false;
    }
    memcpy(L->runs, st->r, sizeof(st->r[0]) * st->n);
    L->nruns = st->n;
    return true;
}
