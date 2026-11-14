#include "at_fuel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Autoterm Flow 5 spec-sheet figures (about 0.15 to 0.62 l/h across its range), not measured on this
 * heater: the owner calibrates with `factor` against a real fill-up. Starting covers the glow-plug and
 * chamber-heating phases, which burn at roughly the low rate. */
void at_fuel_cfg_defaults(at_fuel_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->rate_lph[AT_FUEL_STARTING] = 0.15f;
    cfg->rate_lph[AT_FUEL_LOW] = 0.15f;
    cfg->rate_lph[AT_FUEL_MIDDLE] = 0.35f;
    cfg->rate_lph[AT_FUEL_HIGH] = 0.62f;
    cfg->factor = 1.0f;
    cfg->price = 0.0f;
}

bool at_fuel_cfg_valid(const at_fuel_cfg_t *cfg)
{
    for (int i = AT_FUEL_STARTING; i < AT_FUEL_LEVELS; i++) {
        if (!(cfg->rate_lph[i] >= 0.0f && cfg->rate_lph[i] <= 5.0f)) return false;
    }
    return cfg->factor >= 0.2f && cfg->factor <= 5.0f && cfg->price >= 0.0f && cfg->price <= 100.0f;
}

void at_fuel_totals_init(at_fuel_totals_t *t)
{
    memset(t, 0, sizeof(*t));
    t->version = AT_FUEL_TOTALS_VERSION;
}

void at_fuel_log_init(at_fuel_log_t *log)
{
    memset(log, 0, sizeof(*log));
}

at_fuel_level_t at_fuel_level(uint8_t state, uint8_t substate, bool burner_active)
{
    at_fuel_level_t l = AT_FUEL_OFF;
    if (state == 3) {
        /* the vendor profile's names: 0 low, 2 High, 8 middle; 4-7 blowing, waiting, pump only */
        if (substate == 0) l = AT_FUEL_LOW;
        else if (substate == 2) l = AT_FUEL_HIGH;
        else if (substate == 8) l = AT_FUEL_MIDDLE;
    } else if (state == 2) {
        /* Ignition 1 and 2, combustion chamber heating */
        if (substate == 3 || substate == 4 || substate == 6) l = AT_FUEL_STARTING;
    }
    if (l == AT_FUEL_OFF && burner_active && (state == 2 || state == 3)) {
        l = AT_FUEL_MIDDLE; /* burning at a level we have no name for: count it mid-range */
    }
    return l;
}

const char *at_fuel_level_name(at_fuel_level_t level)
{
    switch (level) {
    case AT_FUEL_STARTING: return "starting";
    case AT_FUEL_LOW: return "low";
    case AT_FUEL_MIDDLE: return "middle";
    case AT_FUEL_HIGH: return "high";
    default: return "off";
    }
}

float at_fuel_rate_now(const at_fuel_cfg_t *cfg, at_fuel_level_t level)
{
    if (level <= AT_FUEL_OFF || level >= AT_FUEL_LEVELS) return 0.0f;
    return cfg->rate_lph[level] * cfg->factor;
}

static at_fuel_day_t *log_today(at_fuel_log_t *log, at_fuel_run_t *run, int32_t day)
{
    if (log->count && log->d[log->head].day == day) {
        return &log->d[log->head];
    }
    if (log->count && log->d[log->head].day > day) {
        return NULL; /* the clock went backwards: leave the log alone */
    }
    if (log->count) {
        log->head = (uint16_t)((log->head + 1) % AT_FUEL_DAYS);
    }
    if (log->count < AT_FUEL_DAYS) log->count++;
    at_fuel_day_t *e = &log->d[log->head];
    memset(e, 0, sizeof(*e));
    e->day = day;
    run->day_ml_frac = run->day_cost_frac = run->day_burn_s_frac = 0;
    return e;
}

void at_fuel_pump_init(at_fuel_pump_t *p)
{
    memset(p, 0, sizeof(*p));
    p->version = AT_FUEL_PUMP_VERSION;
}

bool at_fuel_pump_valid(const at_fuel_pump_t *p)
{
    return p->version == AT_FUEL_PUMP_VERSION && (p->ml_per_pulse == 0.0f || (p->ml_per_pulse >= 0.005f && p->ml_per_pulse <= 0.2f)) &&
           p->pulses >= 0.0;
}

