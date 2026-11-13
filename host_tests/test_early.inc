/* Early start inside the range (docs/EARLY_START.md §9), included by test_main.c after test_warm.inc: the start
 * point (at_early), the busy rule, the saved state, Auto's starts in the simulator, and the cooling time kept when a
 * run starts above the working bottom (at_learn). */

static at_early_in_t early_in(float lo, float hi, float stop_at)
{
    at_early_in_t in;
    memset(&in, 0, sizeof(in));
    in.ext = true;
    in.lo = lo;
    in.hi = hi;
    in.stop_at = stop_at;
    return in;
}

static void test_early_point(void)
{
    printf("early start: the start point\n");
    at_early_t e;
    at_early_init(&e);
    at_early_out_t o;

    /* nothing learned: halfway, 18.5 for a set temperature of 19 */
    at_early_in_t in = early_in(18, 19, 19);
    at_early_point(&e, &in, &o);
    CHECK(o.est == AT_EARLY_EST_HALFWAY && NEAR(o.start_at, 18.5f, 0.001f) && !o.have_rate);

    /* the cabin's fastest run (plan §1): net 0.0255 C/min, 20 min warm-up -> climb 0.31, 18.69 rounded down */
    in.have_gross = true;
    in.gross = 0.035f;
    in.have_loss = true;
    in.loss = 0.0095f;
    in.have_warm = true;
    in.warm_s = 20 * 60;
    at_early_point(&e, &in, &o);
    CHECK(o.est == AT_EARLY_EST_MODEL && NEAR(o.net_rate, 0.0255f, 1e-4f) && NEAR(o.climb, 0.306f, 0.002f));
    CHECK(NEAR(o.start_at, 18.6f, 0.001f));

    /* a slow room: the narrowest band applies, 18.7 */
    in.gross = 0.0069f;
    in.loss = 0;
    at_early_point(&e, &in, &o);
    CHECK(NEAR(o.start_at, 18.7f, 0.001f));

    /* no current cooling figure: the gross rate alone (a faster climb, a lower start) */
    in.gross = 0.035f;
    in.have_loss = false;
    at_early_point(&e, &in, &o);
    CHECK(o.est == AT_EARLY_EST_GROSS && NEAR(o.net_rate, 0.035f, 1e-4f) && NEAR(o.start_at, 18.5f, 0.001f));

    /* a fast room: clamped at the working bottom */
    in.gross = 0.1f;
    in.warm_s = 5 * 60;
    at_early_point(&e, &in, &o);
    CHECK(NEAR(o.start_at, 18.0f, 0.001f));

    /* warm-up not known: none assumed, the whole 30 minutes of climb */
    in.gross = 0.01f;
    in.have_warm = false;
    at_early_point(&e, &in, &o);
    CHECK(NEAR(o.climb, 0.36f, 0.002f) && NEAR(o.start_at, 18.6f, 0.001f));

    /* a warm-up longer than the cap (20 min) counts as 20 */
    in.have_warm = true;
    in.warm_s = 50 * 60;
    at_early_point(&e, &in, &o);
    CHECK(NEAR(o.climb, 0.12f, 0.002f) && NEAR(o.start_at, 18.7f, 0.001f));

    /* the heater cannot be shown to beat the loss: no climb needed, the narrowest band */
    in.gross = 0.02f;
    in.have_loss = true;
    in.loss = 0.018f;
    at_early_point(&e, &in, &o);
    CHECK(o.climb == 0 && NEAR(o.start_at, 18.7f, 0.001f));

    /* the early stop (coast) lowers the stop point: the start stays 0.1 below it */
    in = early_in(18, 19, 18.6f);
    at_early_point(&e, &in, &o);   /* halfway 18.5, under 18.5 */
    CHECK(NEAR(o.start_at, 18.5f, 0.001f));
    in = early_in(18, 19, 18.5f);
    in.have_gross = true;
    in.gross = 0.006f;
    in.have_warm = true;
    in.warm_s = 1200;
    at_early_point(&e, &in, &o);
    CHECK(NEAR(o.start_at, 18.4f, 0.001f));

    /* the controller's whole-degree sensor: the bottom of the working range, as before */
    in = early_in(17, 19, 19);
    in.ext = false;
    in.have_gross = true;
    in.gross = 0.006f;
    at_early_point(&e, &in, &o);
    CHECK(o.est == AT_EARLY_EST_OFF && o.start_at == 17.0f);
    CHECK(!strcmp(at_early_est_name(AT_EARLY_EST_OFF), "off") && !strcmp(at_early_est_name(AT_EARLY_EST_MODEL), "model"));

    /* the busy widening lowers the cap: 0.4 -> at most 18.3 */
    e.st.busy_cc = 40;
    in = early_in(18, 19, 19);
    at_early_point(&e, &in, &o);
    CHECK(NEAR(o.busy_w, 0.4f, 0.001f) && NEAR(o.start_at, 18.3f, 0.001f));
    /* widened to the whole range: back at the bottom */
    e.st.busy_cc = 70;
    at_early_point(&e, &in, &o);
    CHECK(o.start_at == 18.0f);
    e.st.busy_cc = 300;
    at_early_point(&e, &in, &o);
    CHECK(o.start_at == 18.0f);
}

