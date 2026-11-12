/* Heater Care tests (docs/ADAPTIVE_THERMOSTAT.md §3.6), included by test_main.c: the thermostat loops with their run
 * locks, ceiling, sensor handling and restart rules (at_ctl), and the learning (at_learn). */

#define MIN_MS 60000u

/* ---- a small heater and room ---------------------------------------------------------------------------------- */

typedef struct {
    ops_log_t l;               /* first: the ops callbacks count into it */
    at_ctl_t *t;
    at_ctl_ops_t ops;
    uint32_t now;
    int state;                 /* what the heater reports */
    uint32_t since;
    uint32_t light_ms;         /* switch-on to burning */
    uint32_t purge_ms;
    uint32_t delay_ms;         /* flame established to the room's first rise */
    bool want_start, want_stop;
    float room;
    float rate_on, rate_off;   /* C per minute while heating / resting */
    bool feed_ext;             /* the external sensor sends readings */
    bool ext_hold;             /* ...always the same value, ext_value, whatever the room does */
    float ext_value;
    uint32_t last_ext;
    bool cabin_ok;             /* the controller reports its cabin reading */
    int fault;
    int drop_stops;            /* the next this many stops are lost on the bus */
    uint32_t last_start_ms, last_stop_ms;
} hsim_t;

static void hsim_start_cb(void *c)
{
    hsim_t *m = c;
    m->l.starts++;
    m->want_start = true;
    m->last_start_ms = m->now;
}
static void hsim_stop_cb(void *c)
{
    hsim_t *m = c;
    m->l.stops++;
    m->want_stop = true;
    m->last_stop_ms = m->now;
}

static void hsim_init(hsim_t *m, at_ctl_t *t, float room)
{
    memset(m, 0, sizeof(*m));
    m->t = t;
    m->ops = (at_ctl_ops_t){hsim_start_cb, hsim_stop_cb, m};
    m->now = 1000000;
    m->light_ms = 5 * MIN_MS;
    m->purge_ms = 3 * MIN_MS;
    m->delay_ms = 0;
    m->room = room;
    m->rate_on = 0.05f;
    m->rate_off = 0.02f;
    m->feed_ext = true;
    m->cabin_ok = true;
    m->last_ext = 0;
    at_ctl_init(t, 20.5f, 5.0f);
    at_ctl_set_auto_range(t, 20.0f, 21.0f);
    at_ctl_set_clock(t, true, 1800000000);
}

static at_snapshot_t hsim_snap(const hsim_t *m)
{
    at_snapshot_t s = mk_snap((int)lroundf(m->room), (uint8_t)m->state, (uint8_t)m->fault);
    s.have_cabin = m->cabin_ok;
    return s;
}

/* one 3 s step: the heater reacts, the room changes, readings arrive, the loops tick */
static void hsim_step(hsim_t *m)
{
    const uint32_t dt = 3000;
    m->now += dt;
    if (m->want_start && m->state == 0) {
        m->state = 2;
        m->since = m->now;
    }
    m->want_start = false;
    if (m->want_stop && m->drop_stops > 0) {
        m->drop_stops--;
        m->want_stop = false;
    }
    if (m->want_stop && (m->state == 2 || m->state == 3)) {
        m->state = 4;
        m->since = m->now;
    }
    m->want_stop = false;
    if (m->state == 2 && m->now - m->since >= m->light_ms) {
        m->state = 3;
        m->since = m->now;
    }
    if (m->state == 4 && m->now - m->since >= m->purge_ms) {
        m->state = 0;
        m->since = m->now;
    }
    if (m->state == 3 && m->now - m->since >= m->delay_ms) m->room += m->rate_on * dt / 60000.0f;
    else if (m->state == 0) m->room -= m->rate_off * dt / 60000.0f;
    if (m->feed_ext && m->now - m->last_ext >= 30000) {
        at_ctl_set_external(m->t, m->ext_hold ? m->ext_value : roundf(m->room * 10) / 10, m->now);
        m->last_ext = m->now;
    }
    at_snapshot_t s = hsim_snap(m);
    at_ctl_tick(m->t, &s, m->now, &m->ops);
}

static void hsim_run(hsim_t *m, uint32_t ms)
{
    uint32_t end = m->now + ms;
    while ((int32_t)(m->now - end) < 0) hsim_step(m);
}

/* run until cond or a time limit; true if cond happened */
#define HSIM_UNTIL(m, cond, limit_ms)                                                                        \
    ({                                                                                                       \
        uint32_t _end = (m)->now + (limit_ms);                                                               \
        while (!(cond) && (int32_t)((m)->now - _end) < 0) hsim_step(m);                                       \
        (bool)(cond);                                                                                        \
    })

static void hsim_arm(hsim_t *m)
{
    at_snapshot_t s = hsim_snap(m);
    at_ctl_set_external(m->t, roundf(m->room * 10) / 10, m->now);
    m->last_ext = m->now;
    at_ctl_set_auto(m->t, true, &s, m->now, &m->ops);
}

/* ---- the control loop ------------------------------------------------------------------------------------------ */

static void test_ctl_locks(void)
{
    printf("Heater Care: run and off locks\n");
    /* fast room: reaches the top long before 30 min; the stop waits for 30 min from flame established */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.1f;
        hsim_arm(&m);
        CHECK(m.l.starts == 1);
        hsim_step(&m);
        uint32_t on = m.now;
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t flame = m.now;
        CHECK(HSIM_UNTIL(&m, t.held == AT_HOLD_TOP, 20 * MIN_MS));
        CHECK(m.l.stops == 0 && m.room > 21.0f);
        CHECK(strstr(t.autot.note, "holding: minimum run") != NULL);
        at_ctl_status_t st;
        at_ctl_status(&t, m.now, &st);
        CHECK(st.holding == AT_HOLD_TOP && st.min_run_left_s > 0 && st.min_run_left_s <= 1800);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS && m.last_stop_ms - flame < AT_CTL_MIN_RUN_MS + 10000);
        CHECK(m.last_stop_ms - on >= AT_CTL_MIN_RUN_MS + m.light_ms - 6000);
        /* the room keeps rising through the purge; the off lock starts at idle, not at the stop */
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        uint32_t idle = m.now;
        at_ctl_status(&t, m.now, &st);
        CHECK(st.min_off_left_s > 1190 && st.min_off_left_s <= 1200);
        CHECK(t.last_end == AT_END_TOP);
        /* cool fast below the bottom: no start inside 20 min of idle */
        m.rate_off = 0.5f;
        CHECK(HSIM_UNTIL(&m, m.l.starts == 2, 40 * MIN_MS));
        CHECK(m.last_start_ms - idle >= AT_CTL_MIN_OFF_MS);
        CHECK(t.learn.dip > 0 || t.learn.last_dip >= 0);
    }
    /* 12 min light-up: held 42 min from switch-on, and a stop wanted during light-up is held too */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.light_ms = 12 * MIN_MS;
        m.rate_on = 0.1f;
        hsim_arm(&m);
        hsim_step(&m);
        uint32_t on = m.now;
        /* lighting up: the full minimum run is still to come */
        hsim_run(&m, 6 * MIN_MS);
        at_ctl_status_t st;
        at_ctl_status(&t, m.now, &st);
        CHECK(st.running && !st.flame && st.min_run_left_s == 1800);
        CHECK(at_ctl_min_run_left_ms(&t, m.now) == AT_CTL_MIN_RUN_MS);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 60 * MIN_MS));
        CHECK(m.last_stop_ms - on >= 42 * MIN_MS - 3000 && m.last_stop_ms - on < 42 * MIN_MS + 10000);
    }
    /* the countdown: full through light-up, counts down from flame, 0 after */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.001f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 10 * MIN_MS);
        uint32_t left = at_ctl_min_run_left_ms(&t, m.now);
        CHECK(left <= 20 * MIN_MS + 3000 && left >= 20 * MIN_MS - 6000);
        hsim_run(&m, 21 * MIN_MS);
        CHECK(at_ctl_min_run_left_ms(&t, m.now) == 0);
    }
}

static void test_ctl_frost(void)
{
    printf("Heater Care: frost protection\n");
    at_ctl_t t;
    hsim_t m;
    hsim_init(&m, &t, 6.0f);
    at_ctl_set_freeze(&t, true);
    at_ctl_set_freeze_target(&t, 5.0f);
    m.rate_off = 0.2f;
    m.rate_on = 0.2f;
    /* a run just ended: the off lock is on, but frost protection may still start */
    t.off_lock = true;
    t.off_until_ms = m.now + AT_CTL_MIN_OFF_MS;
    CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 30 * MIN_MS));
    hsim_step(&m);
    CHECK(at_ctl_min_off_left_ms(&t, m.now) == 0);   /* the run started: the lock is spent */
    CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
    uint32_t flame = m.now;
    CHECK(t.run.owner == AT_OWNER_FREEZE);
    /* target + 1 reached after ~5 min: the stop waits for 20 min from flame, not 30 */
    CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
    CHECK(m.last_stop_ms - flame >= AT_CTL_FROST_RUN_MS && m.last_stop_ms - flame < AT_CTL_FROST_RUN_MS + 10000);
    at_ctl_status_t st;
    at_ctl_status(&t, m.now, &st);
    /* a run frost protection did not start is left alone */
    at_ctl_t t5;
    hsim_t m5;
    hsim_init(&m5, &t5, 8.0f);
    at_ctl_set_freeze(&t5, true);
    m5.state = 3;
    hsim_run(&m5, 30 * MIN_MS);
    CHECK(m5.l.stops == 0 && m5.l.starts == 0);
    /* frost ceiling: stop point + 5 */
    at_ctl_t t6;
    hsim_t m6;
    hsim_init(&m6, &t6, 4.0f);
    at_ctl_set_freeze(&t6, true);
    m6.rate_on = 1.0f;
    CHECK(HSIM_UNTIL(&m6, m6.l.stops == 1, 30 * MIN_MS));
    CHECK(m6.room >= 11.0f - 0.2f && m6.room < 12.5f);
    CHECK(at_ctl_min_run_left_ms(&t6, m6.now) == 0);
    CHECK(t6.last_end == AT_END_CEILING || t6.run.end == AT_END_CEILING);
}

