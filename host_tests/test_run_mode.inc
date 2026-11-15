/* Switching a run between thermostat and preheat with no stop (1.68, docs/RUN_MODE.md), and Auto off while the
 * heater cools down. Included by test_main.c after test_heater_care.inc (uses its hsim). */

static void test_run_mode_bus(void)
{
    printf("run mode switch: the frames on the bus\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 3000);
    at_snapshot_t sn;

    /* the controller's preheat: its start carries the run time, a type02 changes it */
    static const uint8_t preheat[] = {0x00, 0x1e}, longer[] = {0x00, 0x46};
    panel_control(&s, AT_TYPE_START, preheat, 2);
    sim_run(&s, &c, 1000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.run_mode == AT_RUN_PREHEAT && sn.preheat_total_min == 30 && !sn.hearth_run);
    panel_control(&s, AT_TYPE_PREHEAT_DUR, longer, 2);
    sim_run(&s, &c, 1000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.preheat_total_min == 70);

    /* to thermostat: ff fb again, and the run is the Hearth's (the controller's preheat stop is held back) */
    CHECK(at_cmd_switch_to_thermostat(&c, s.now));
    sim_run(&s, &c, 3000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(s.last_start_len == 9 && s.last_start[5] == 0xFF && s.last_start[6] == 0xFB);
    CHECK(sn.run_mode == AT_RUN_THERMOSTAT && sn.preheat_total_min == 0 && sn.hearth_run);
    CHECK(count_type(&s, AT_TYPE_STOP) == 0);
    panel_control(&s, AT_TYPE_STOP, NULL, 0);
    sim_run(&s, &c, 1000);
    CHECK(s.heater_state == 2 && count_type(&s, AT_TYPE_STOP) == 0);

    /* to preheat: one type02 with the run time from the start, no stop, no start */
    int starts = count_type(&s, AT_TYPE_START);
    CHECK(at_cmd_switch_to_preheat(&c, 75, s.now));
    sim_run(&s, &c, 3000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(s.last_frame_len == 9 && s.last_frame[4] == AT_TYPE_PREHEAT_DUR && s.last_frame[5] == 0 &&
          s.last_frame[6] == 75);
    CHECK(sn.run_mode == AT_RUN_PREHEAT && sn.preheat_total_min == 75 && sn.hearth_run);
    CHECK(count_type(&s, AT_TYPE_START) == starts && count_type(&s, AT_TYPE_STOP) == 0 && s.heater_state == 2);

    /* a Hearth preheat start knows its time; a thermostat start has none */
    CHECK(at_cmd_stop(&c, s.now));
    sim_run(&s, &c, 3000);
    CHECK(at_cmd_start_preheat(&c, 45, s.now));
    sim_run(&s, &c, 4000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.preheat_total_min == 45);
    CHECK(at_cmd_set_preheat_duration(&c, 90, s.now));
    sim_run(&s, &c, 2000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.preheat_total_min == 90);
    CHECK(at_cmd_start_thermostat(&c, s.now));
    sim_run(&s, &c, 2000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.preheat_total_min == 0);
}

static void test_run_mode_ctl(void)
{
    printf("run mode switch: the thermostat loop\n");
    /* thermostat -> preheat: Auto off with no stop; nothing of Hearth's stops it (the heater does, on its time) */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.02f;
        hsim_arm(&m);
        CHECK(m.l.starts == 1);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 10 * MIN_MS);
        at_snapshot_t s = hsim_snap(&m);
        CHECK(at_ctl_can_switch(&t, &s));
        at_ctl_to_preheat(&t, m.now, 600);
        CHECK(!t.autot.enabled && t.run.owner == AT_OWNER_OTHER && t.held == AT_HOLD_NONE);
        CHECK(m.l.stops == 0);
        /* past the top and past Auto's ceiling: no stop from Hearth */
        m.rate_on = 0.2f;
        hsim_run(&m, 60 * MIN_MS);
        CHECK(m.room > 26.5f && m.l.stops == 0 && m.state == 3);
        /* the heater stops itself when its time runs out: not a thermostat run, so not learned from */
        m.state = 4;
        m.since = m.now;
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.learn.nruns > 0 && t.learn.runs[t.learn.nruns - 1].why == AT_WHY_NOT_THERMOSTAT);
        /* Auto stays off: the room cools below the range and nothing starts */
        m.rate_off = 0.5f;
        hsim_run(&m, 60 * MIN_MS);
        CHECK(m.l.starts == 1 && m.state == 0);
    }
    /* preheat -> thermostat: Auto takes the run over and stops it at the top, after the minimum run */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.0f);
        m.rate_on = 0.02f;
        at_ctl_set_external(&t, 19.0f, m.now);
        m.want_start = true;    /* someone's preheat: not Hearth's thermostat */
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t flame = m.now;
        at_snapshot_t s = hsim_snap(&m);
        CHECK(t.run.owner == AT_OWNER_OTHER && at_ctl_can_switch(&t, &s));
        at_ctl_to_thermostat(&t, &s, m.now, &m.ops);
        CHECK(t.autot.enabled && t.run.owner == AT_OWNER_AUTO);
        CHECK(m.l.starts == 0 && m.l.stops == 0);  /* no start of its own: the caller sends ff fb */
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 240 * MIN_MS));
        CHECK(m.room >= 20.95f && m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        CHECK(t.last_end == AT_END_TOP);
    }
    /* preheat -> thermostat in a warm room: held to the minimum run, then stopped */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 21.5f);
        at_ctl_set_external(&t, 21.5f, m.now);
        m.want_start = true;
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t flame = m.now;
        at_snapshot_t s = hsim_snap(&m);
        at_ctl_to_thermostat(&t, &s, m.now, &m.ops);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 60 * MIN_MS));
        CHECK(m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS && m.last_stop_ms - flame < AT_CTL_MIN_RUN_MS + 100000);
    }
    /* no switch without a run going: idle, cooling down, or a stop already sent */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 22.0f);
        at_snapshot_t s = hsim_snap(&m);
        CHECK(!at_ctl_can_switch(&t, &s));
        hsim_step(&m);
        m.want_start = true;
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        s = hsim_snap(&m);
        CHECK(at_ctl_can_switch(&t, &s));
        at_ctl_user_stop(&t, AT_SRC_APP, m.now);
        CHECK(!at_ctl_can_switch(&t, &s));
        m.state = 4;
        s = hsim_snap(&m);
        CHECK(!at_ctl_can_switch(&t, &s));
    }
}