static void test_early_busy(void)
{
    printf("early start: the busy rule and the saved state\n");
    at_early_t e;
    at_early_init(&e);
    const uint32_t H = 3600u * 1000u;
    uint32_t now = 1000000;

    /* two starts in an hour: not busy yet */
    at_early_auto_start(&e, now, 1.0f);
    at_early_auto_start(&e, now += 30 * MIN_MS, 1.0f);
    CHECK(e.st.busy_cc == 0 && !e.dirty);
    /* the third inside 2 hours: widened 0.2 */
    at_early_auto_start(&e, now += 40 * MIN_MS, 1.0f);
    CHECK(e.st.busy_cc == 20 && e.dirty);
    e.dirty = false;
    /* one start more is not a second step: it takes 3 new starts */
    at_early_auto_start(&e, now += 30 * MIN_MS, 1.0f);
    CHECK(e.st.busy_cc == 20);
    at_early_auto_start(&e, now += 30 * MIN_MS, 1.0f);
    CHECK(e.st.busy_cc == 40);
    /* never past the working width less 0.3 */
    for (int i = 0; i < 12; i++) at_early_auto_start(&e, now += 30 * MIN_MS, 1.0f);
    CHECK(e.st.busy_cc == 70);
    /* a narrower working range does not cut what is kept (the clamp at the bottom covers it) */
    for (int i = 0; i < 3; i++) at_early_auto_start(&e, now += 30 * MIN_MS, 0.5f);
    CHECK(e.st.busy_cc == 70);
    /* calm: a start more than 2 hours after the last narrows it by 0.1 */
    at_early_auto_start(&e, now += 2 * H + MIN_MS, 1.0f);
    CHECK(e.st.busy_cc == 60);
    at_early_auto_start(&e, now += 3 * H, 1.0f);
    at_early_auto_start(&e, now += 3 * H, 1.0f);
    CHECK(e.st.busy_cc == 40);
    /* three starts spread over more than 2 hours (each under 2 hours apart): neither busy nor calm */
    at_early_auto_start(&e, now += 70 * MIN_MS, 1.0f);
    at_early_auto_start(&e, now += 70 * MIN_MS, 1.0f);
    CHECK(e.st.busy_cc == 40);
    /* down to 0, never below */
    for (int i = 0; i < 8; i++) at_early_auto_start(&e, now += 3 * H, 1.0f);
    CHECK(e.st.busy_cc == 0);
    /* the first start after a restart is not judged calm (nothing to compare it with) */
    at_early_t r;
    at_early_init(&r);
    r.st.busy_cc = 30;
    at_early_auto_start(&r, 5000, 1.0f);
    CHECK(r.st.busy_cc == 30);

    /* restore: good, wrong version, out of range */
    at_early_store_t st = {AT_EARLY_VERSION, 0, 50};
    at_early_init(&r);
    CHECK(at_early_restore(&r, &st) && NEAR(at_early_busy(&r), 0.5f, 0.001f) && !r.dirty);
    st.version = 9;
    CHECK(!at_early_restore(&r, &st) && r.st.busy_cc == 50);
    st.version = AT_EARLY_VERSION;
    st.busy_cc = -1;
    CHECK(!at_early_restore(&r, &st));
    st.busy_cc = 401;
    CHECK(!at_early_restore(&r, &st));
    /* reset (Reset learning) clears it and asks for a save */
    at_early_reset(&r);
    CHECK(r.st.busy_cc == 0 && r.dirty && r.nstarts == 0);
    /* a box with no saved blob (the update from 1.59): starts at 0 */
    at_ctl_t t;
    at_ctl_init(&t, 20.5f, 5.0f);
    CHECK(t.early_start.st.busy_cc == 0 && t.early_start.st.version == AT_EARLY_VERSION);
}