static void test_ctl_stops(void)
{
    printf("Heater Care: user, external, ceiling and fault stops\n");
    /* a user stop inside 30 min goes at once and is logged as an early stop with its source */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 9 * MIN_MS);
        at_ctl_user_stop(&t, AT_SRC_MATTER, m.now);
        m.ops.stop(m.ops.ctx);
        CHECK(t.early_unseen == 1 && t.n_early == 1 && t.early[0].src == AT_SRC_MATTER);
        CHECK(t.early[0].after_s >= 14 * 60 - 10 && t.early[0].after_s <= 14 * 60 + 10);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_EXTERNAL);
        at_ctl_status_t st;
        at_ctl_status(&t, m.now, &st);
        CHECK(st.early_stop_unseen == 1 && st.have_last_early && st.last_early.src == AT_SRC_MATTER);
        at_ctl_early_seen(&t);
        at_ctl_status(&t, m.now, &st);
        CHECK(st.early_stop_unseen == 0);
        /* an app stop is "user" */
        CHECK(t.learn.nruns >= 1 && t.learn.runs[t.learn.nruns - 1].why != AT_WHY_COUNTED);
    }
    /* ceiling: top + 5 stops inside the 30 min, not counted, no early-stop explainer */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 1.0f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.room >= 26.0f - 0.1f && m.room < 27.5f);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_CEILING && t.early_unseen == 0);
        CHECK(t.learn.runs[t.learn.nruns - 1].why != AT_WHY_COUNTED &&
              t.learn.runs[t.learn.nruns - 1].end == AT_END_CEILING);
    }
    /* a fault inside 30 min: the heater stops itself, nothing held, no early stop */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 5 * MIN_MS);
        m.fault = 78;
        m.state = 4;
        m.since = m.now;
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_FAULT && t.early_unseen == 0);
    }
    /* the Comfort Controller stops it mid-run: early stop from the controller, the rest starts, no restart in it */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_off = 0.3f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 6 * MIN_MS);
        /* the controller's stop frame went through (panel_control_seq moves) */
        m.state = 4;
        m.since = m.now;
        at_snapshot_t s = hsim_snap(&m);
        s.panel_control_seq = 1;
        s.panel_last_control_type = AT_TYPE_STOP;
        int starts = m.l.starts;
        for (int i = 0; i < 70; i++) {
            m.now += 3000;
            if (m.now - m.since >= m.purge_ms) m.state = 0;
            s = hsim_snap(&m);
            s.panel_control_seq = 1;
            s.panel_last_control_type = AT_TYPE_STOP;
            at_ctl_tick(&t, &s, m.now, &m.ops);
        }
        CHECK(t.last_end == AT_END_EXTERNAL && t.early_unseen == 1 && t.early[0].src == AT_SRC_CONTROLLER);
        uint32_t idle = t.idle_since_ms;
        CHECK(HSIM_UNTIL(&m, m.l.starts > starts, 60 * MIN_MS));
        CHECK(m.last_start_ms - idle >= AT_CTL_MIN_OFF_MS);
    }
    /* the heater pauses its own burner and relights: one run, the lock unbroken */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.08f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t flame = m.now;
        uint32_t on = t.run.on_ms;
        hsim_run(&m, 4 * MIN_MS);
        /* "waiting for temperature reduction" (1) and a moment in 4, then burning again */
        for (int st = 0; st < 2; st++) {
            m.state = st ? 4 : 1;
            for (int i = 0; i < 20; i++) {
                m.now += 3000;
                at_snapshot_t s = hsim_snap(&m);
                at_ctl_tick(&t, &s, m.now, &m.ops);
            }
            m.state = 3;
            m.since = m.now;
        }
        hsim_step(&m);
        CHECK(t.run.running && t.run.on_ms == on && t.run.flame_ms == flame && !t.run.stopping);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS);
    }
}

static void test_ctl_thermostat_off(void)
{
    printf("Heater Care: thermostat off and range lowered mid-run, timers\n");
    /* finish true (and missing, which set_auto(false) means): the run reaches 30 min, then stops */
    for (int k = 0; k < 2; k++) {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.01f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t flame = m.now;
        hsim_run(&m, 8 * MIN_MS);
        at_snapshot_t s = hsim_snap(&m);
        if (k == 0) at_ctl_auto_off(&t, true, AT_HOLD_THERMO_OFF, AT_SRC_APP, &s, m.now, &m.ops);
        else at_ctl_set_auto(&t, false, &s, m.now, &m.ops);
        CHECK(m.l.stops == 0 && t.held == AT_HOLD_THERMO_OFF && !t.autot.enabled);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS);
        CHECK(t.early_unseen == 0);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_THERMO_OFF);
    }
    /* finish false: stops at once, logged as an early stop */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 3 * MIN_MS);
        at_snapshot_t s = hsim_snap(&m);
        at_ctl_auto_off(&t, false, AT_HOLD_THERMO_OFF, AT_SRC_APP, &s, m.now, &m.ops);
        CHECK(m.l.stops == 1 && t.early_unseen == 1 && t.early[0].src == AT_SRC_APP);
    }
    /* thermostat off while idle: a plain stop, nothing held */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 20.8f);
        hsim_arm(&m);
        at_snapshot_t s = hsim_snap(&m);
        at_ctl_set_auto(&t, false, &s, m.now, &m.ops);
        CHECK(m.l.stops == 1 && t.held == AT_HOLD_NONE && t.early_unseen == 0);
    }
    /* range lowered below the room mid-run: the normal top stop, held to 30 min */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.01f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t flame = m.now;
        hsim_run(&m, 5 * MIN_MS);
        at_ctl_set_auto_range(&t, 17.0f, 18.0f);
        hsim_run(&m, 3 * MIN_MS);
        CHECK(m.l.stops == 0 && t.held == AT_HOLD_TOP);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS);
    }
    /* a timer ends 10 min into a run: its stop waits for the minimum run, and the next timer may take it over */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.01f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t flame = m.now;
        hsim_run(&m, 10 * MIN_MS);
        at_snapshot_t s = hsim_snap(&m);
        at_ctl_auto_off(&t, true, AT_HOLD_TIMER_ENDED, AT_SRC_TIMER, &s, m.now, &m.ops);
        CHECK(t.held == AT_HOLD_TIMER_ENDED && m.l.stops == 0);
        at_ctl_status_t st;
        at_ctl_status(&t, m.now, &st);
        CHECK(st.holding == AT_HOLD_TIMER_ENDED && st.min_run_left_s > 18 * 60 && st.min_run_left_s <= 20 * 60);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS && m.last_stop_ms - flame < AT_CTL_MIN_RUN_MS + 10000);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_TIMER);
        /* the take-over: Auto armed again during the hold clears it, no stop sent, no start sent */
        at_ctl_t t2;
        hsim_t m2;
        hsim_init(&m2, &t2, 19.9f);
        m2.rate_on = 0.01f;
        hsim_arm(&m2);
        CHECK(HSIM_UNTIL(&m2, m2.state == 3, 10 * MIN_MS));
        hsim_run(&m2, 5 * MIN_MS);
        s = hsim_snap(&m2);
        at_ctl_auto_off(&t2, true, AT_HOLD_TIMER_ENDED, AT_SRC_TIMER, &s, m2.now, &m2.ops);
        hsim_run(&m2, MIN_MS);
        s = hsim_snap(&m2);
        at_ctl_set_auto(&t2, true, &s, m2.now, &m2.ops);
        CHECK(t2.held == AT_HOLD_NONE && m2.l.starts == 1 && m2.l.stops == 0);
    }
}

static void test_ctl_sensors(void)
{
    printf("Heater Care: filter, stuck sensor, readings lost\n");
    /* one bad reading (0.6 low, 6 high): no start, no stop, no ceiling */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 20.4f);
        m.rate_off = 0;
        hsim_arm(&m);
        hsim_run(&m, 2 * MIN_MS);
        CHECK(m.l.starts == 0);
        at_ctl_set_external(&t, 19.8f, m.now);
        m.last_ext = m.now;
        hsim_run(&m, 20000);
        CHECK(m.l.starts == 0);
        /* running: a 6 C spike does not stop it at the top or the ceiling */
        at_ctl_t t2;
        hsim_t m2;
        hsim_init(&m2, &t2, 19.9f);
        m2.rate_on = 0.02f;
        hsim_arm(&m2);
        CHECK(HSIM_UNTIL(&m2, m2.state == 3, 10 * MIN_MS));
        hsim_run(&m2, 31 * MIN_MS);
        CHECK(m2.room < 21.0f);
        int stops = m2.l.stops;
        at_ctl_set_external(&t2, m2.room + 6.0f, m2.now);
        m2.last_ext = m2.now;
        hsim_run(&m2, 20000);
        CHECK(m2.l.stops == stops);
    }
    /* a room that holds steady while burning is not a stuck sensor (the 1.48 bug: a room sat at 16.9 for
     * 15 min of a slow heat-up and Hearth switched to a controller reading 3 C low) */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.0f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 60 * MIN_MS);
        CHECK(!t.ext_stuck && t.sensor == AT_SENS_EXT);
    }
    /* stuck external sensor: flat for 15 min while the controller's reading climbs 2 C -> the controller's */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.2f;
        m.ext_hold = true;
        m.ext_value = 19.9f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 14 * MIN_MS);           /* controller up 2+ C already, but not yet 15 min */
        CHECK(!t.ext_stuck && t.sensor == AT_SENS_EXT);
        hsim_run(&m, 2 * MIN_MS);
        CHECK(t.ext_stuck && t.sensor == AT_SENS_CTL);
        at_ctl_status_t st;
        at_ctl_status(&t, m.now, &st);
        CHECK(st.sensor_stuck && st.min_width == 2.0f);
        /* the same value resent keeps it set aside; the first new value brings it back */
        hsim_run(&m, 2 * MIN_MS);
        CHECK(t.ext_stuck);
        m.ext_value = 20.0f;
        hsim_run(&m, 31000);
        CHECK(!t.ext_stuck && t.sensor == AT_SENS_EXT);
        /* the controller's whole-degree sensor sitting still is never "stuck" */
        at_ctl_t t2;
        hsim_t m2;
        hsim_init(&m2, &t2, 19.9f);
        m2.feed_ext = false;
        m2.rate_on = 0.0f;
        at_snapshot_t s = hsim_snap(&m2);
        at_ctl_set_auto(&t2, true, &s, m2.now, &m2.ops);
        hsim_run(&m2, 30 * MIN_MS);
        CHECK(!t2.ext_stuck && t2.sensor == AT_SENS_CTL);
    }
    /* every reading lost 2 min into a run: on to the end of the minimum run, then stop; no new start */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.01f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t flame = m.now;
        hsim_run(&m, 2 * MIN_MS);
        m.feed_ext = false;
        m.cabin_ok = false;
        hsim_run(&m, 3 * MIN_MS);
        CHECK(t.held == AT_HOLD_NO_READING && m.l.stops == 0);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_NO_READING);
        m.room = 10;
        hsim_run(&m, 60 * MIN_MS);
        CHECK(m.l.starts == 1);
    }
    /* sensor drop mid-run: the working range jumps to at least 2 C, and back when it returns */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 20.5f);
        hsim_arm(&m);
        hsim_step(&m);
        float lo, hi;
        at_ctl_working_range(&t, &lo, &hi);
        CHECK(lo == 20.0f && hi == 21.0f);
        m.feed_ext = false;
        hsim_run(&m, AT_CTL_EXT_STALE_MS + 10000);
        at_ctl_working_range(&t, &lo, &hi);
        CHECK(lo == 19.0f && hi == 21.0f && t.sensor == AT_SENS_CTL);
        m.feed_ext = true;
        hsim_run(&m, 40000);
        at_ctl_working_range(&t, &lo, &hi);
        CHECK(lo == 20.0f && t.sensor == AT_SENS_EXT);
    }
    /* enabling with no reading at all never starts blind */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 15.0f);
        m.feed_ext = false;
        m.cabin_ok = false;
        at_snapshot_t s = hsim_snap(&m);
        at_ctl_set_auto(&t, true, &s, m.now, &m.ops);
        hsim_run(&m, 10 * MIN_MS);
        CHECK(m.l.starts == 0);
    }
    /* external preferred while fresh, falls back when stale */
    {
        at_ctl_t t6;
        at_ctl_init(&t6, 20.0f, 5.0f);
        at_snapshot_t cab = mk_snap(25, 0, 0);
        float temp;
        uint32_t age;
        bool ext;
        at_ctl_set_external(&t6, 18.5f, 1000);
        CHECK(at_ctl_effective_temp(&t6, &cab, 5000, &temp, &age, &ext) && ext && temp == 18.5f);
        CHECK(at_ctl_effective_temp(&t6, &cab, 1000 + AT_CTL_EXT_STALE_MS + 1, &temp, &age, &ext) && !ext && temp == 25.0f);
    }
}

