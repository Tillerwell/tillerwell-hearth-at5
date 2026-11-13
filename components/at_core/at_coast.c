#include "at_coast.h"

#include <math.h>
#include <string.h>

void at_coast_init(at_coast_t *c)
{
    memset(c, 0, sizeof(*c));
    c->st.version = AT_COAST_VERSION;
}

bool at_coast_restore(at_coast_t *c, const at_coast_store_t *st)
{
    if (st->version != AT_COAST_VERSION || st->n > AT_COAST_KEEP) return false;
    for (int i = 0; i < st->n; i++) {
        if (st->rise_cc[i] < 0 || st->rise_cc[i] > (int16_t)(AT_COAST_MAX_KEPT * 100.0f + 0.5f)) return false;
    }
    c->st = *st;
    c->dirty = false;
    return true;
}

void at_coast_reset(at_coast_t *c)
{
    at_coast_init(c);
    c->dirty = true;
}

static void keep(at_coast_t *c, float rise)
{
    if (rise < 0) rise = 0;                   /* the room fell straight away: no rise, which is worth knowing */
    if (rise > AT_COAST_MAX_KEPT) return;
    if (c->st.n == AT_COAST_KEEP) {
        memmove(&c->st.rise_cc[0], &c->st.rise_cc[1], (AT_COAST_KEEP - 1) * sizeof(c->st.rise_cc[0]));
        c->st.n--;
    }
    c->st.rise_cc[c->st.n++] = (int16_t)lroundf(rise * 100.0f);
    c->dirty = true;
}

void at_coast_stop(at_coast_t *c, uint32_t now_ms, float temp, bool ext)
{
    c->on = ext;
    c->stop_ms = now_ms;
    c->stop_temp = c->peak = temp;
}

void at_coast_sample(at_coast_t *c, uint32_t now_ms, float temp, bool ext)
{
    if (!c->on) return;
    if (!ext || now_ms - c->stop_ms > AT_COAST_WATCH_MS) {
        c->on = false;
        return;
    }
    if (temp > c->peak) c->peak = temp;
    if (temp <= c->peak - AT_COAST_FALL + 1e-3f) {
        c->on = false;
        keep(c, c->peak - c->stop_temp);
    }
}

void at_coast_cancel(at_coast_t *c)
{
    c->on = false;
}

bool at_coast_typical(const at_coast_t *c, float *rise)
{
    int n = c->st.n;
    if (n < AT_COAST_MIN_LEARN) return false;
    int16_t v[AT_COAST_KEEP];
    memcpy(v, c->st.rise_cc, n * sizeof(v[0]));
    for (int i = 1; i < n; i++) {
        int16_t x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
    float m = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
    *rise = m / 100.0f;
    return true;
}

float at_coast_lead(const at_coast_t *c, float work_width)
{
    float r;
    if (!at_coast_typical(c, &r)) return 0;
    if (r > AT_COAST_MAX) r = AT_COAST_MAX;
    if (r > work_width / 2) r = work_width / 2;
    if (r < 0) r = 0;
    return floorf(r * 10.0f + 1e-3f) / 10.0f;   /* 0.1 steps, rounded down: stopping a little late is the safe side */
}