static void test_preheat_backstop(void)
{
    printf("run mode switch: a switched preheat the heater does not stop is stopped by Hearth\n");
    at_ctl_t t;
    hsim_t m;
    hsim_init(&m, &t, 19.9f);
    m.rate_on = 0.01f;
    hsim_arm(&m);
    CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
    uint32_t on = t.run.on_ms;
    hsim_run(&m, 10 * MIN_MS);
    at_ctl_to_preheat(&t, m.now, 45);      /* this sim heater never stops itself: as if it ignored the type02 */
    CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 120 * MIN_MS));
    CHECK(m.last_stop_ms - on >= 50 * MIN_MS && m.last_stop_ms - on < 50 * MIN_MS + 10000);
    CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
    CHECK(t.last_end == AT_END_HEATER && !t.autot.enabled);
    /* a longer time asked for afterwards moves the backstop; back to thermostat clears it */
    {
        at_ctl_t t2;
        hsim_t m2;
        hsim_init(&m2, &t2, 19.9f);
        m2.rate_on = 0.001f;
        hsim_arm(&m2);
        CHECK(HSIM_UNTIL(&m2, m2.state == 3, 10 * MIN_MS));
        uint32_t on2 = t2.run.on_ms;
        at_ctl_to_preheat(&t2, m2.now, 40);
        at_ctl_preheat_time(&t2, 90);
        CHECK(HSIM_UNTIL(&m2, m2.l.stops == 1, 200 * MIN_MS));
        CHECK(m2.last_stop_ms - on2 >= 95 * MIN_MS && m2.last_stop_ms - on2 < 95 * MIN_MS + 10000);
    }
    {
        at_ctl_t t3;
        hsim_t m3;
        hsim_init(&m3, &t3, 19.9f);
        m3.rate_on = 0.001f;   /* never reaches the top: only a backstop would stop it */
        hsim_arm(&m3);
        CHECK(HSIM_UNTIL(&m3, m3.state == 3, 10 * MIN_MS));
        at_ctl_to_preheat(&t3, m3.now, 40);
        at_snapshot_t s = hsim_snap(&m3);
        at_ctl_to_thermostat(&t3, &s, m3.now, &m3.ops);
        hsim_run(&m3, 120 * MIN_MS);
        CHECK(m3.l.stops == 0 && m3.state == 3 && t3.autot.enabled);
    }
    /* a preheat nobody switched has no backstop: the heater's own time ends it */
    {
        at_ctl_t t4;
        hsim_t m4;
        hsim_init(&m4, &t4, 19.0f);
        at_ctl_set_external(&t4, 19.0f, m4.now);
        m4.rate_on = 0.001f;
        m4.want_start = true;
        hsim_run(&m4, 300 * MIN_MS);
        CHECK(m4.l.stops == 0 && m4.state == 3);
    }
}