static void test_ctl_restart(void)
{
    printf("Heater Care: restarts, the saved stop time and the clock\n");
    const int64_t build = 1790000000, now_e = 1800000000;
    /* power cut mid-run: the mark set and the heater idle -> 20 min rest from boot */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 18.0f);
        at_ctl_saved_t sv = {true, true, now_e - 3600};
        at_ctl_boot(&t, &sv, build, m.now);
        uint32_t boot = m.now;
        hsim_arm(&m);
        CHECK(m.l.starts == 0);
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 30 * MIN_MS));
        CHECK(m.last_start_ms - boot >= AT_CTL_MIN_OFF_MS);
    }
    /* booted with the heater running: the run lock counts from boot */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 22.0f);
        m.state = 3;
        at_ctl_saved_t sv = {true, false, 0};
        at_ctl_boot(&t, &sv, build, m.now);
        uint32_t boot = m.now;
        t.autot.enabled = true;   /* restored at boot */
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.last_stop_ms - boot >= AT_CTL_MIN_RUN_MS);
    }
    /* booted with the heater running and its flame time saved (burning 50 min): the 30 min lock is already served */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 22.0f);
        m.state = 3;
        at_ctl_saved_t sv = {true, false, 0, now_e - 50 * 60};
        at_ctl_boot(&t, &sv, build, m.now);
        uint32_t boot = m.now;
        t.autot.enabled = true;
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.last_stop_ms - boot < 5 * MIN_MS);
    }
    /* the same, burning only 10 min: 20 min of the lock left, not 30 */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 22.0f);
        m.state = 3;
        at_ctl_saved_t sv = {true, false, 0, now_e - 10 * 60};
        at_ctl_boot(&t, &sv, build, m.now);
        uint32_t boot = m.now;
        t.autot.enabled = true;
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.last_stop_ms - boot >= 20 * MIN_MS - 3000 && m.last_stop_ms - boot < 22 * MIN_MS);
    }
    /* a saved flame time with no clock at boot: counted from boot until the clock arrives (the run is "adopted") */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 22.0f);
        m.state = 3;
        at_ctl_set_clock(&t, false, 0);
        at_ctl_saved_t sv = {true, false, 0, now_e - 50 * 60};
        at_ctl_boot(&t, &sv, build, m.now);
        t.autot.enabled = true;
        hsim_run(&m, 20 * 1000);
        at_ctl_status_t st;
        at_ctl_status(&t, m.now, &st);
        CHECK(st.running && st.run_adopted);
    }
    /* the clock is set: the rest carries on from the saved stop time (stopped 15 min ago: 5 min left) */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 18.0f);
        at_ctl_saved_t sv = {false, true, now_e - 15 * 60};
        at_ctl_boot(&t, &sv, build, m.now);
        uint32_t boot = m.now;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 30 * MIN_MS));
        CHECK(m.last_start_ms - boot >= 5 * MIN_MS - 3000 && m.last_start_ms - boot < 5 * MIN_MS + 100000);
    }
    /* no clock at boot: 20 min from boot, until the clock arrives and the saved time ends it sooner */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 18.0f);
        at_ctl_set_clock(&t, false, 0);
        at_ctl_saved_t sv = {false, true, now_e - 18 * 60};
        at_ctl_boot(&t, &sv, build, m.now);
        uint32_t boot = m.now;
        hsim_arm(&m);
        hsim_run(&m, MIN_MS);
        CHECK(m.l.starts == 0 && at_ctl_min_off_left_ms(&t, m.now) > 18 * MIN_MS);
        at_ctl_set_clock(&t, true, now_e + 60);
        hsim_step(&m);
        CHECK(at_ctl_min_off_left_ms(&t, m.now) <= MIN_MS + 3000);
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 10 * MIN_MS));
        CHECK(m.last_start_ms - boot < 5 * MIN_MS);
    }
    /* a saved time in the future, or a clock before the build date: ignored, 20 min from boot */
    for (int k = 0; k < 2; k++) {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 18.0f);
        at_ctl_saved_t sv = {false, true, k == 0 ? now_e + 3600 : now_e - 3600};
        if (k == 1) at_ctl_set_clock(&t, true, build - 86400);
        at_ctl_boot(&t, &sv, build, m.now);
        uint32_t boot = m.now;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 30 * MIN_MS));
        CHECK(m.last_start_ms - boot >= AT_CTL_MIN_OFF_MS);
    }
    /* the wall clock jumping mid-run or mid-rest changes neither lock */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.08f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t flame = m.now;
        hsim_run(&m, 5 * MIN_MS);
        at_ctl_set_clock(&t, true, now_e + 86400);
        hsim_run(&m, MIN_MS);
        at_ctl_set_clock(&t, true, now_e - 86400);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS && m.last_stop_ms - flame < AT_CTL_MIN_RUN_MS + 10000);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        uint32_t idle = m.now;
        at_ctl_set_clock(&t, true, now_e + 7 * 86400);
        m.rate_off = 1.0f;
        CHECK(HSIM_UNTIL(&m, m.l.starts == 2, 40 * MIN_MS));
        CHECK(m.last_start_ms - idle >= AT_CTL_MIN_OFF_MS);
    }
    /* NVS writes: the run mark when a run starts, the stop time when it ends */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.3f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, t.save_run_mark, 3 * MIN_MS));
        t.save_run_mark = false;
        CHECK(HSIM_UNTIL(&m, t.save_stop, 60 * MIN_MS));
        CHECK(t.save_stop_epoch == 1800000000);
    }
}

/* ---- learning --------------------------------------------------------------------------------------------------- */

typedef struct {
    float start, rate;        /* C, C per minute */
    uint32_t light_min, delay_min, burn_min;
    at_run_end_t end;
    bool by_thermo;
    uint32_t off_min;         /* idle before it */
    float dip_after_flame;    /* the room keeps cooling this far for 3 min after flame established */
    float lose_heat;          /* drop this far part-way */
    float purge_rise;         /* the room rises this much after the stop */
} run_spec_t;

static run_spec_t spec(float start, float rate)
{
    run_spec_t r = {start, rate, 5, 6, 30, AT_END_TOP, true, 30, 0, 0, 0.4f};
    return r;
}

/* Feed one run (and the off period after it) through at_learn, samples every 30 s. */
static at_learn_run_t feed(at_learn_t *L, at_sensor_t s, float user_w, uint32_t *now, run_spec_t r, float cool_rate,
                           uint32_t cool_min)
{
    const bool w = s == AT_SENS_CTL;
#define RD(x) (w ? roundf(x) : roundf((x) * 100) / 100)
    at_learn_run_start(L, *now, s, r.by_thermo, true, r.off_min * MIN_MS);
    float temp = r.start;
    for (uint32_t t = 0; t < r.light_min * 60; t += 30) {
        at_learn_sample(L, *now, RD(temp + ((t / 30) % 2 ? 0.3f : -0.3f)), s, 0, false);  /* noise during light-up */
        *now += 30000;
    }
    at_learn_flame(L, *now);
    for (uint32_t t = 0; t < r.burn_min * 60; t += 30) {
        float m = t / 60.0f, v = r.start;
        if (m < 3 && r.dip_after_flame > 0) v -= r.dip_after_flame * m / 3;
        else if (r.dip_after_flame > 0 && m < r.delay_min) v -= r.dip_after_flame;
        if (m >= r.delay_min) v += r.rate * (m - r.delay_min) - (r.dip_after_flame > 0 ? r.dip_after_flame : 0);
        if (r.lose_heat > 0 && m > r.burn_min / 2.0f) v -= r.lose_heat;
        temp = v;
        at_learn_sample(L, *now, RD(temp), s, 0, false);
        *now += 30000;
    }
    at_learn_stop(L, *now, r.end == AT_END_TOP);
    for (uint32_t t = 0; t < 180; t += 30) {   /* in run, purge: the room still rises */
        temp += r.purge_rise / 6;
        at_learn_sample(L, *now, RD(temp), s, 0, false);
        *now += 30000;
    }
    at_learn_run_end(L, *now, r.end, user_w);
    at_learn_run_t out = L->runs[L->nruns - 1];
    float low = temp - 1.0f;
    low = temp - 0.5f;
    for (uint32_t t = 0; t < cool_min * 60; t += 30) {
        temp -= cool_rate / 2;
        at_learn_sample(L, *now, RD(temp), s, low, t < 20 * 60);
        *now += 30000;
    }
#undef RD
    return out;
}

static at_learn_run_t feed1(at_learn_t *L, at_sensor_t s, float uw, uint32_t *now, run_spec_t r)
{
    return feed(L, s, uw, now, r, 0.02f, 40);
}