/* Auto's starts in the simulator (range 20..21) */
static void test_early_ctl(void)
{
    printf("early start: Auto's starts\n");
    /* nothing learned: Auto starts at 20.5, not at 20.0 */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 20.9f);
        m.rate_off = 0.01f;
        hsim_arm(&m);
        CHECK(m.l.starts == 0 && NEAR(at_ctl_auto_start_at(&t), 20.5f, 0.001f));
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 120 * MIN_MS));
        CHECK(m.room > 20.4f && m.room < 20.56f);   /* readings every 30 s, to 0.1, through the middle-of-3 filter */
        CHECK(t.early_start.nstarts == 1);
        at_ctl_status_t st;
        at_ctl_status(&t, m.now, &st);
        CHECK(st.early.est == AT_EARLY_EST_HALFWAY && NEAR(st.early.start_at, 20.5f, 0.001f));
        CHECK(st.work_low == 20.0f && st.work_high == 21.0f);   /* the working range is not touched */
    }
    /* Auto switched on at 20.4: starts straight away (at or below its start point); not counted by the busy rule */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 20.4f);
        hsim_arm(&m);
        CHECK(m.l.starts == 1 && t.early_start.nstarts == 0);
    }
    /* the controller's sensor: the start stays at the bottom of its working range (19 on its 2 C floor) */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 20.9f);
        m.feed_ext = false;
        m.rate_off = 0.02f;
        at_snapshot_t s = hsim_snap(&m);
        at_ctl_set_auto(&t, true, &s, m.now, &m.ops);
        hsim_run(&m, 10000);
        CHECK(t.sensor == AT_SENS_CTL || t.boot_ext_wait == false);
        CHECK(at_ctl_auto_start_at(&t) == 19.0f);
        CHECK(HSIM_UNTIL(&m, m.l.starts == 1, 200 * MIN_MS));
        CHECK(lroundf(m.room) <= 19);
    }
    /* busy: a fast room cycling every half hour or so widens the band back toward the bottom */
    {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 20.4f);
        m.rate_on = 0.05f;
        m.rate_off = 0.07f;   /* a cycle about every 55 min: three counted starts inside 2 hours */
        m.light_ms = 2 * MIN_MS;
        m.purge_ms = MIN_MS;
        hsim_arm(&m);
        hsim_run(&m, 4 * 60 * MIN_MS);
        CHECK(t.early_start.st.busy_cc >= 20);
        float lo, hi;
        at_ctl_working_range(&t, &lo, &hi);
        CHECK(at_ctl_auto_start_at(&t) <= hi - AT_EARLY_MIN_BAND - at_early_busy(&t.early_start) + 0.001f);
    }
}

/* A long simulation (plan §9.2): no thermostat run ends inside its 30 minutes, the room never goes past the ceiling,
 * and once the rates are learned the starts are inside the range, on fast and slow rooms. */
static void test_early_sim(void)
{
    printf("early start: long simulation, fast and slow rooms\n");
    const float on[] = {0.008f, 0.0255f, 0.06f};
    const float off[] = {0.004f, 0.01f, 0.02f};
    for (int k = 0; k < 3; k++) {
        at_ctl_t t;
        hsim_t m;
        hsim_init(&m, &t, 19.5f);
        m.rate_on = on[k];
        m.rate_off = off[k];
        m.delay_ms = 10 * MIN_MS;
        hsim_arm(&m);
        float hi_room = 0, lo_room = 99, lo_late = 99;
        int short_runs = 0;
        int seen = 0;
        for (int i = 0; i < 2 * 24 * 60 * 20; i++) {   /* two days */
            hsim_step(&m);
            if (m.room > hi_room) hi_room = m.room;
            if (i > 20 * 60 * 20) {                     /* after the first 20 hours */
                if (m.room < lo_room) lo_room = m.room;
                if (t.warm.st.ngross[AT_SENS_EXT] >= AT_WARM_MIN_LEARN && m.room < lo_late) lo_late = m.room;
            }
            while (seen < t.learn.nruns) {
                const at_learn_run_t *r = &t.learn.runs[seen++];
                if (r->end == AT_END_TOP && r->burn_s < 1800) short_runs++;
            }
            if (seen > 0 && t.learn.nruns < seen) seen = t.learn.nruns;   /* the log rolled over */
        }
        CHECK(short_runs == 0);
        CHECK(hi_room < 21.0f + AT_CTL_CEILING_OVER);
        CHECK(m.l.starts >= 2);
        if (k == 0) {
            /* so slow the warm-up (a 0.2 C rise) takes over 20 min: no rate is learned, the start stays halfway */
            CHECK(t.warm.st.ngross[AT_SENS_EXT] < AT_WARM_MIN_LEARN && NEAR(at_ctl_auto_start_at(&t), 20.5f, 0.001f));
            CHECK(lo_room > 20.35f);
        } else {
            CHECK(t.warm.st.ngross[AT_SENS_EXT] >= AT_WARM_MIN_LEARN);
        }
        /* the cabin's rate: the model keeps the room well above the bottom of the working range */
        if (k == 1) CHECK(lo_late > 20.15f);
        printf("    rate on %.4f off %.4f: %d starts, room %.2f..%.2f late, start at %.1f\n", (double)on[k],
               (double)off[k], m.l.starts, (double)lo_room, (double)hi_room, (double)at_ctl_auto_start_at(&t));
    }
}