bool at_fuel_pump_set_ml(at_fuel_pump_t *p, float ml_per_pulse)
{
    if (!(ml_per_pulse == 0.0f || (ml_per_pulse >= 0.005f && ml_per_pulse <= 0.2f))) return false;
    p->ml_per_pulse = ml_per_pulse;
    return true;
}

void at_fuel_pump_reset_count(at_fuel_pump_t *p)
{
    p->pulses = 0;
}

bool at_fuel_pump_calibrate(at_fuel_pump_t *p, float litres)
{
    if (p->pulses < AT_FUEL_PUMP_MIN_PULSES || !(litres > 0.0f)) return false;
    return at_fuel_pump_set_ml(p, (float)(litres * 1000.0 / p->pulses)) && p->ml_per_pulse != 0.0f;
}

float at_fuel_pump_rate_lph(const at_fuel_pump_t *p, float pump_hz)
{
    if (!p || p->ml_per_pulse <= 0.0f || !(pump_hz > 0.0f)) return 0.0f;
    return pump_hz * p->ml_per_pulse * 3.6f; /* ml/s -> l/h */
}

at_fuel_level_t at_fuel_tick(at_fuel_totals_t *t, at_fuel_log_t *log, at_fuel_run_t *run, const at_fuel_cfg_t *cfg,
                             uint8_t state, uint8_t substate, bool burner_active, double dt_s, int32_t day)
{
    return at_fuel_tick_pump(t, log, run, cfg, NULL, -1.0f, state, substate, burner_active, dt_s, day);
}

at_fuel_level_t at_fuel_tick_pump(at_fuel_totals_t *t, at_fuel_log_t *log, at_fuel_run_t *run, const at_fuel_cfg_t *cfg,
                                  at_fuel_pump_t *pump, float pump_hz, uint8_t state, uint8_t substate,
                                  bool burner_active, double dt_s, int32_t day)
{
    at_fuel_level_t level = at_fuel_level(state, substate, burner_active);
    bool started = run->have_state && state == 2 && run->last_state != 2 && run->last_state != 3;
    run->have_state = true;
    run->last_state = state;
    if (dt_s < 0) dt_s = 0;

    double litres = at_fuel_rate_now(cfg, level) * dt_s / 3600.0;
    if (pump && pump_hz >= 0.0f) {
        double pulses = pump_hz * dt_s;
        pump->pulses += pulses;
        if (pump->ml_per_pulse > 0.0f && !(level == AT_FUEL_STARTING && pump_hz == 0.0f)) {
            litres = pulses * pump->ml_per_pulse / 1000.0;
        }
    }
    double cost = litres * cfg->price;
    t->litres += litres;
    t->cost += cost;
    if (started) t->starts++;
    if (level != AT_FUEL_OFF) {
        run->burn_s_frac += dt_s;
        uint32_t whole = (uint32_t)run->burn_s_frac;
        t->level_s[level] += whole;
        run->burn_s_frac -= whole;
    }

    if (day >= 0 && (litres > 0 || started || level != AT_FUEL_OFF)) {
        at_fuel_day_t *e = log_today(log, run, day);
        if (e) {
            run->day_ml_frac += litres * 1000.0;
            run->day_cost_frac += cost * 1000.0;
            uint32_t ml = (uint32_t)run->day_ml_frac, cm = (uint32_t)run->day_cost_frac;
            e->ml += ml;
            e->cost_m += cm;
            run->day_ml_frac -= ml;
            run->day_cost_frac -= cm;
            if (level != AT_FUEL_OFF) {
                run->day_burn_s_frac += dt_s;
                uint32_t mins = (uint32_t)(run->day_burn_s_frac / 60.0);
                e->burn_min = (uint16_t)(e->burn_min + mins);
                run->day_burn_s_frac -= mins * 60.0;
            }
            if (started) e->starts++;
        }
    }
    return level;
}

void at_fuel_detail_init(at_fuel_detail_t *d)
{
    memset(d, 0, sizeof(*d));
    d->version = AT_FUEL_DETAIL_VERSION;
}