static void test_learn_measure(void)
{
    printf("Heater Care learning: measuring a run\n");
    uint32_t now = 5000000;
    /* rise rate, warm-up delay and width needed; 5 or 15 min light-up gives the same answer */
    at_learn_run_t a, b;
    {
        at_learn_t L;
        at_learn_init(&L);
        a = feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.1f));
        CHECK(a.why == AT_WHY_COUNTED && a.have_rate);
        CHECK(fabsf(a.rate - 0.1f) < 0.003f);
        CHECK(a.warm_s >= 6 * 60 && a.warm_s <= 8 * 60 + 30);
        CHECK(a.vote == AT_VOTE_FAST);
        /* readings after the stop (the purge) do not change the rate */
        CHECK(a.burn_s == 30 * 60);
    }
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(20.0f, 0.1f);
        r.light_min = 15;
        b = feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        CHECK(b.why == AT_WHY_COUNTED && fabsf(b.rate - a.rate) < 1e-4f && b.warm_s == a.warm_s);
        CHECK(fabsf(b.need - a.need) < 1e-4f && fabsf(b.climb_min - a.climb_min) < 1e-3f);
    }
    /* still cooling for 3 min after flame established: the baseline is the lowest reading, not the first */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(20.0f, 0.1f);
        r.dip_after_flame = 0.3f;
        at_learn_run_t c = feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        CHECK(c.why == AT_WHY_COUNTED && c.warm_s >= 6 * 60 && c.warm_s <= 8 * 60 + 30);
        CHECK(fabsf(c.rate - 0.1f) < 0.003f);
    }
    /* a 25 min warm-up, or under 10 min of readings to fit: not enough data */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(20.0f, 0.1f);
        r.delay_min = 25;
        r.burn_min = 40;
        CHECK(feed1(&L, AT_SENS_EXT, 1.0f, &now, r).why == AT_WHY_NOT_ENOUGH_DATA);
        r = spec(20.0f, 0.1f);
        r.burn_min = 15;   /* fit from ~8 min to 15 */
        CHECK(feed1(&L, AT_SENS_EXT, 1.0f, &now, r).why == AT_WHY_NOT_ENOUGH_DATA);
        CHECK(L.st.s[0].n == 0);
    }
    /* open window: a 0.5 C fall part-way, or a rate under 0.005 C/min */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(20.0f, 0.05f);
        r.lose_heat = 0.6f;
        CHECK(feed1(&L, AT_SENS_EXT, 1.0f, &now, r).why == AT_WHY_LOST_HEAT);
        r = spec(20.0f, 0.004f);
        r.delay_min = 0;
        r.dip_after_flame = 0;
        at_learn_run_t z = feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        CHECK(z.why == AT_WHY_LOST_HEAT || z.why == AT_WHY_NOT_ENOUGH_DATA);
        CHECK(L.st.s[0].n == 0);
    }
    /* excluded: off 2 hours or more, not the thermostat's, stopped by the user, sensor changed mid-run */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(20.0f, 0.1f);
        r.off_min = 130;
        CHECK(feed1(&L, AT_SENS_EXT, 1.0f, &now, r).why == AT_WHY_LONG_OFF);
        r = spec(20.0f, 0.1f);
        r.by_thermo = false;
        CHECK(feed1(&L, AT_SENS_EXT, 1.0f, &now, r).why == AT_WHY_NOT_THERMOSTAT);
        r = spec(20.0f, 0.1f);
        r.end = AT_END_USER;
        CHECK(feed1(&L, AT_SENS_EXT, 1.0f, &now, r).why == AT_WHY_ENDED);
        at_learn_run_start(&L, now, AT_SENS_EXT, true, true, 0);
        at_learn_flame(&L, now);
        at_learn_sensor_changed(&L);
        at_learn_stop(&L, now + 1000, true);
        at_learn_run_end(&L, now + 2000, AT_END_TOP, 1.0f);
        CHECK(L.runs[L.nruns - 1].why == AT_WHY_SENSOR_CHANGED);
        CHECK(L.st.s[0].n == 0 && L.st.width == 0);
    }
    /* fast or slow comes from the speed: the same speed from the bottom or 1.5 C below gives the same vote */
    {
        at_learn_t L;
        at_learn_init(&L);
        at_learn_run_t p = feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.03f));
        at_learn_run_t q = feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(18.5f, 0.03f));
        CHECK(p.why == AT_WHY_COUNTED && q.why == AT_WHY_COUNTED && p.vote == q.vote);
        CHECK(fabsf(p.climb_min - q.climb_min) < 0.5f);
    }
    /* whole-degree sensor: the fit runs on the tick-overs */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(19.6f, 0.1f);
        r.burn_min = 40;
        at_learn_run_t c = feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        CHECK(c.why == AT_WHY_COUNTED && c.rate > 0.07f && c.rate < 0.13f);
    }
    /* warm-up and cooling medians are kept per sensor */
    {
        at_learn_t L;
        at_learn_init(&L);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.03f));
        feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.03f));
        uint32_t ext_w, ext_c, ctl_w;
        CHECK(at_learn_warm_typical(&L, AT_SENS_EXT, &ext_w) && !at_learn_warm_typical(&L, AT_SENS_CTL, &ctl_w));
        CHECK(at_learn_cool_typical(&L, AT_SENS_EXT, &ext_c) && ext_c > 0);
        run_spec_t r = spec(19.6f, 0.1f);
        r.burn_min = 40;
        r.delay_min = 10;
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        uint32_t ext_w2;
        at_learn_warm_typical(&L, AT_SENS_EXT, &ext_w2);
        CHECK(ext_w2 == ext_w && at_learn_warm_typical(&L, AT_SENS_CTL, &ctl_w) && ctl_w > ext_w);
    }
}

static int count_ev(const at_learn_t *L, at_learn_ev_t k)
{
    int n = 0;
    for (int i = 0; i < L->st.nhist; i++) n += L->st.hist[i].kind == k;
    return n;
}

static void test_learn_decide(void)
{
    printf("Heater Care learning: deciding\n");
    uint32_t now = 9000000;
    /* fast, fast -> straight to the wider width needed, rounded up (0.1/min, 6-8 min delay -> ~3.3 -> 3.5) */
    {
        at_learn_t L;
        at_learn_init(&L);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.1f));
        CHECK(L.st.width == 0);   /* one fast run alone: no change */
        feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.1f));
        CHECK(L.st.width == 3.5f || L.st.width == 3.0f);
        CHECK(count_ev(&L, AT_EV_WIDENED) == 1 && L.st.s[0].n == 0 && L.st.s[1].n == 0);
        const at_learn_event_t *e = &L.st.hist[L.st.nhist - 1];
        CHECK(e->old_w == 0 && e->new_w == L.st.width && e->raw_w > 2.5f && e->raw_w <= e->new_w && e->rate1 > 0.09f);
        /* settling: a run at the measured speed and the new width is neither fast nor slow */
        at_learn_run_t s = feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.1f));
        CHECK(s.why == AT_WHY_COUNTED && s.vote == AT_VOTE_OK);
        CHECK(at_learn_working_width(L.st.width, 1.0f, false) == L.st.width);
    }
    /* fast, slow, fast -> widen */
    {
        at_learn_t L;
        at_learn_init(&L);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.06f));
        feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.01f));
        CHECK(L.st.width == 0);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.06f));
        CHECK(L.st.width == 2.0f || L.st.width == 2.5f);
    }
    /* cap at 4 C; asking for 5: limit "max", one entry, nothing more for the next 6 fast runs */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(20.0f, 0.2f);
        r.burn_min = 30;
        feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        CHECK(L.st.width == 4.0f && L.st.s[0].limit == AT_LIMIT_NONE);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        CHECK(L.st.s[0].limit == AT_LIMIT_MAX && count_ev(&L, AT_EV_LIMIT) == 1);
        int nh = L.st.nhist;
        for (int i = 0; i < 6; i++) feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        CHECK(L.st.nhist == nh && L.st.width == 4.0f);
        /* life cycle: runs that no longer ask for more -> cleared, then fast again -> a new event */
        run_spec_t ok = spec(20.0f, 0.12f);
        ok.burn_min = 45;
        feed1(&L, AT_SENS_EXT, 1.0f, &now, ok);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, ok);
        CHECK(L.st.s[0].limit == AT_LIMIT_NONE && count_ev(&L, AT_EV_LIMIT_CLEARED) == 1);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        CHECK(L.st.s[0].limit == AT_LIMIT_MAX && count_ev(&L, AT_EV_LIMIT) == 2);
        /* survives a restart / a backup round trip */
        at_learn_t L2;
        at_learn_init(&L2);
        CHECK(at_learn_restore(&L2, &L.st) && L2.st.s[0].limit == AT_LIMIT_MAX && L2.st.width == 4.0f &&
              L2.st.nhist == L.st.nhist);
        /* the controller's runs do not clear the external sensor's limit */
        run_spec_t c = spec(19.6f, 0.12f);
        c.burn_min = 45;
        feed1(&L, AT_SENS_CTL, 1.0f, &now, c);
        feed1(&L, AT_SENS_CTL, 1.0f, &now, c);
        CHECK(L.st.s[0].limit == AT_LIMIT_MAX);
    }
    /* narrowing: 2 slow of 3 -> straight down, never below the user's range */
    {
        at_learn_t L;
        at_learn_init(&L);
        L.st.width = 3.0f;
        run_spec_t r = spec(20.0f, 0.03f);   /* need ~1.0: rounds to 1.0, the user's range */
        r.burn_min = 60;
        feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        CHECK(L.st.width == 1.0f || L.st.width == 1.5f);
        CHECK(count_ev(&L, AT_EV_NARROWED) == 1 && L.st.last_change == AT_EV_NARROWED);
    }
    /* two runs saying 1.2 and 0.9 -> 1.5 */
    {
        at_learn_t L;
        at_learn_init(&L);
        L.st.width = 3.0f;
        L.st.s[0].n = 2;
        L.st.s[0].v[0] = (at_learn_vote_t){1.2f, 0.035f, AT_VOTE_SLOW};
        L.st.s[0].v[1] = (at_learn_vote_t){0.9f, 0.026f, AT_VOTE_OK};
        run_spec_t r = spec(20.0f, 0.026f);
        r.burn_min = 60;
        feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        CHECK(L.st.width == 1.5f);
        /* the external sensor learned 1.5: 1.5 C on it, 3 C on the controller's (2 in whole degrees, plus 1) */
        CHECK(at_learn_working_width(L.st.width, 1.0f, false) == 1.5f);
        CHECK(at_learn_working_width(L.st.width, 1.0f, true) == 3.0f);
        CHECK(L.st.hist[L.st.nhist - 1].sensor == AT_SENS_EXT && L.st.hist[L.st.nhist - 1].work_w == 1.5f);
    }
    /* at the bottom with slow runs: "min", one entry */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(20.0f, 0.02f);
        r.burn_min = 60;
        for (int i = 0; i < 5; i++) feed1(&L, AT_SENS_EXT, 1.0f, &now, r);
        CHECK(L.st.width == 0 && L.st.s[0].limit == AT_LIMIT_MIN && count_ev(&L, AT_EV_LIMIT) == 1);
    }
    /* external learns 1 C, the controller takes over: working 2 C, learned still 1 C; slow runs on the controller
     * leave it, "min" there; the external sensor back: working 1 C */
    {
        at_learn_t L;
        at_learn_init(&L);
        L.st.width = 1.0f;
        CHECK(at_learn_working_width(1.0f, 1.0f, true) == 2.0f);
        run_spec_t r = spec(20.4f, 0.015f);   /* a real swing of 1 C in ~67 min: slow */
        r.burn_min = 100;
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        CHECK(L.st.width == 1.0f && L.st.s[1].limit == AT_LIMIT_MIN);
        CHECK(at_learn_working_width(L.st.width, 1.0f, false) == 1.0f);
        /* learned 1.5 (working 3 on the controller: 2 C + 1 C), slow runs there asking for ~0.4: down to 1 C, a whole
         * degree and no lower, working 2 */
        L.st.width = 1.5f;
        L.st.s[1].limit = 0;
        CHECK(at_learn_working_width(1.5f, 1.0f, true) == 3.0f);
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        CHECK(L.st.width == 1.0f && at_learn_working_width(L.st.width, 1.0f, true) == 2.0f);
        CHECK(count_ev(&L, AT_EV_NARROWED) == 1 && L.st.hist[L.st.nhist - 1].work_w == 2.0f);
        /* and from there, slow runs leave it: "min" */
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        CHECK(L.st.width == 1.0f && L.st.s[1].limit == AT_LIMIT_MIN && count_ev(&L, AT_EV_NARROWED) == 1);
    }
    /* learned 3 C (working 4 on the controller), the controller's runs ask for ~1.2 (0.04/min, ~8.5 min delay, a
     * real swing of 3 C in ~84 min: slow): 2 C, not 1.5, working 3 */
    {
        at_learn_t L;
        at_learn_init(&L);
        L.st.width = 3.0f;
        run_spec_t r = spec(20.4f, 0.04f);
        r.burn_min = 90;
        at_learn_run_t a = feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        CHECK(a.vote == AT_VOTE_SLOW && a.need > 1.0f && a.need < 1.5f);
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        CHECK(L.st.width == 2.0f && L.st.hist[L.st.nhist - 1].work_w == 3.0f);
    }
    /* each sensor keeps its own window: a fast external run and a fast controller run make no decision */
    {
        at_learn_t L;
        at_learn_init(&L);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.1f));
        run_spec_t c = spec(19.6f, 0.15f);
        c.burn_min = 30;
        feed1(&L, AT_SENS_CTL, 1.0f, &now, c);
        CHECK(L.st.width == 0 && L.st.s[0].n == 1 && L.st.s[1].n == 1);
    }
    /* external at "max", the controller's runs ask for narrower: the width narrows, "max" is dropped */
    {
        at_learn_t L;
        at_learn_init(&L);
        L.st.width = 4.0f;
        L.st.s[0].limit = AT_LIMIT_MAX;
        run_spec_t r = spec(20.4f, 0.04f);
        r.burn_min = 120;
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        CHECK(L.st.width < 4.0f && L.st.s[0].limit == AT_LIMIT_NONE && count_ev(&L, AT_EV_LIMIT_CLEARED) == 1);
        CHECK(!(L.st.s[0].limit == AT_LIMIT_MAX && L.st.s[1].limit == AT_LIMIT_MIN));
    }
    /* the controller's "max": its working width at the 4 C cap is a learned width of 3; fast runs there leave 3
     * alone. The external sensor then narrowing to 2.5 drops it (the controller's working width is 4 still, from
     * 3 whole degrees + 1, but the learned width is no longer at the controller's limit) */
    {
        at_learn_t L;
        at_learn_init(&L);
        L.st.width = 3.0f;
        CHECK(at_learn_working_width(3.0f, 1.0f, true) == 4.0f);
        run_spec_t c = spec(19.6f, 0.3f);
        c.burn_min = 30;
        feed1(&L, AT_SENS_CTL, 1.0f, &now, c);
        feed1(&L, AT_SENS_CTL, 1.0f, &now, c);
        CHECK(L.st.width == 3.0f && L.st.s[1].limit == AT_LIMIT_MAX && count_ev(&L, AT_EV_LIMIT) == 1);
        CHECK(count_ev(&L, AT_EV_WIDENED) == 0);
        at_learn_check_limits(&L, 1.0f);
        CHECK(L.st.s[1].limit == AT_LIMIT_MAX);
        L.st.width = 2.5f;
        at_learn_check_limits(&L, 1.0f);
        CHECK(L.st.s[1].limit == AT_LIMIT_NONE && count_ev(&L, AT_EV_LIMIT_CLEARED) == 1);
        /* learned 2 from below, fast runs on the controller asking for more than 3: up to 3, not 3.5 or 4 */
        at_learn_init(&L);
        L.st.width = 2.0f;
        feed1(&L, AT_SENS_CTL, 1.0f, &now, c);
        feed1(&L, AT_SENS_CTL, 1.0f, &now, c);
        CHECK(L.st.width == 3.0f && L.st.hist[L.st.nhist - 1].work_w == 4.0f);
    }
    /* history on the controller's sensor: worked out ~2.1 (the real swing), learned 2.5 (0.5 C steps), working 4
     * (3 in whole degrees, plus 1 C) */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(20.4f, 0.065f);
        r.burn_min = 40;
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        CHECK(count_ev(&L, AT_EV_WIDENED) == 1);
        const at_learn_event_t *e = &L.st.hist[L.st.nhist - 1];
        CHECK(e->sensor == AT_SENS_CTL && e->raw_w > 2.0f && e->raw_w < 2.5f && e->old_w == 0 && e->new_w == 2.5f &&
              e->work_w == 4.0f);
    }
    /* cold-night guard: a slow run after an off period that cooled in under half the typical time is not counted */
    {
        at_learn_t L;
        at_learn_init(&L);
        L.st.width = 3.0f;
        run_spec_t r = spec(20.0f, 0.03f);
        r.burn_min = 60;
        for (int i = 0; i < 3; i++) feed(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.075f), 0.02f, 80);
        float w = L.st.width;
        at_learn_run_t c1 = feed(&L, AT_SENS_EXT, 1.0f, &now, r, 0.5f, 10);
        at_learn_run_t c2 = feed(&L, AT_SENS_EXT, 1.0f, &now, r, 0.5f, 10);
        (void)c1;
        CHECK(c2.why == AT_WHY_COLD_SPELL && L.st.width == w);
        /* cooling alone never moves the width */
        CHECK(count_ev(&L, AT_EV_NARROWED) == 0);
    }
    /* reset clears it, with an entry */
    {
        at_learn_t L;
        at_learn_init(&L);
        L.st.width = 2.5f;
        at_learn_reset(&L);
        CHECK(L.st.width == 0 && L.st.nhist == 1 && L.st.hist[0].kind == AT_EV_RESET && L.dirty);
    }
    /* a store that makes no sense is refused */
    {
        at_learn_t L;
        at_learn_init(&L);
        at_learn_store_t bad = L.st;
        bad.width = 2.3f;
        CHECK(!at_learn_restore(&L, &bad));
        bad = L.st;
        bad.version = 99;
        CHECK(!at_learn_restore(&L, &bad));
    }
}

