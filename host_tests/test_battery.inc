/* Battery-use estimate (1.62, at_battery.c). Included from test_main.c. */
#include "at_battery.h"

static void test_battery(void)
{
    at_batt_totals_t t;
    at_batt_run_t run = {0};
    at_batt_totals_init(&t);
    CHECK(at_batt_totals_valid(&t) && t.day == -1);

    /* phases from state/substate */
    CHECK(at_batt_phase(0, 1, false) == AT_BATT_OFF);
    CHECK(at_batt_phase(1, 1, false) == AT_BATT_OFF);
    CHECK(at_batt_phase(2, 1, false) == AT_BATT_STARTING);
    CHECK(at_batt_phase(2, 6, false) == AT_BATT_STARTING);
    CHECK(at_batt_phase(2, 5, false) == AT_BATT_FAN);
    CHECK(at_batt_phase(3, 0, true) == AT_BATT_LOW);
    CHECK(at_batt_phase(3, 8, true) == AT_BATT_MIDDLE);
    CHECK(at_batt_phase(3, 2, true) == AT_BATT_HIGH);
    CHECK(at_batt_phase(3, 7, false) == AT_BATT_PUMP);
    CHECK(at_batt_phase(3, 5, false) == AT_BATT_PUMP);
    CHECK(at_batt_phase(3, 9, true) == AT_BATT_MIDDLE);
    CHECK(at_batt_phase(4, 0, false) == AT_BATT_FAN);
    CHECK(at_batt_watts(AT_BATT_HIGH) == 42.0f && at_batt_watts(AT_BATT_OFF) == 0.0f);

    /* voltage: believable readings are used, anything else is the nominal 12 V */
    CHECK(at_batt_volts(12.6f) == 12.6f && at_batt_volts(25.0f) == 25.0f);
    CHECK(at_batt_volts(0.0f) == 12.0f && at_batt_volts(40.0f) == 12.0f);

    /* idle counts nothing, and the first tick (no previous state) is not a start */
    at_batt_tick(&t, &run, 1.0f, 0, 1, false, 12.0f, 3600, 100);
    CHECK(t.ah == 0 && at_batt_ah_today(&t, 100) == 0);

    /* a start adds the glow plug's extra energy once: (122 - 35) W x 100 s */
    double glow_wh = (122.0 - 35.0) * 100.0 / 3600.0;
    at_batt_tick(&t, &run, 1.0f, 2, 1, false, 12.0f, 0, 100);
    CHECK(fabs(t.wh - glow_wh) < 1e-9 && fabs(t.ah - glow_wh / 12.0) < 1e-9);
    at_batt_tick(&t, &run, 1.0f, 2, 6, false, 12.0f, 120, 100); /* still starting: no second glow */
    CHECK(fabs(t.wh - glow_wh - 35.0 * 120 / 3600.0) < 1e-9);

    /* an hour at High on 12.5 V */
    double before = t.ah;
    at_batt_tick(&t, &run, 1.0f, 3, 2, true, 12.5f, 3600, 100);
    CHECK(fabs(t.ah - before - 42.0 / 12.5) < 1e-9);
    CHECK(fabs(t.ah_run - t.ah) < 1e-9 && fabs(at_batt_ah_today(&t, 100) - t.ah) < 1e-9);

    /* cooldown, idle, then a new start resets the run but not today or the total */
    at_batt_tick(&t, &run, 1.0f, 4, 0, false, 12.5f, 180, 100);
    at_batt_tick(&t, &run, 1.0f, 0, 1, false, 12.5f, 60, 100);
    double total = t.ah;
    at_batt_tick(&t, &run, 1.0f, 2, 1, false, 12.0f, 0, 100);
    CHECK(fabs(t.ah_run - glow_wh / 12.0) < 1e-9 && t.ah > total);

    /* next day: today starts again from 0; the clock going back leaves today's alone */
    double day1 = at_batt_ah_today(&t, 100);
    at_batt_tick(&t, &run, 1.0f, 3, 0, true, 12.0f, 3600, 101);
    CHECK(fabs(at_batt_ah_today(&t, 101) - 18.0 / 12.0) < 1e-9 && at_batt_ah_today(&t, 100) == 0);
    at_batt_tick(&t, &run, 1.0f, 3, 0, true, 12.0f, 3600, 100);
    CHECK(fabs(at_batt_ah_today(&t, 101) - 18.0 / 12.0) < 1e-9 && day1 > 0);

    /* no clock: the total still counts */
    total = t.ah;
    at_batt_tick(&t, &run, 1.0f, 3, 0, true, 12.0f, 3600, -1);
    CHECK(fabs(t.ah - total - 1.5) < 1e-9);

    /* the factor scales it; a bad factor counts as 1; a 24 V heater counts half the amp-hours */
    total = t.ah;
    at_batt_tick(&t, &run, 0.5f, 3, 2, true, 24.0f, 3600, -1);
    CHECK(fabs(t.ah - total - 21.0 / 24.0) < 1e-9);
    total = t.ah;
    at_batt_tick(&t, &run, 99.0f, 3, 2, true, 24.0f, 3600, -1);
    CHECK(fabs(t.ah - total - 42.0 / 24.0) < 1e-9);
    CHECK(at_batt_factor_valid(1.0f) && !at_batt_factor_valid(0.1f) && !at_batt_factor_valid(6.0f));

    at_batt_totals_t bad = t;
    bad.version = 7;
    CHECK(!at_batt_totals_valid(&bad));
}