bool at_fuel_detail_valid(const at_fuel_detail_t *d)
{
    return d->version == AT_FUEL_DETAIL_VERSION && d->count <= AT_FUEL_DETAIL_DAYS && d->head < AT_FUEL_DETAIL_DAYS;
}

const at_fuel_detail_day_t *at_fuel_detail_find(const at_fuel_detail_t *d, int32_t day)
{
    for (int i = 0; i < d->count; i++) {
        const at_fuel_detail_day_t *e = &d->d[(d->head + AT_FUEL_DETAIL_DAYS - i) % AT_FUEL_DETAIL_DAYS];
        if (e->day == day) return e;
        if (e->day < day) break;
    }
    return NULL;
}

void at_fuel_detail_add(at_fuel_detail_t *d, at_fuel_detail_run_t *run, int32_t day, int hour, at_fuel_level_t level,
                        double dt_s, double litres)
{
    if (level <= AT_FUEL_OFF || level >= AT_FUEL_LEVELS || day < 0 || hour < 0 || hour > 23 || dt_s < 0) return;
    at_fuel_detail_day_t *e;
    if (d->count && d->d[d->head].day == day) {
        e = &d->d[d->head];
    } else {
        if (d->count && d->d[d->head].day > day) return; /* the clock went backwards: leave it alone */
        if (d->count) d->head = (uint16_t)((d->head + 1) % AT_FUEL_DETAIL_DAYS);
        if (d->count < AT_FUEL_DETAIL_DAYS) d->count++;
        e = &d->d[d->head];
        memset(e, 0, sizeof(*e));
        e->day = day;
    }
    if (run->day != day || run->hour != hour) {
        memset(run, 0, sizeof(*run));
        run->day = day;
        run->hour = hour;
    }
    int i = level - AT_FUEL_STARTING;
    run->s[i] += dt_s;
    unsigned mins = (unsigned)(run->s[i] / 60.0);
    if (mins) {
        unsigned m = e->min[hour][i] + mins;
        e->min[hour][i] = (uint8_t)(m > 60 ? 60 : m);
        run->s[i] -= mins * 60.0;
    }
    run->ml[i] += litres * 1000.0;
    unsigned ml = (unsigned)run->ml[i];
    if (ml) {
        unsigned v = e->ml[i] + ml;
        e->ml[i] = (uint16_t)(v > 65535 ? 65535 : v);
        run->ml[i] -= ml;
    }
}

int at_fuel_log_reprice(at_fuel_log_t *log, at_fuel_totals_t *t, at_fuel_run_t *run, int32_t from_day, float per_litre)
{
    int n = 0;
    int64_t delta_m = 0;
    for (int i = 0; i < log->count; i++) {
        at_fuel_day_t *e = &log->d[(log->head + AT_FUEL_DAYS - i) % AT_FUEL_DAYS];
        if (e->day < from_day) break; /* newest first: everything after this is older */
        double c = (double)e->ml * per_litre + 0.5;
        uint32_t cost_m = c >= 4294967295.0 ? UINT32_MAX : (uint32_t)c;
        delta_m += (int64_t)cost_m - (int64_t)e->cost_m;
        e->cost_m = cost_m;
        if (i == 0 && run) run->day_cost_frac = 0;
        n++;
    }
    t->cost += delta_m / 1000.0;
    if (t->cost < 0) t->cost = 0;
    return n;
}

const at_fuel_day_t *at_fuel_log_at(const at_fuel_log_t *log, int i)
{
    if (i < 0 || i >= log->count) return NULL;
    return &log->d[(log->head + AT_FUEL_DAYS - i) % AT_FUEL_DAYS];
}

const at_fuel_day_t *at_fuel_log_find(const at_fuel_log_t *log, int32_t day)
{
    for (int i = 0; i < log->count; i++) {
        const at_fuel_day_t *e = at_fuel_log_at(log, i);
        if (e->day == day) return e;
        if (e->day < day) break;
    }
    return NULL;
}

void at_fuel_tank_init(at_fuel_tank_t *t)
{
    memset(t, 0, sizeof(*t));
    t->version = AT_FUEL_TANK_VERSION;
}