static void test_learn_widths(void)
{
    printf("Heater Care learning: working widths\n");
    CHECK(at_learn_working_width(0, 1.0f, false) == 1.0f);
    CHECK(at_learn_working_width(0, 1.0f, true) == 2.0f);   /* fresh 20-21 on the controller: 19-21 */
    CHECK(at_learn_working_width(1.5f, 1.0f, false) == 1.5f);
    /* on the controller's sensor: the learned width in whole degrees plus 1 C, at least 2, at most 4 */
    CHECK(at_learn_working_width(1.0f, 1.0f, true) == 2.0f);
    CHECK(at_learn_working_width(1.5f, 1.0f, true) == 3.0f);
    CHECK(at_learn_working_width(2.0f, 1.0f, true) == 3.0f);
    CHECK(at_learn_working_width(2.5f, 1.0f, true) == 4.0f);
    CHECK(at_learn_working_width(3.0f, 1.0f, true) == 4.0f);
    CHECK(at_learn_working_width(4.0f, 1.0f, true) == 4.0f);
    CHECK(at_learn_working_width(1.5f, 5.0f, true) == 5.0f);    /* the user's own wider range stands */
    CHECK(at_learn_working_width(1.0f, 3.0f, true) == 3.0f);
    /* learned 0, user range 1 C: the controller still runs on 2 C */
    CHECK(at_learn_working_width(0, 1.0f, true) == 2.0f && at_learn_working_width(0, 1.0f, false) == 1.0f);
    CHECK(at_learn_working_width(4.0f, 1.0f, false) == 4.0f);
    CHECK(at_learn_working_width(4.0f, 5.0f, false) == 5.0f);   /* the user's own wider range stands */
    CHECK(at_learn_working_width(0, 3.0f, false) == 3.0f);       /* a saved 18-21 loads unchanged */
    CHECK(at_learn_working_width(0, 1.5f, true) == 2.0f);
    CHECK(at_learn_round_up(1.02f, false) == 1.5f && at_learn_round_up(3.4f, false) == 3.5f);
    CHECK(at_learn_round_up(1.0f, false) == 1.0f && at_learn_round_up(2.1f, true) == 3.0f);
    /* learned width belongs to the space: changing the range keeps it */
    at_ctl_t t;
    at_ctl_init(&t, 20.0f, 5.0f);
    t.learn.st.width = 2.0f;
    at_ctl_set_auto_range(&t, 18.0f, 19.0f);
    CHECK(t.learn.st.width == 2.0f);
    at_ctl_set_external(&t, 18.5f, 1000);
    at_snapshot_t s = mk_snap(18, 0, 0);
    ops_log_t l = {0};
    at_ctl_ops_t ops = {op_start, op_stop, &l};
    at_ctl_tick(&t, &s, 2000, &ops);
    float lo, hi;
    at_ctl_working_range(&t, &lo, &hi);
    CHECK(lo == 17.0f && hi == 19.0f);
    at_ctl_status_t st;
    at_ctl_status(&t, 2000, &st);
    CHECK(!strcmp(st.reason, "widened") && st.learned_width == 2.0f && st.ceiling == 24.0f);
    /* on the controller's sensor: learned 2 runs on 3 C (16-19), still "widened"; nothing learned, 2 C for the
     * controller's sensor alone */
    t.sensor = AT_SENS_CTL;
    at_ctl_status(&t, 2000, &st);
    CHECK(st.work_low == 16.0f && st.work_high == 19.0f && !strcmp(st.reason, "widened") && st.min_width == 2.0f);
    t.learn.st.width = 0;
    at_ctl_status(&t, 2000, &st);
    CHECK(st.work_low == 17.0f && !strcmp(st.reason, "controller_sensor"));
    t.learn.st.width = 1.0f;
    at_ctl_status(&t, 2000, &st);
    CHECK(st.work_low == 17.0f && !strcmp(st.reason, "controller_sensor"));
    CHECK(at_ctl_src_parse("home_assistant") == AT_SRC_HA && at_ctl_src_parse("x") == AT_SRC_UNKNOWN);
}


/* ---- review fixes (1.47 review) ------------------------------------------------------------------------------- */

static void test_review_lost_stop(void)
{
    printf("Heater Care: a lost stop is sent again\n");
    /* Hearth's stop at the top is lost: sent again 90 s later, and no more once the heater purges */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.1f;
        hsim_arm(&m);
        m.drop_stops = 1;
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 60 * MIN_MS));
        uint32_t first = m.last_stop_ms;
        hsim_run(&m, 60000);
        CHECK(m.l.stops == 1 && m.state == 3 && t.run.stopping);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 2, 60000));
        CHECK(m.last_stop_ms - first >= AT_CTL_MIN_ACTION_MS && m.last_stop_ms - first < AT_CTL_MIN_ACTION_MS + 6000);
        CHECK(strstr(t.autot.note, "sending stop again") != NULL);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(m.l.stops == 2 && t.last_end == AT_END_TOP);
    }
    /* lost twice: keeps going every 90 s */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.1f;
        hsim_arm(&m);
        m.drop_stops = 3;
        CHECK(HSIM_UNTIL(&m, m.l.stops == 4, 70 * MIN_MS));
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_run(&m, 5 * MIN_MS);
        CHECK(m.l.stops == 4);
    }
    /* the held stop (thermostat off, let it finish) is lost and the room races up: the ceiling sends it at once,
     * without waiting for the 90 s retry */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.01f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 5 * MIN_MS);
        at_snapshot_t s = hsim_snap(&m);
        at_ctl_auto_off(&t, true, AT_HOLD_THERMO_OFF, AT_SRC_APP, &s, m.now, &m.ops);
        m.drop_stops = 100;
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        uint32_t first = m.last_stop_ms;
        m.room = 25.5f;
        m.rate_on = 4.0f;
        CHECK(HSIM_UNTIL(&m, m.l.stops == 2, 60000));
        CHECK(m.last_stop_ms - first < AT_CTL_MIN_ACTION_MS && m.room >= 26.0f - 0.3f);
        CHECK(t.run.end == AT_END_CEILING);
        /* then every 90 s until the heater acts on one */
        uint32_t second = m.last_stop_ms;
        m.rate_on = 0;
        CHECK(HSIM_UNTIL(&m, m.l.stops == 3, 2 * MIN_MS));
        CHECK(m.last_stop_ms - second >= AT_CTL_MIN_ACTION_MS);
        m.drop_stops = 0;
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_CEILING);
    }
    /* frost protection's stop is lost: sent again */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 4.5f);
        at_ctl_set_freeze(&t, true);
        m.rate_on = 0.2f;
        m.drop_stops = 1;
        CHECK(HSIM_UNTIL(&m, m.l.stops == 2, 60 * MIN_MS));
        CHECK(m.last_stop_ms - m.last_start_ms > AT_CTL_FROST_RUN_MS);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
    }
    /* someone starts it again while a stop is being retried: the retries end and the run carries on as Auto's run
     * would, so with Auto still on it is stopped at the top again (a fresh stop, not the retry), and not counted */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.1f;
        t.idle_known = true;
        t.idle_since_ms = m.now - 30 * MIN_MS;
        hsim_arm(&m);
        m.drop_stops = 1;
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 60 * MIN_MS));
        uint32_t first = m.last_stop_ms;
        at_ctl_user_start(&t, m.now);
        CHECK(!t.run.stopping && !t.run.resend && !t.autot.stop_pending);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 2, 5 * MIN_MS));
        CHECK(m.last_stop_ms - first >= AT_CTL_MIN_ACTION_MS);
        CHECK(strstr(t.autot.note, "-> stop") != NULL && strstr(t.autot.note, "again") == NULL);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_TOP && t.learn.runs[t.learn.nruns - 1].why == AT_WHY_ENDED);
    }
}