static void test_auto_off_in_cooldown(void)
{
    printf("Auto off and a new set temperature while the heater cools down\n");
    at_ctl_t t;
    hsim_t m;
    hsim_init(&m, &t, 20.4f);
    m.rate_on = 0.05f;
    hsim_arm(&m);
    CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 120 * MIN_MS));
    CHECK(HSIM_UNTIL(&m, m.state == 4, 30000));
    /* cooling down: a new set temperature is just a setting */
    at_ctl_set_auto_range(&t, 18.0f, 19.0f);
    hsim_step(&m);
    CHECK(m.l.stops == 1 && m.l.starts == 1 && m.state == 4);
    /* thermostat off: no stop frame into the purge, and it does not start again when the room cools */
    at_snapshot_t s = hsim_snap(&m);
    at_ctl_auto_off(&t, true, AT_HOLD_THERMO_OFF, AT_SRC_HA, &s, m.now, &m.ops);
    CHECK(!t.autot.enabled && m.l.stops == 1 && t.held == AT_HOLD_NONE);
    CHECK(strcmp(t.autot.note, "Auto thermostat disabled") == 0);
    CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
    CHECK(t.last_end == AT_END_TOP);
    m.rate_off = 0.5f;
    hsim_run(&m, 90 * MIN_MS);
    CHECK(m.room < 17.0f && m.l.starts == 1 && m.l.stops == 1);
    /* idle: off sends nothing either */
    at_ctl_set_external(&t, roundf(m.room * 10) / 10, m.now);
    s = hsim_snap(&m);
    t.off_lock = true;   /* keep the start back so Auto on is only armed */
    t.off_until_ms = m.now + 10 * MIN_MS;
    at_ctl_set_auto(&t, true, &s, m.now, &m.ops);
    CHECK(m.l.starts == 1);
    at_ctl_auto_off(&t, true, AT_HOLD_THERMO_OFF, AT_SRC_APP, &s, m.now, &m.ops);
    CHECK(m.l.stops == 1);
    /* ...but a start Auto has just sent, not yet showing, is stopped */
    t.off_lock = false;
    at_ctl_set_auto(&t, true, &s, m.now, &m.ops);
    CHECK(m.l.starts == 2);
    at_ctl_auto_off(&t, true, AT_HOLD_THERMO_OFF, AT_SRC_APP, &s, m.now, &m.ops);
    CHECK(m.l.stops == 2);
}

static void test_run_mode_all(void)
{
    test_run_mode_bus();
    test_run_mode_ctl();
    test_preheat_backstop();
    test_auto_off_in_cooldown();
}