bool at_fuel_tank_set_capacity(at_fuel_tank_t *t, float litres)
{
    if (!(litres >= 1.0f && litres <= AT_FUEL_TANK_MAX_L)) return false;
    t->capacity_l = litres;
    if (t->have_level && t->level_l > litres) t->level_l = litres;
    return true;
}

bool at_fuel_tank_set_level(at_fuel_tank_t *t, float litres, double used_total, int64_t epoch)
{
    float max = t->capacity_l > 0 ? t->capacity_l : AT_FUEL_TANK_MAX_L;
    if (!(litres >= 0.0f && litres <= max)) return false;
    t->have_level = true;
    t->level_l = litres;
    t->used_at_set = used_total;
    t->set_epoch = epoch;
    return true;
}

bool at_fuel_tank_remaining(const at_fuel_tank_t *t, double used_total, float *litres)
{
    if (!t->have_level) return false;
    double left = t->level_l - (used_total - t->used_at_set);
    if (left < 0) left = 0;
    if (t->capacity_l > 0 && left > t->capacity_l) left = t->capacity_l;
    *litres = (float)left;
    return true;
}

/* ---- the cloud backup's copy (1.65) ---- */

static char min_char(unsigned m)
{
    if (m > 60) m = 60;
    if (m < 10) return (char)('0' + m);
    if (m < 36) return (char)('a' + m - 10);
    return (char)('A' + m - 36);
}

static int char_min(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Y') return c - 'A' + 36;
    return -1;
}

size_t at_fuel_log_encode(const at_fuel_log_t *log, char *out, size_t cap)
{
    if (!cap) return 0;
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < log->count; i++) {
        const at_fuel_day_t *e = at_fuel_log_at(log, i);
        int w = snprintf(out + n, cap - n, "%ld:%lu,%lu,%u,%u;", (long)e->day, (unsigned long)e->ml, (unsigned long)e->cost_m,
                         (unsigned)e->burn_min, (unsigned)e->starts);
        if (w < 0 || (size_t)w >= cap - n) { out[0] = 0; return 0; }
        n += (size_t)w;
    }
    return n;
}

size_t at_fuel_detail_encode(const at_fuel_detail_t *d, char *out, size_t cap)
{
    if (!cap) return 0;
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < d->count; i++) {
        const at_fuel_detail_day_t *e = &d->d[(d->head + AT_FUEL_DETAIL_DAYS - i) % AT_FUEL_DETAIL_DAYS];
        int w = snprintf(out + n, cap - n, "%ld:%u,%u,%u,%u:", (long)e->day, e->ml[0], e->ml[1], e->ml[2], e->ml[3]);
        if (w < 0 || (size_t)w + 24 * AT_FUEL_BURN_LEVELS + 2 > cap - n) { out[0] = 0; return 0; }
        n += (size_t)w;
        for (int h = 0; h < 24; h++)
            for (int l = 0; l < AT_FUEL_BURN_LEVELS; l++) out[n++] = min_char(e->min[h][l]);
        out[n++] = ';';
        out[n] = 0;
    }
    return n;
}

/* Reads an unsigned number up to `max`; advances *p. */
static bool read_num(const char **p, long min, long max, long *v)
{
    char *end;
    long x = strtol(*p, &end, 10);
    if (end == *p || x < min || x > max) return false;
    *v = x;
    *p = end;
    return true;
}

static bool expect(const char **p, char c)
{
    if (**p != c) return false;
    (*p)++;
    return true;
}

static int cmp_day(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return x < y ? -1 : x > y;
}