/* Auto on with the controller's whole-degree sensor only, idle 30 min before: the run's learning record */
static at_learn_run_t slow_ctl_run(float rate_on)
{
    at_ctl_t t;
    hsim_t m;
    hsim_init(&m, &t, 19.4f);
    m.feed_ext = false;
    m.rate_on = rate_on;
    t.idle_known = true;
    t.idle_since_ms = m.now - 30 * MIN_MS;
    at_snapshot_t s = hsim_snap(&m);
    at_ctl_tick(&t, &s, m.now, &m.ops);
    at_ctl_set_auto(&t, true, &s, m.now, &m.ops);
    CHECK(HSIM_UNTIL(&m, t.learn.nruns > 0, 4 * 60 * MIN_MS));
    CHECK(t.last_end == AT_END_TOP && t.sensor == AT_SENS_CTL);
    at_learn_run_t r = {0};
    if (t.learn.nruns) r = t.learn.runs[t.learn.nruns - 1];
    return r;
}

static void test_review_stop_tick_and_carry_on(void)
{
    printf("Heater Care: the stop tick's reading counts, user stops are re-sent, a start after a stop carries on\n");
    /* a slow run on the controller's sensor: the 21 that triggers the stop at the top is its second tick-over */
    {
        at_learn_run_t r = slow_ctl_run(0.025f);
        CHECK(r.why == AT_WHY_COUNTED && r.have_rate);
        CHECK(r.vote == AT_VOTE_SLOW || r.vote == AT_VOTE_OK);
        CHECK(fabsf(r.rate - 0.025f) < 0.006f);
        CHECK(r.burn_s > 30 * 60);
        r = slow_ctl_run(0.04f);
        CHECK(r.why == AT_WHY_COUNTED && fabsf(r.rate - 0.04f) < 0.01f);
    }
    /* a stop someone asked for is lost on the bus: sent again 90 s later while the heater still burns */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 5 * MIN_MS);
        m.drop_stops = 1;
        at_ctl_user_stop(&t, AT_SRC_APP, m.now);
        at_ctl_disarm_auto(&t, "manual Hearth stop");
        m.ops.stop(m.ops.ctx);
        uint32_t first = m.last_stop_ms;
        hsim_run(&m, 60000);
        CHECK(m.l.stops == 1 && m.state == 3 && t.run.stopping && t.run.resend);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 2, 60000));
        CHECK(m.last_stop_ms - first >= AT_CTL_MIN_ACTION_MS && m.last_stop_ms - first < AT_CTL_MIN_ACTION_MS + 6000);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(m.l.stops == 2 && !t.run.running && t.last_end == AT_END_USER && t.n_early == 1);
        hsim_run(&m, 5 * MIN_MS);
        CHECK(m.l.stops == 2);
    }
    /* a user stop, then a thermostat start during the purge: the heater relights without going idle, one run, the
     * lock from the first flame, and Auto stops it at the top */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        t.idle_known = true;
        t.idle_since_ms = m.now - 30 * MIN_MS;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t on = t.run.on_ms, flame = t.run.flame_ms;
        hsim_run(&m, 5 * MIN_MS);
        at_ctl_user_stop(&t, AT_SRC_APP, m.now);
        m.ops.stop(m.ops.ctx);
        hsim_run(&m, 60000);
        CHECK(m.state == 4 && t.run.stopping && at_ctl_min_run_left_ms(&t, m.now) == 0);
        at_ctl_user_start(&t, m.now);
        m.ops.start_thermostat(m.ops.ctx);
        CHECK(!t.run.stopping && !t.run.resend && t.run.carry);
        m.state = 2;   /* relit from the purge */
        m.since = m.now;
        hsim_step(&m);
        CHECK(t.run.running && t.run.on_ms == on && t.run.flame_ms == flame && !t.run.stopping);
        CHECK(!t.run.carry && t.run.end == AT_END_NONE && at_ctl_min_run_left_ms(&t, m.now) > 0);
        int stops = m.l.stops;
        CHECK(HSIM_UNTIL(&m, m.l.stops == stops + 1, 60 * MIN_MS));
        CHECK(m.room >= 21.0f - 0.1f && m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS);
        CHECK(t.run.hearth_stop && t.run.end == AT_END_TOP);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_TOP && t.learn.runs[t.learn.nruns - 1].why == AT_WHY_ENDED);
    }
    /* the same start, but the heater purges to idle anyway: the run still ends as the user's stop */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 5 * MIN_MS);
        at_ctl_user_stop(&t, AT_SRC_WEB, m.now);
        m.ops.stop(m.ops.ctx);
        hsim_run(&m, 30000);
        at_ctl_user_start(&t, m.now);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(!t.run.running && t.last_end == AT_END_USER);
    }
    /* a user stop keeps being lost; Auto turned on again takes the run over and stops it at the top */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        hsim_run(&m, 5 * MIN_MS);
        m.drop_stops = 100;
        at_ctl_user_stop(&t, AT_SRC_APP, m.now);
        at_ctl_disarm_auto(&t, "manual Hearth stop");
        m.ops.stop(m.ops.ctx);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 3, 4 * MIN_MS));
        at_snapshot_t s = hsim_snap(&m);
        at_ctl_set_auto(&t, true, &s, m.now, &m.ops);
        CHECK(!t.run.stopping && !t.run.resend && t.run.owner == AT_OWNER_AUTO);
        m.want_stop = false;   /* the last retry was lost too */
        m.drop_stops = 0;
        hsim_run(&m, 3 * MIN_MS);
        CHECK(m.l.stops == 3 && m.state == 3);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 4, 60 * MIN_MS));
        CHECK(m.room >= 21.0f - 0.1f && t.run.end == AT_END_TOP);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
    }
}

static void test_review_frost_and_auto(void)
{
    printf("Heater Care: frost protection and Auto never both start\n");
    /* both on, room below the frost floor: one start, Auto's run (to the top of Auto's range, not frost target + 1) */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 4.0f);
        at_ctl_set_freeze(&t, true);
        m.rate_on = 0.5f;
        hsim_arm(&m);
        CHECK(m.l.starts == 1);
        hsim_run(&m, 3 * MIN_MS);
        CHECK(m.l.starts == 1 && t.run.owner == AT_OWNER_AUTO);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 60 * MIN_MS));
        CHECK(m.room >= 21.0f - 0.1f);
        CHECK(m.last_stop_ms - t.run.flame_ms >= AT_CTL_MIN_RUN_MS);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_TOP && m.l.starts == 1);
    }
    /* Auto paced by its 90 s spacing when the room drops: frost waits for it rather than sending its own start */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 22.0f);
        at_ctl_set_freeze(&t, true);
        m.rate_off = 0;
        hsim_arm(&m);
        CHECK(m.l.starts == 0);
        hsim_step(&m);
        m.room = 4.0f;
        at_ctl_set_external(&t, 4.0f, m.now);
        m.last_ext = m.now;
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 3 * MIN_MS));
        hsim_run(&m, 3 * MIN_MS);
        CHECK(m.l.starts == 1 && t.run.owner == AT_OWNER_AUTO);
    }
    /* Auto held back by its off lock: frost starts alone (and Auto, being on, owns the run) */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 4.0f);
        at_ctl_set_freeze(&t, true);
        t.off_lock = true;
        t.off_until_ms = m.now + AT_CTL_MIN_OFF_MS;
        hsim_arm(&m);
        CHECK(m.l.starts == 0);
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 3 * MIN_MS));
        hsim_run(&m, 3 * MIN_MS);
        CHECK(m.l.starts == 1 && t.run.running && t.run.owner == AT_OWNER_AUTO);
    }
    /* Auto off: frost starts and owns its run as before */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 4.0f);
        at_ctl_set_freeze(&t, true);
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 3 * MIN_MS));
        hsim_step(&m);
        CHECK(t.run.owner == AT_OWNER_FREEZE);
    }
    /* a frost run, then Auto turned on: Auto takes it over, frost does not stop it at target + 1 */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 4.0f);
        at_ctl_set_freeze(&t, true);
        m.rate_on = 0.3f;
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        CHECK(t.run.owner == AT_OWNER_FREEZE);
        hsim_arm(&m);
        CHECK(t.run.owner == AT_OWNER_AUTO && m.l.starts == 1);
        hsim_run(&m, 25 * MIN_MS);
        CHECK(m.l.stops == 0 && m.room > 6.5f);
    }
}

static void test_review_restart_no_clock(void)
{
    printf("Heater Care: a stop with no clock still owes a rest after a restart\n");
    const int64_t build = 1790000000;
    /* the run ends with the clock not set: "stopped, time unknown" is what gets saved */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        at_ctl_set_clock(&t, false, 0);
        m.rate_on = 0.3f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, t.save_stop, 60 * MIN_MS));
        CHECK(t.save_stop_epoch == 0);
    }
    /* boot with a stop of unknown time: 20 min from boot, even once the clock is set */
    for (int k = 0; k < 2; k++) {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 18.0f);
        if (k == 1) at_ctl_set_clock(&t, false, 0);
        at_ctl_saved_t sv = {false, true, 0};
        at_ctl_boot(&t, &sv, build, m.now);
        uint32_t boot = m.now;
        hsim_arm(&m);
        hsim_run(&m, MIN_MS);
        if (k == 1) at_ctl_set_clock(&t, true, 1800000000);
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 30 * MIN_MS));
        CHECK(m.last_start_ms - boot >= AT_CTL_MIN_OFF_MS);
    }
    /* a box that has never run owes no rest */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 18.0f);
        at_ctl_set_clock(&t, false, 0);
        at_ctl_saved_t sv = {false, false, 0};
        at_ctl_boot(&t, &sv, build, m.now);
        uint32_t boot = m.now;
        hsim_arm(&m);
        hsim_step(&m);
        CHECK(at_ctl_min_off_left_ms(&t, m.now) == 0 && !t.off_lock);
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 3 * MIN_MS));
        CHECK(m.last_start_ms - boot <= AT_CTL_MIN_ACTION_MS + 6000);
    }
}

