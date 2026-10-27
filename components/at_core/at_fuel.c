#include "at_fuel.h"

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

at_fuel_level_t at_fuel_tick(at_fuel_totals_t *t, at_fuel_log_t *log, at_fuel_run_t *run, const at_fuel_cfg_t *cfg,
                             uint8_t state, uint8_t substate, bool burner_active, double dt_s, int32_t day)
{
    at_fuel_level_t level = at_fuel_level(state, substate, burner_active);
    bool started = run->have_state && state == 2 && run->last_state != 2 && run->last_state != 3;
    run->have_state = true;
    run->last_state = state;
    if (dt_s < 0) dt_s = 0;

    double litres = at_fuel_rate_now(cfg, level) * dt_s / 3600.0;
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