int at_fuel_log_merge_text(at_fuel_log_t *log, const char *text)
{
    if (!text) return -1;
    /* everything the box has, plus the restored days it lacks, sorted oldest first; the newest AT_FUEL_DAYS kept */
    at_fuel_day_t *all = malloc(sizeof(at_fuel_day_t) * AT_FUEL_DAYS * 2);
    if (!all) return -1;
    int n = 0, added = 0;
    for (int i = 0; i < log->count; i++) all[n++] = *at_fuel_log_at(log, i);
    int own = n;
    const char *p = text;
    while (*p) {
        long day, ml, cm, burn, starts;
        if (!read_num(&p, 0, 200000, &day) || !expect(&p, ':') || !read_num(&p, 0, 4000000000L, &ml) || !expect(&p, ',') ||
            !read_num(&p, 0, 4000000000L, &cm) || !expect(&p, ',') || !read_num(&p, 0, 65535, &burn) || !expect(&p, ',') ||
            !read_num(&p, 0, 65535, &starts) || !expect(&p, ';')) {
            free(all);
            return -1;
        }
        bool have = false;
        for (int i = 0; i < own; i++) if (all[i].day == day) { have = true; break; }
        for (int i = own; i < n && !have; i++) if (all[i].day == day) have = true; /* a day twice in the text */
        if (have || n >= AT_FUEL_DAYS * 2) continue;
        at_fuel_day_t e = { .day = (int32_t)day, .ml = (uint32_t)ml, .cost_m = (uint32_t)cm, .burn_min = (uint16_t)burn, .starts = (uint16_t)starts };
        all[n++] = e;
        added++;
    }
    qsort(all, (size_t)n, sizeof(at_fuel_day_t), cmp_day); /* day is the first field */
    int first = n > AT_FUEL_DAYS ? n - AT_FUEL_DAYS : 0;
    at_fuel_log_init(log);
    for (int i = first; i < n; i++) log->d[i - first] = all[i];
    log->count = (uint16_t)(n - first);
    log->head = (uint16_t)(log->count ? log->count - 1 : 0);
    free(all);
    return added;
}

int at_fuel_detail_merge_text(at_fuel_detail_t *d, const char *text)
{
    if (!text) return -1;
    at_fuel_detail_day_t *all = malloc(sizeof(at_fuel_detail_day_t) * AT_FUEL_DETAIL_DAYS * 2);
    if (!all) return -1;
    int n = 0, added = 0;
    for (int i = 0; i < d->count; i++) all[n++] = d->d[(d->head + AT_FUEL_DETAIL_DAYS - i) % AT_FUEL_DETAIL_DAYS];
    int own = n;
    const char *p = text;
    while (*p) {
        long day, ml[AT_FUEL_BURN_LEVELS];
        bool ok = read_num(&p, 0, 200000, &day) && expect(&p, ':');
        for (int l = 0; ok && l < AT_FUEL_BURN_LEVELS; l++) ok = read_num(&p, 0, 65535, &ml[l]) && expect(&p, l < AT_FUEL_BURN_LEVELS - 1 ? ',' : ':');
        at_fuel_detail_day_t e;
        memset(&e, 0, sizeof(e));
        for (int h = 0; ok && h < 24; h++)
            for (int l = 0; ok && l < AT_FUEL_BURN_LEVELS; l++) {
                int m = char_min(*p);
                ok = m >= 0;
                if (ok) { e.min[h][l] = (uint8_t)m; p++; }
            }
        ok = ok && expect(&p, ';');
        if (!ok) { free(all); return -1; }
        bool have = false;
        for (int i = 0; i < n; i++) if (all[i].day == day) { have = true; break; }
        if (have || n >= AT_FUEL_DETAIL_DAYS * 2) continue;
        e.day = (int32_t)day;
        for (int l = 0; l < AT_FUEL_BURN_LEVELS; l++) e.ml[l] = (uint16_t)ml[l];
        all[n++] = e;
        added++;
    }
    (void)own;
    qsort(all, (size_t)n, sizeof(at_fuel_detail_day_t), cmp_day);
    int first = n > AT_FUEL_DETAIL_DAYS ? n - AT_FUEL_DETAIL_DAYS : 0;
    at_fuel_detail_init(d);
    for (int i = first; i < n; i++) d->d[i - first] = all[i];
    d->count = (uint16_t)(n - first);
    d->head = (uint16_t)(d->count ? d->count - 1 : 0);
    free(all);
    return added;
}

bool at_fuel_totals_merge(at_fuel_totals_t *own, const at_fuel_totals_t *r)
{
    if (!(r->litres > own->litres)) return false;
    own->litres += r->litres;
    own->cost += r->cost;
    for (int i = 0; i < AT_FUEL_LEVELS; i++) own->level_s[i] += r->level_s[i];
    own->starts += r->starts;
    return true;
}
