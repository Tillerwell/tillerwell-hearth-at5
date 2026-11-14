#include "at_battery.h"

#include <string.h>

/* Watts by phase for a 12 V Flow 5D. Starting, High and the start's glow-plug energy are Autoterm's figures (operation
 * manual, Characteristics); Low and Middle sit between the water pump alone and High, as the fan and fuel pump slow
 * down. A measured start (2026-11-08: about +9 A for 2 min, then about +3 A) fits these. */
static const float k_watts[AT_BATT_PHASES] = {
    [AT_BATT_OFF] = 0.0f,
    [AT_BATT_STARTING] = 35.0f,
    [AT_BATT_LOW] = 18.0f,
    [AT_BATT_MIDDLE] = 28.0f,
    [AT_BATT_HIGH] = 42.0f,
    [AT_BATT_PUMP] = 18.0f,
    [AT_BATT_FAN] = 30.0f,
};

void at_batt_totals_init(at_batt_totals_t *t)
{
    memset(t, 0, sizeof(*t));
    t->version = AT_BATT_TOTALS_VERSION;
    t->day = -1;
}

bool at_batt_totals_valid(const at_batt_totals_t *t)
{
    return t->version == AT_BATT_TOTALS_VERSION && t->ah >= 0 && t->wh >= 0 && t->ah_day >= 0 && t->ah_run >= 0;
}

bool at_batt_totals_merge(at_batt_totals_t *own, double ah, double wh, int32_t day, double ah_day)
{
    if (!(ah > own->ah) || !(ah >= 0) || !(wh >= 0)) return false;
    own->ah += ah;
    own->wh += wh;
    if (day >= 0 && ah_day >= 0) {
        if (own->day == day) own->ah_day += ah_day;
        else if (own->day < day) { own->day = day; own->ah_day = ah_day; }
    }
    return true;
}

bool at_batt_factor_valid(float factor)
{
    return factor >= AT_BATT_FACTOR_MIN && factor <= AT_BATT_FACTOR_MAX;
}

at_batt_phase_t at_batt_phase(uint8_t state, uint8_t substate, bool burner_active)
{
    switch (state) {
    case 2:
        /* 1 glow plug warming up, 2 preparation, 3-4 ignition, 6 chamber heating; 0, 5, 7 blowing */
        if (substate == 0 || substate == 5 || substate == 7) return AT_BATT_FAN;
        return AT_BATT_STARTING;
    case 3:
        switch (substate) {
        case 0: return AT_BATT_LOW;
        case 2: return AT_BATT_HIGH;
        case 8: return AT_BATT_MIDDLE;
        case 5: /* waiting: the water pump keeps circulating */
        case 7: return AT_BATT_PUMP;
        case 4:
        case 6: return AT_BATT_FAN;
        default: return burner_active ? AT_BATT_MIDDLE : AT_BATT_FAN;
        }
    case 4:
        return AT_BATT_FAN;
    default:
        return burner_active ? AT_BATT_MIDDLE : AT_BATT_OFF;
    }
}

const char *at_batt_phase_name(at_batt_phase_t phase)
{
    switch (phase) {
    case AT_BATT_STARTING: return "starting";
    case AT_BATT_LOW: return "low";
    case AT_BATT_MIDDLE: return "middle";
    case AT_BATT_HIGH: return "high";
    case AT_BATT_PUMP: return "pump";
    case AT_BATT_FAN: return "fan";
    default: return "off";
    }
}

float at_batt_watts(at_batt_phase_t phase)
{
    if (phase <= AT_BATT_OFF || phase >= AT_BATT_PHASES) return 0.0f;
    return k_watts[phase];
}

float at_batt_volts(float supply_v)
{
    return supply_v >= 9.0f && supply_v <= 32.0f ? supply_v : AT_BATT_NOMINAL_V;
}

double at_batt_ah_today(const at_batt_totals_t *t, int32_t day)
{
    return day >= 0 && t->day == day ? t->ah_day : 0.0;
}

at_batt_phase_t at_batt_tick(at_batt_totals_t *t, at_batt_run_t *run, float factor, uint8_t state, uint8_t substate,
                             bool burner_active, float supply_v, double dt_s, int32_t day)
{
    at_batt_phase_t phase = at_batt_phase(state, substate, burner_active);
    bool started = run->have_state && state == 2 && run->last_state != 2 && run->last_state != 3;
    run->have_state = true;
    run->last_state = state;
    if (!(dt_s > 0)) dt_s = 0;
    if (!at_batt_factor_valid(factor)) factor = 1.0f;

    double wh = at_batt_watts(phase) * dt_s / 3600.0;
    if (started) {
        /* the glow plug's extra draw over the starting phase's own, once per start */
        wh += (AT_BATT_START_W - k_watts[AT_BATT_STARTING]) * AT_BATT_START_S / 3600.0;
        t->ah_run = 0;
    }
    wh *= factor;
    double ah = wh / at_batt_volts(supply_v);
    t->wh += wh;
    t->ah += ah;
    t->ah_run += ah;
    if (day >= 0 && (ah > 0 || t->day == day)) {
        if (t->day != day) {
            if (day < t->day) return phase; /* the clock went backwards: leave today's alone */
            t->day = day;
            t->ah_day = 0;
        }
        t->ah_day += ah;
    }
    return phase;
}