static void test_review_learning(void)
{
    printf("Heater Care learning: 2 tick-overs, and the cooling peak in the purge\n");
    uint32_t now = 20000000;
    /* the controller's sensor over its 2 C working range: the warm-up tick-over and one more, counted */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(19.4f, 0.05f);   /* 19 -> 20 at ~8 min, 21 at ~28 min */
        r.burn_min = 29;
        at_learn_run_t a = feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        /* the real swing is 1 C (19.5 -> 20.5), done in ~28 min: fast, and it asks for a real swing of ~1.6 */
        CHECK(a.why == AT_WHY_COUNTED && a.vote == AT_VOTE_FAST);
        CHECK(a.rate > 0.04f && a.rate < 0.06f);
        CHECK(a.climb_min > 20.0f && a.climb_min < 30.0f);
        CHECK(a.need > 1.0f && a.need < 2.0f);
        /* slow ones count too, and vote slow; at the 2 C floor the controller leaves the width ("min") */
        r = spec(19.4f, 0.015f);             /* 21 at ~80 min */
        r.burn_min = 81;
        at_learn_run_t b = feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        at_learn_run_t c = feed1(&L, AT_SENS_CTL, 1.0f, &now, r);
        CHECK(b.why == AT_WHY_COUNTED && b.vote == AT_VOTE_SLOW && c.why == AT_WHY_COUNTED);
        CHECK(b.rate > 0.01f && b.rate < 0.02f);
        CHECK(L.st.width == 0 && L.st.s[1].limit == AT_LIMIT_MIN);
        /* a single tick-over is still not enough */
        r = spec(19.4f, 0.05f);
        r.burn_min = 15;
        CHECK(feed1(&L, AT_SENS_CTL, 1.0f, &now, r).why == AT_WHY_NOT_ENOUGH_DATA);
    }
    /* the room rises 0.6 C in the purge after the stop at the top: that is the peak the cooling time runs from */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(20.0f, 0.03f);
        r.purge_rise = 0.6f;
        at_learn_run_t a = feed(&L, AT_SENS_EXT, 1.0f, &now, r, 0.02f, 40);
        CHECK(L.have_peak && fabsf(L.peak - a.peak_temp) < 0.006f);
        CHECK((int32_t)(now - 40 * MIN_MS - L.peak_ms) > 0);   /* seen during the purge, before the run ended */
        CHECK(L.reached_bottom && L.cool_ms >= 25 * MIN_MS);
    }
}

/* The controller's whole-degree sensor, through at_ctl_tick: the room really swings the working width less 1 C */
static void test_ctl_sensor_swing(void)
{
    printf("Heater Care learning: the controller's sensor judges the real swing (width - 1 C)\n");
    at_ctl_t t;
    hsim_t m;
    hsim_init(&m, &t, 19.4f);
    m.feed_ext = false;
    m.rate_on = 0.04f;   /* 19.5 -> 20.5 in 25 min: the top inside 30 min of flame */
    t.idle_known = true;
    t.idle_since_ms = m.now - 30 * MIN_MS;
    at_snapshot_t s = hsim_snap(&m);
    at_ctl_tick(&t, &s, m.now, &m.ops);
    at_ctl_set_auto(&t, true, &s, m.now, &m.ops);
    float lo, hi;
    at_ctl_working_range(&t, &lo, &hi);
    CHECK(lo == 19.0f && hi == 21.0f);

    /* run 1: the top inside 30 min of flame, held by the run lock; it votes fast */
    CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
    uint32_t flame = m.now;
    CHECK(HSIM_UNTIL(&m, lroundf(m.room) >= 21, 40 * MIN_MS));
    CHECK(m.now - flame < 30 * MIN_MS && m.l.stops == 0);
    CHECK(HSIM_UNTIL(&m, t.learn.nruns == 1, 60 * MIN_MS));
    at_learn_run_t a = t.learn.runs[0];
    CHECK(a.sensor == AT_SENS_CTL && a.why == AT_WHY_COUNTED && a.vote == AT_VOTE_FAST);
    CHECK(a.climb_min > 20.0f && a.climb_min < 30.0f);
    CHECK(a.need > 1.0f && a.need < 2.0f);   /* the real swing: 0.04 x (40 - 2.5) = 1.5 */
    CHECK(count_ev(&t.learn, AT_EV_WIDENED) == 0);

    /* run 2, the same room: fast again, and two fast runs widen the range (whole degrees on this sensor) */
    CHECK(HSIM_UNTIL(&m, t.learn.nruns == 2, 4 * 60 * MIN_MS));
    at_learn_run_t b = t.learn.runs[1];
    CHECK(b.why == AT_WHY_COUNTED && b.vote == AT_VOTE_FAST);
    CHECK(count_ev(&t.learn, AT_EV_WIDENED) == 1);
    /* run 2 lights just as the reading is about to tick over, so its warm-up delay is ~0 and the typical delay
     * (median of 2.5 and ~0) is ~1.3 min: it asks for 0.04 x 38.7 = ~1.55, which rounds up to a learned width of
     * 2.0 (the real swing, in 0.5 C steps), worked on as 2 + 1 = 3 C on this sensor */
    CHECK(b.need > 1.5f && b.need < 2.0f);
    const at_learn_event_t *e = &t.learn.st.hist[t.learn.st.nhist - 1];
    CHECK(t.learn.st.width == 2.0f && e->new_w == 2.0f && e->work_w == 3.0f && e->raw_w > 1.5f && e->raw_w < 2.0f);
    hsim_step(&m);
    at_ctl_working_range(&t, &lo, &hi);
    CHECK(hi == 21.0f && lo == 18.0f);

    /* run 3 at the same speed over the wider range: about 50 min of real swing, ok: it settles */
    CHECK(HSIM_UNTIL(&m, t.learn.nruns == 3, 5 * 60 * MIN_MS));
    at_learn_run_t c = t.learn.runs[2];
    CHECK(c.why == AT_WHY_COUNTED && c.vote == AT_VOTE_OK);
    CHECK(c.climb_min > 40.0f && c.climb_min < 60.0f);
    CHECK(count_ev(&t.learn, AT_EV_WIDENED) == 1 && count_ev(&t.learn, AT_EV_NARROWED) == 0);
}

/* counted runs on sensor s in the run log from index `from` on; all of them voted ok */
static int counted_on(const at_ctl_t *t, at_sensor_t s, int from, bool *all_ok)
{
    int n = 0;
    *all_ok = true;
    for (int i = from; i < t->learn.nruns; i++) {
        const at_learn_run_t *r = &t->learn.runs[i];
        if (r->sensor != s || r->why != AT_WHY_COUNTED) continue;
        n++;
        if (r->vote != AT_VOTE_OK) *all_ok = false;
    }
    return n;
}

/* The learned width is the real swing the space needs, the same number on both sensors: what the controller's
 * sensor learned is right on the external sensor too, and back again (room 19.4, 0.04 C/min on and off, 20-21) */
static void test_ctl_sensor_swap(void)
{
    printf("Heater Care learning: one learned width, right on both sensors\n");
    at_ctl_t t;
    hsim_t m;
    hsim_init(&m, &t, 19.4f);
    m.feed_ext = false;
    m.rate_on = 0.04f;
    m.rate_off = 0.04f;
    t.idle_known = true;
    t.idle_since_ms = m.now - 30 * MIN_MS;
    at_snapshot_t s = hsim_snap(&m);
    at_ctl_tick(&t, &s, m.now, &m.ops);
    at_ctl_set_auto(&t, true, &s, m.now, &m.ops);

    /* the controller's sensor learns: two fast runs, 19-21 widens to 18-21 */
    CHECK(HSIM_UNTIL(&m, count_ev(&t.learn, AT_EV_WIDENED) == 1, 6 * 60 * MIN_MS));
    const float w = t.learn.st.width;
    CHECK(w == 2.0f);   /* as in test_ctl_sensor_swing: the real swing ~1.55, rounded up */
    CHECK(m.state == 0 && !t.run.running);
    float lo, hi;
    at_ctl_working_range(&t, &lo, &hi);
    CHECK(lo == 18.0f && hi == 21.0f);
    const int nh = t.learn.st.nhist;

    /* the external sensor comes back: it runs on the learned width as it is, and its runs vote ok, no change */
    m.feed_ext = true;
    int from = t.learn.nruns;
    hsim_run(&m, 40000);
    CHECK(t.sensor == AT_SENS_EXT);
    at_ctl_working_range(&t, &lo, &hi);
    CHECK(hi == 21.0f && lo == 21.0f - w);
    bool ok = false;
    CHECK(HSIM_UNTIL(&m, counted_on(&t, AT_SENS_EXT, from, &ok) >= 2, 8 * 60 * MIN_MS));
    CHECK(ok && t.learn.st.width == w && t.learn.st.nhist == nh);

    /* and back to the controller's sensor: 18-21 again, its runs vote ok, no change */
    CHECK(HSIM_UNTIL(&m, m.state == 0 && !t.run.running, 2 * 60 * MIN_MS));
    m.feed_ext = false;
    from = t.learn.nruns;
    hsim_run(&m, AT_CTL_EXT_STALE_MS + 10000);
    CHECK(t.sensor == AT_SENS_CTL);
    at_ctl_working_range(&t, &lo, &hi);
    CHECK(lo == 18.0f && hi == 21.0f);
    CHECK(HSIM_UNTIL(&m, counted_on(&t, AT_SENS_CTL, from, &ok) >= 2, 8 * 60 * MIN_MS));
    CHECK(ok && t.learn.st.width == w && t.learn.st.nhist == nh);
    CHECK(count_ev(&t.learn, AT_EV_WIDENED) == 1 && count_ev(&t.learn, AT_EV_NARROWED) == 0);
}

static void test_review_small(void)
{
    printf("Heater Care: frost hold shown, a timer's ceiling while held\n");
    /* frost at target + 1 inside its 20 min: holding "top" while it waits, cleared after the stop */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 4.5f);
        at_ctl_set_freeze(&t, true);
        m.rate_on = 0.3f;
        CHECK(HSIM_UNTIL(&m, t.held == AT_HOLD_TOP, 20 * MIN_MS));
        CHECK(m.l.stops == 0 && t.run.owner == AT_OWNER_FREEZE);
        at_ctl_status_t st;
        at_ctl_status(&t, m.now, &st);
        CHECK(st.holding == AT_HOLD_TOP && st.ceiling == 11.0f);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 30 * MIN_MS));
        CHECK(t.held == AT_HOLD_NONE);
    }
    /* a timer for 24-25 ends 10 min in; the user's 20-21 comes back; the held run's ceiling is still 30, not 26 */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 23.9f);
        at_ctl_set_auto_range(&t, 24.0f, 25.0f);
        m.rate_on = 0.2f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t flame = m.now;
        hsim_run(&m, 3 * MIN_MS);
        at_snapshot_t s = hsim_snap(&m);
        at_ctl_auto_off(&t, true, AT_HOLD_TIMER_ENDED, AT_SRC_TIMER, &s, m.now, &m.ops);
        at_ctl_set_auto_range(&t, 20.0f, 21.0f);
        at_ctl_status_t st;
        at_ctl_status(&t, m.now, &st);
        CHECK(t.held == AT_HOLD_TIMER_ENDED && st.ceiling == 30.0f);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 40 * MIN_MS));
        CHECK(m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS && m.room > 26.0f);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_TIMER);
    }
    /* a frost run that hits its ceiling (5 + 1 + 5 = 11): after the stop the status still says 11, not Auto's 26 */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 4.5f);
        at_ctl_set_freeze(&t, true);
        m.rate_on = 1.0f;
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 30 * MIN_MS));
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        at_ctl_status_t st;
        at_ctl_status(&t, m.now, &st);
        CHECK(t.last_end == AT_END_CEILING && st.last_run_end == AT_END_CEILING && st.ceiling == 11.0f);
    }
}