/* The cooling time when a run starts above the working bottom (plan §6): the fall so far, scaled to the whole range */
static void test_early_cooling(void)
{
    printf("early start: cooling time kept for a run started above the bottom\n");
    at_learn_t L;
    at_learn_init(&L);
    uint32_t now = 1000000;
    /* a run to the top, stopped there */
    at_learn_run_start(&L, now, AT_SENS_EXT, true, true, 30 * MIN_MS);
    at_learn_flame(&L, now += 5 * MIN_MS);
    float temp = 18.0f;
    for (int i = 0; i < 40 * 20; i++) at_learn_sample(&L, now += 3000, temp += 0.00125f, AT_SENS_EXT, 18.0f, false);
    at_learn_stop(&L, now, true);
    at_learn_run_end(&L, now += 3 * MIN_MS, AT_END_TOP, 1.0f);
    /* the room peaks at 19.0, then falls 0.01 C a minute to 18.6 (40 min): a run starts there */
    temp = 19.0f;
    for (int i = 0; i < 40 * 20; i++) at_learn_sample(&L, now += 3000, temp -= 0.0005f, AT_SENS_EXT, 18.0f, false);
    at_learn_run_start(&L, now, AT_SENS_EXT, true, true, 40 * MIN_MS);
    /* 40 min for 0.4 C: 100 min for the whole 1 C */
    CHECK(L.last_have_cool && L.last_cool_s >= 99 * 60 && L.last_cool_s <= 101 * 60);
    at_learn_run_end(&L, now += 10 * MIN_MS, AT_END_USER, 1.0f);
    uint32_t typ;
    CHECK(at_learn_cool_typical(&L, AT_SENS_EXT, &typ) && typ >= 99 * 60 && typ <= 101 * 60);

    /* a fall of under 0.2 C is too little to scale from: not kept */
    at_learn_init(&L);
    at_learn_run_start(&L, now, AT_SENS_EXT, true, true, 30 * MIN_MS);
    at_learn_flame(&L, now += 5 * MIN_MS);
    at_learn_sample(&L, now += 3000, 19.0f, AT_SENS_EXT, 18.0f, false);
    at_learn_stop(&L, now, true);
    at_learn_run_end(&L, now += 3 * MIN_MS, AT_END_TOP, 1.0f);
    temp = 19.0f;
    for (int i = 0; i < 20 * 20; i++) at_learn_sample(&L, now += 3000, temp -= 0.0004f, AT_SENS_EXT, 18.0f, false);
    at_learn_run_start(&L, now, AT_SENS_EXT, true, true, 20 * MIN_MS);
    CHECK(!L.last_have_cool);

    /* reaching the bottom is measured as before */
    at_learn_init(&L);
    at_learn_run_start(&L, now, AT_SENS_EXT, true, true, 30 * MIN_MS);
    at_learn_flame(&L, now += 5 * MIN_MS);
    at_learn_sample(&L, now += 3000, 19.0f, AT_SENS_EXT, 18.0f, false);
    at_learn_stop(&L, now, true);
    at_learn_run_end(&L, now += 3 * MIN_MS, AT_END_TOP, 1.0f);
    temp = 19.0f;
    for (int i = 0; i < 60 * 20; i++) at_learn_sample(&L, now += 3000, temp -= 0.001f, AT_SENS_EXT, 18.0f, false);
    at_learn_run_start(&L, now, AT_SENS_EXT, true, true, 60 * MIN_MS);
    CHECK(L.last_have_cool && L.last_cool_s >= 49 * 60 && L.last_cool_s <= 51 * 60);
}

static void test_early(void)
{
    test_early_point();
    test_early_busy();
    test_early_ctl();
    test_early_sim();
    test_early_cooling();
}