/* the old fault back-off still holds */
static void test_ctl_fault_backoff(void)
{
    printf("Heater Care: fault back-off\n");
    ops_log_t l3 = {0};
    at_ctl_ops_t ops3 = {op_start, op_stop, &l3};
    at_ctl_t t3;
    at_ctl_init(&t3, 20.0f, 5.0f);
    at_snapshot_t warm3 = mk_snap(25, 0, 0);
    at_ctl_set_auto(&t3, true, &warm3, 0, &ops3); /* warm: no start */
    at_snapshot_t faulted = mk_snap(15, 0, 78);
    uint32_t tt = 200000;
    at_ctl_tick(&t3, &faulted, tt - 6000, &ops3);   /* the middle-of-3 filter settles on 15 */
    at_ctl_tick(&t3, &faulted, tt - 3000, &ops3);
    at_ctl_tick(&t3, &faulted, tt, &ops3);
    CHECK(l3.starts == 0); /* armed, waiting */
    at_ctl_tick(&t3, &faulted, tt + 4 * 60000, &ops3);
    CHECK(l3.starts == 0);
    at_ctl_tick(&t3, &faulted, tt + 5 * 60000 + 1000, &ops3);
    CHECK(l3.starts == 1);
    uint32_t t_after1 = tt + 5 * 60000 + 1000;
    at_ctl_tick(&t3, &faulted, t_after1 + 100000, &ops3); /* re-arm 15 min */
    at_ctl_tick(&t3, &faulted, t_after1 + 100000 + 14 * 60000, &ops3);
    CHECK(l3.starts == 1);
    at_ctl_tick(&t3, &faulted, t_after1 + 100000 + 15 * 60000 + 1000, &ops3);
    CHECK(l3.starts == 2);
}

/* ---- §3.6 cases the first pass left out ------------------------------------------------------------------------- */

/* the newest run in the learning log */
static const at_learn_run_t *last_run(const at_ctl_t *t) { return &t->learn.runs[t->learn.nruns - 1]; }

static void test_gaps(void)
{
    printf("Heater Care: fault runs, burner pause, spikes, dropouts, both limits, clock after a restored rest\n");
    uint32_t now = 20000000;
    /* a fault or a ceiling stop: the run is not counted and the window does not move */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t r = spec(20.0f, 0.1f);
        r.end = AT_END_FAULT;
        CHECK(feed1(&L, AT_SENS_EXT, 1.0f, &now, r).why == AT_WHY_ENDED);
        r.end = AT_END_CEILING;
        CHECK(feed1(&L, AT_SENS_EXT, 1.0f, &now, r).why == AT_WHY_ENDED);
        CHECK(L.st.s[0].n == 0 && L.st.width == 0);
    }
    /* the heater's own burner pause and relight: one run, the run lock from the first flame, and the pause stays in
     * the rise-rate fit as real elapsed time (so the rate comes out below the burning rate) */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.05f;
        hsim_arm(&m);
        CHECK(HSIM_UNTIL(&m, m.state == 3, 10 * MIN_MS));
        uint32_t flame = m.now;
        int runs = t.learn.nruns;
        hsim_run(&m, 10 * MIN_MS);
        m.state = 4;               /* the heater pauses its burner ... */
        m.since = m.now;
        hsim_run(&m, MIN_MS);
        m.state = 2;               /* ... and relights (5 min light-up) */
        m.since = m.now;
        hsim_run(&m, 30000);
        CHECK(t.run.running && !t.run.stopping && t.run.flame && t.run.flame_ms == flame);
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 60 * MIN_MS));
        CHECK(m.l.starts == 1);
        CHECK(m.last_stop_ms - flame >= AT_CTL_MIN_RUN_MS && m.last_stop_ms - flame < AT_CTL_MIN_RUN_MS + 10000);
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.learn.nruns == runs + 1);
        const at_learn_run_t *r = last_run(&t);
        CHECK(r->burn_s >= 30 * 60 - 5 && r->have_rate && r->rate > 0.02f && r->rate < 0.046f);
    }
    /* one bad reading mid-run (6 C high): no stop, and the run still counts */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        m.rate_on = 0.05f;
        hsim_arm(&m);
        /* a first run, so the second has a known off period before it */
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1, 60 * MIN_MS));
        CHECK(HSIM_UNTIL(&m, m.l.starts == 2 && m.state == 3, 240 * MIN_MS));
        hsim_run(&m, 15 * MIN_MS);
        int stops = m.l.stops;
        at_ctl_set_external(&t, m.room + 6.0f, m.now);
        m.last_ext = m.now;
        hsim_run(&m, 20000);
        CHECK(m.l.stops == stops);
        CHECK(HSIM_UNTIL(&m, m.l.stops == stops + 1, 60 * MIN_MS));
        CHECK(HSIM_UNTIL(&m, m.state == 0, 10 * MIN_MS));
        hsim_step(&m);
        CHECK(t.last_end == AT_END_TOP && last_run(&t)->why == AT_WHY_COUNTED);
    }
    /* the external sensor drops out and returns several times: each run goes into its own sensor's window, and a
     * decision only ever comes from one sensor's runs */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t c = spec(19.6f, 0.1f);
        c.burn_min = 40;
        feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.1f));
        feed1(&L, AT_SENS_CTL, 1.0f, &now, c);
        CHECK(L.st.width == 0 && L.st.s[0].n == 1 && L.st.s[1].n == 1);
        feed1(&L, AT_SENS_EXT, 1.0f, &now, spec(20.0f, 0.1f));
        CHECK(L.st.width > 0 && count_ev(&L, AT_EV_WIDENED) == 1 && L.st.hist[L.st.nhist - 1].sensor == AT_SENS_EXT);
        feed1(&L, AT_SENS_CTL, 1.0f, &now, c);
        CHECK(L.st.hist[L.st.nhist - 1].sensor == AT_SENS_EXT || L.st.hist[L.st.nhist - 1].sensor == AT_SENS_CTL);
        CHECK(L.st.s[0].n == 0);   /* the external window was used by its decision; the controller's is its own */
        /* through the loop: a dropout mid-run, twice, marks each of those runs "sensor changed" */
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.9f);
        hsim_arm(&m);
        /* a plain first run, so the off period before the next ones is known ("long off" would win otherwise) */
        CHECK(HSIM_UNTIL(&m, m.l.stops == 1 && m.state == 0, 90 * MIN_MS));
        for (int k = 0; k < 2; k++) {
            CHECK(HSIM_UNTIL(&m, m.state == 3, 120 * MIN_MS));
            hsim_run(&m, 5 * MIN_MS);
            m.feed_ext = false;
            hsim_run(&m, AT_CTL_EXT_STALE_MS + 10000);
            CHECK(t.sensor == AT_SENS_CTL);
            m.feed_ext = true;
            hsim_run(&m, 40000);
            CHECK(t.sensor == AT_SENS_EXT);
            CHECK(HSIM_UNTIL(&m, m.state == 0 && m.l.stops == k + 2, 60 * MIN_MS));
            hsim_step(&m);
            CHECK(last_run(&t)->why == AT_WHY_SENSOR_CHANGED);
        }
    }
    /* both sensors reach "max" in turn: each kept, each with its own entry; the status shows the sensor in use */
    {
        at_learn_t L;
        at_learn_init(&L);
        run_spec_t e = spec(20.0f, 0.2f);
        for (int i = 0; i < 4; i++) feed1(&L, AT_SENS_EXT, 1.0f, &now, e);
        CHECK(L.st.width == 4.0f && L.st.s[0].limit == AT_LIMIT_MAX && count_ev(&L, AT_EV_LIMIT) == 1);
        run_spec_t c = spec(19.6f, 0.3f);
        c.burn_min = 30;
        for (int i = 0; i < 3; i++) feed1(&L, AT_SENS_CTL, 1.0f, &now, c);
        CHECK(L.st.s[0].limit == AT_LIMIT_MAX && L.st.s[1].limit == AT_LIMIT_MAX && count_ev(&L, AT_EV_LIMIT) == 2);
        int ext_ev = 0, ctl_ev = 0;
        for (int i = 0; i < L.st.nhist; i++)
            if (L.st.hist[i].kind == AT_EV_LIMIT) *(L.st.hist[i].sensor == AT_SENS_EXT ? &ext_ev : &ctl_ev) += 1;
        CHECK(ext_ev == 1 && ctl_ev == 1);
        at_ctl_t t;
        at_ctl_init(&t, 20.0f, 5.0f);
        t.learn.st.s[0].limit = AT_LIMIT_MAX;
        t.learn.st.s[1].limit = AT_LIMIT_NONE;
        at_ctl_status_t st;
        t.sensor = AT_SENS_EXT;
        at_ctl_status(&t, 1000, &st);
        CHECK(st.at_limit == AT_LIMIT_MAX);
        t.sensor = AT_SENS_CTL;
        at_ctl_status(&t, 1000, &st);
        CHECK(st.at_limit == AT_LIMIT_NONE);
    }
    /* a rest restored from the saved stop time (stopped 15 min ago: 5 min left), then the wall clock jumps a day
     * either way: the deadline does not move */
    for (int k = 0; k < 2; k++) {
        const int64_t build = 1790000000, now_e = 1800000000;
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 18.0f);
        at_ctl_saved_t sv = {false, true, now_e - 15 * 60};
        at_ctl_boot(&t, &sv, build, m.now);
        uint32_t boot = m.now;
        hsim_arm(&m);
        hsim_run(&m, MIN_MS);
        at_ctl_set_clock(&t, true, k == 0 ? now_e + 86400 : now_e - 86400);
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 30 * MIN_MS));
        CHECK(m.last_start_ms - boot >= 5 * MIN_MS - 3000 && m.last_start_ms - boot < 5 * MIN_MS + 100000);
    }
}

static void test_ctl(void)
{
    test_ctl_locks();
    test_ctl_frost();
    test_ctl_stops();
    test_ctl_thermostat_off();
    test_ctl_sensors();
    test_ctl_restart();
    test_ctl_fault_backoff();
    test_review_lost_stop();
    test_review_stop_tick_and_carry_on();
    test_review_frost_and_auto();
    test_review_restart_no_clock();
    test_review_small();
    test_review_learning();
    test_ctl_sensor_swing();
    test_ctl_sensor_swap();
    test_gaps();
    test_learn_widths();
    test_learn_measure();
    test_learn_decide();
}
