/* Warm by a set time (docs/ADAPTIVE_THERMOSTAT.md §10.9), included by test_main.c after the timer helpers: the idle
 * cooling tracker, the saved lists, the prediction and its learned correction (at_warm), the timer rules and the
 * warming window (at_timers), and the one start inside the range (at_ctl). */

#define NEAR(a, b, tol) (fabsf((float)(a) - (float)(b)) <= (tol))

/* idle readings every 3 s for `min` minutes, falling `fall` C per minute */
static uint32_t idle_feed(at_warm_t *w, uint32_t now, float *temp, int min, float fall, uint8_t sensor)
{
    for (int i = 0; i < min * 20; i++) {
        now += 3000;
        *temp -= fall * 3.0f / 60.0f;
        at_warm_sample(w, now, sensor == AT_SENS_CTL ? floorf(*temp) : roundf(*temp * 10) / 10, sensor, false);
    }
    return now;
}

static at_learn_run_t warm_run(float rate, uint8_t sensor, uint32_t light_s)
{
    at_learn_run_t r;
    memset(&r, 0, sizeof(r));
    r.sensor = sensor;
    r.light_s = light_s;
    r.have_rate = true;
    r.rate_ok = true;
    r.rate = rate;
    return r;
}

static void test_warm_tracker(void)
{
    printf("warm by: idle cooling tracker\n");
    at_warm_t w;
    float loss, temp = 20.0f;
    uint32_t now = 1000000;

    /* a steady fall */
    at_warm_init(&w);
    now = idle_feed(&w, now, &temp, 40, 0.02f, AT_SENS_EXT);
    CHECK(at_warm_idle_loss(&w, AT_SENS_EXT, 30, &loss) && NEAR(loss, 0.02f, 0.003f));
    /* the other sensor's tracker has nothing */
    CHECK(!at_warm_idle_loss(&w, AT_SENS_CTL, 15, &loss));

    /* too few readings */
    at_warm_init(&w);
    temp = 20.0f;
    now = idle_feed(&w, now, &temp, 10, 0.02f, AT_SENS_EXT);
    CHECK(!at_warm_idle_loss(&w, AT_SENS_EXT, 15, &loss));
    now = idle_feed(&w, now, &temp, 8, 0.02f, AT_SENS_EXT);
    CHECK(at_warm_idle_loss(&w, AT_SENS_EXT, 15, &loss));

    /* a sensor change starts it again */
    now = idle_feed(&w, now, &temp, 5, 0.02f, AT_SENS_CTL);
    CHECK(!at_warm_idle_loss(&w, AT_SENS_EXT, 15, &loss) && !at_warm_idle_loss(&w, AT_SENS_CTL, 15, &loss));

    /* a room warming with the heater off is a loss of 0, never negative */
    at_warm_init(&w);
    temp = 15.0f;
    now = idle_feed(&w, now, &temp, 40, -0.01f, AT_SENS_EXT);
    CHECK(at_warm_idle_loss(&w, AT_SENS_EXT, 30, &loss) && loss == 0.0f);

    /* whole-degree tick-overs on the controller's sensor still give a slope */
    at_warm_init(&w);
    temp = 18.99f;
    now = idle_feed(&w, now, &temp, 60, 1.0f / 30.0f, AT_SENS_CTL);
    CHECK(at_warm_idle_loss(&w, AT_SENS_CTL, 30, &loss) && loss > 0.015f && loss < 0.06f);

    /* a start in the middle: the tracker stops, and readings while running are ignored */
    at_warm_init(&w);
    temp = 20.0f;
    now = idle_feed(&w, now, &temp, 20, 0.02f, AT_SENS_EXT);
    at_warm_run_start(&w);
    CHECK(w.run_loss_ok && NEAR(w.run_loss, 0.02f, 0.004f));
    CHECK(!at_warm_idle_loss(&w, AT_SENS_EXT, 1, &loss));
    for (int i = 0; i < 200; i++) at_warm_sample(&w, now += 3000, temp += 0.01f, AT_SENS_EXT, true);
    CHECK(!at_warm_idle_loss(&w, AT_SENS_EXT, 1, &loss));

    /* after the stop the room rises on the warm coolant: not in the fit until it is 0.3 below its peak */
    float peak = temp;
    for (int i = 0; i < 100; i++) at_warm_sample(&w, now += 3000, temp += 0.005f, AT_SENS_EXT, false);   /* +0.5 */
    peak = temp;
    now = idle_feed(&w, now, &temp, 10, 0.02f, AT_SENS_EXT);   /* -0.2: not yet past the peak */
    CHECK(w.wait_peak && !at_warm_idle_loss(&w, AT_SENS_EXT, 1, &loss));
    now = idle_feed(&w, now, &temp, 40, 0.02f, AT_SENS_EXT);
    CHECK(!w.wait_peak && at_warm_idle_loss(&w, AT_SENS_EXT, 30, &loss) && NEAR(loss, 0.02f, 0.003f));
    CHECK(temp < peak - 0.3f);
}

static void test_warm_rates(void)
{
    printf("warm by: rates kept per run\n");
    at_warm_t w;
    float temp = 20.0f;
    uint32_t now = 1000000;
    at_learn_run_t r;

    /* with an idle slope before it: gross = rise + loss */
    at_warm_init(&w);
    now = idle_feed(&w, now, &temp, 20, 0.02f, AT_SENS_EXT);
    at_warm_run_start(&w);
    r = warm_run(0.06f, AT_SENS_EXT, 240);
    at_warm_run_end(&w, &r, true);
    CHECK(w.st.nrise[AT_SENS_EXT] == 1 && NEAR(w.st.rise[AT_SENS_EXT][0], 0.06f, 1e-4f));
    CHECK(w.st.ngross[AT_SENS_EXT] == 1 && NEAR(w.st.gross[AT_SENS_EXT][0], 0.08f, 0.004f));
    CHECK(w.st.nlight == 1 && w.st.light_s[0] == 240 && w.dirty);

    /* back to back (no idle readings): a rise rate, no gross rate */
    at_warm_run_start(&w);
    at_warm_run_end(&w, &r, true);
    CHECK(w.st.nrise[AT_SENS_EXT] == 2 && w.st.ngross[AT_SENS_EXT] == 1);

    /* a bad fit adds nothing but the light-up time */
    at_warm_run_start(&w);
    at_warm_run_end(&w, &r, false);
    CHECK(w.st.nrise[AT_SENS_EXT] == 2 && w.st.nlight == 3);

    /* the lists keep the last 10 */
    for (int i = 0; i < 15; i++) {
        at_warm_run_start(&w);
        r = warm_run(0.01f * (i + 1), AT_SENS_CTL, 200 + i);
        at_warm_run_end(&w, &r, true);
    }
    CHECK(w.st.nrise[AT_SENS_CTL] == AT_WARM_KEEP && NEAR(w.st.rise[AT_SENS_CTL][0], 0.06f, 1e-4f));
    CHECK(w.st.nlight == AT_WARM_KEEP && w.st.light_s[AT_WARM_KEEP - 1] == 214);
}

static void set_gross(at_warm_t *w, float g, int n)
{
    for (int i = 0; i < n; i++) w->st.gross[AT_SENS_EXT][i] = g;
    w->st.ngross[AT_SENS_EXT] = (uint8_t)n;
}

static void test_warm_predict(void)
{
    printf("warm by: prediction\n");
    at_warm_t w;
    at_warm_pred_t p;
    float temp;
    uint32_t now = 1000000;
    at_warm_in_t in = {true, 15.0f, AT_SENS_EXT, 18.0f, 0, false, 0};

    /* nothing learned: a fixed 60 min */
    at_warm_init(&w);
    at_warm_predict(&w, &in, &p);
    CHECK(p.est == AT_WARM_EST_FIXED && p.lead_s == 60 * 60 && !p.capped && p.flame_climb_min < 0);
    /* plus the minimum off still to run */
    in.off_left_s = 600;
    at_warm_predict(&w, &in, &p);
    CHECK(p.lead_s == 70 * 60);
    in.off_left_s = 0;

    /* typical rise rate: 5 light + 5 warm + 3 C / 0.05 = 60 min * 1.2 */
    for (int i = 0; i < 3; i++) w.st.rise[AT_SENS_EXT][i] = 0.05f;
    w.st.nrise[AT_SENS_EXT] = 3;
    at_warm_predict(&w, &in, &p);
    CHECK(p.est == AT_WARM_EST_TYPICAL && NEAR(p.lead_s, 82 * 60, 2) && NEAR(p.flame_climb_min, 65, 0.01f));
    /* the controller's sensor: 25% */
    for (int i = 0; i < 3; i++) w.st.rise[AT_SENS_CTL][i] = 0.05f;
    w.st.nrise[AT_SENS_CTL] = 3;
    in.sensor = AT_SENS_CTL;
    at_warm_predict(&w, &in, &p);
    CHECK(p.est == AT_WARM_EST_TYPICAL && NEAR(p.lead_s, 85 * 60, 2));
    /* gross rates on the controller's sensor are never the full model */
    w.st.ngross[AT_SENS_CTL] = 3;
    for (int i = 0; i < 3; i++) w.st.gross[AT_SENS_CTL][i] = 0.1f;
    at_warm_predict(&w, &in, &p);
    CHECK(p.est == AT_WARM_EST_TYPICAL);
    in.sensor = AT_SENS_EXT;

    /* the full model needs the current loss: gross 0.1 - loss 0.02 = 0.08; 3 C in 37.5 min, margin 5 */
    set_gross(&w, 0.1f, 3);
    at_warm_predict(&w, &in, &p);
    CHECK(p.est == AT_WARM_EST_TYPICAL);   /* no idle readings yet */
    temp = 16.0f;
    now = idle_feed(&w, now, &temp, 40, 0.02f, AT_SENS_EXT);
    at_warm_predict(&w, &in, &p);
    CHECK(p.est == AT_WARM_EST_MODEL && NEAR(p.rate, 0.08f, 0.003f) && NEAR(p.lead_s / 60.0f, 52.5f, 2.0f));
    /* learned warm-up delay used */
    in.have_warm = true;
    in.warm_s = 600;
    at_warm_predict(&w, &in, &p);
    CHECK(NEAR(p.lead_s / 60.0f, 57.5f, 2.0f) && NEAR(p.flame_climb_min, 47.5f, 2.0f));
    in.have_warm = false;
    /* margin: 10% of a long climb */
    in.temp = 12.0f;
    at_warm_predict(&w, &in, &p);
    CHECK(NEAR(p.lead_s / 60.0f, 5 + 5 + 75 + 7.5f, 3.0f));
    in.temp = 15.0f;

    /* the learned correction: a late room lengthens the next lead, clamped to +-30 */
    w.st.err_min[0] = 10;
    w.st.err_min[1] = 10;
    w.st.err_min[2] = 12;
    w.st.nerr = 3;
    at_warm_predict(&w, &in, &p);
    CHECK(NEAR(p.corr_min, 10, 0.01f) && NEAR(p.lead_s / 60.0f, 62.5f, 2.0f));
    w.st.err_min[0] = w.st.err_min[1] = w.st.err_min[2] = -100;
    at_warm_predict(&w, &in, &p);
    CHECK(p.corr_min == -30.0f && NEAR(p.lead_s / 60.0f, 22.5f, 2.0f));
    w.st.nerr = 0;

    /* the cap */
    in.temp = 5.0f;
    at_warm_predict(&w, &in, &p);
    CHECK(p.capped && p.lead_s == AT_WARM_MAX_LEAD_MIN * 60 && p.raw_lead_s > p.lead_s);
    in.temp = 15.0f;

    /* the heater cannot win against the loss: the longest early start, no divide by zero */
    set_gross(&w, 0.022f, 3);
    at_warm_predict(&w, &in, &p);
    CHECK(p.est == AT_WARM_EST_MODEL && p.capped && p.lead_s == AT_WARM_MAX_LEAD_MIN * 60 && p.flame_climb_min > 30);

    /* already at the top */
    set_gross(&w, 0.1f, 3);
    in.temp = 19.0f;
    at_warm_predict(&w, &in, &p);
    CHECK(!p.capped && p.lead_s <= 16 * 60 && p.flame_climb_min < AT_WARM_MIN_CLIMB_MIN);

    /* no reading: fixed */
    in.have_temp = false;
    at_warm_predict(&w, &in, &p);
    CHECK(p.est == AT_WARM_EST_FIXED);
}

static void test_warm_errors(void)
{
    printf("warm by: arrival errors\n");
    at_warm_t w;
    at_warm_pred_t p = {AT_WARM_EST_MODEL, 3000, 3000, false, 0.08f, 0.02f, 40, 0};
    const int64_t fired = 1800000000, start = fired + 3000;

    /* 5 minutes late, counted, and the sign is right: positive is late */
    at_warm_init(&w);
    at_warm_fired(&w, fired, start, &p, 20, true, 15, AT_WARM_RES_PENDING, 3000);
    CHECK(w.st.nlead == 1 && w.st.lead_min[0] == 50);
    at_warm_watch(&w, fired + 60, true, 15, 20, true);
    at_warm_watch(&w, start + 300, true, 20, 20, true);
    CHECK(!w.armed && w.st.nerr == 1 && w.st.err_min[0] == 5 && w.st.nrec == 1 &&
          w.st.rec[0].result == AT_WARM_RES_COUNTED && w.st.rec[0].actual_epoch == start + 300);
    /* early by 10 minutes */
    at_warm_fired(&w, fired, start, &p, 20, true, 15, AT_WARM_RES_PENDING, 3000);
    at_warm_watch(&w, fired + 60, true, 15, 20, true);
    at_warm_watch(&w, start - 600, true, 20.1f, 20, true);
    CHECK(w.st.nerr == 2 && w.st.err_min[1] == -10);
    /* never got there: +30 at most */
    at_warm_fired(&w, fired, start, &p, 20, true, 15, AT_WARM_RES_PENDING, 3000);
    at_warm_watch(&w, fired + 60, true, 15, 20, true);
    at_warm_watch(&w, start + 29 * 60, true, 19, 20, true);
    CHECK(w.armed);
    at_warm_watch(&w, start + 30 * 60, true, 19, 20, true);
    CHECK(!w.armed && w.st.nerr == 3 && w.st.err_min[2] == 30 && w.st.rec[2].actual_epoch == 0);

    /* capped, delayed, in range: logged, not counted */
    const uint8_t hints[] = {AT_WARM_RES_CAPPED, AT_WARM_RES_DELAYED, AT_WARM_RES_IN_RANGE, AT_WARM_RES_AT_START};
    for (int i = 0; i < 4; i++) {
        at_warm_fired(&w, fired, start, &p, 20, true, 15, (at_warm_res_t)hints[i], 3000);
        at_warm_watch(&w, fired + 60, true, 15, 20, true);
        at_warm_watch(&w, start + 600, true, 20, 20, true);
        CHECK(w.st.nerr == 3 && w.st.rec[w.st.nrec - 1].result == hints[i]);
    }
    CHECK(w.st.nrec == AT_WARM_REC);   /* the oldest dropped */

    /* someone stops the run before the room got there: not counted */
    at_warm_fired(&w, fired, start, &p, 20, true, 15, AT_WARM_RES_PENDING, 3000);
    at_warm_watch(&w, fired + 60, true, 15, 20, true);
    at_warm_run_ended(&w, fired + 1200, AT_END_USER);
    CHECK(!w.armed && w.st.nerr == 3 && w.st.rec[w.st.nrec - 1].result == AT_WARM_RES_STOPPED);
    /* a fallback estimate is never counted */
    at_warm_pred_t pt = p;
    pt.est = AT_WARM_EST_TYPICAL;
    at_warm_fired(&w, fired, start, &pt, 20, true, 15, AT_WARM_RES_PENDING, 3000);
    at_warm_watch(&w, fired + 60, true, 15, 20, true);
    at_warm_watch(&w, start, true, 20, 20, true);
    CHECK(w.st.nerr == 3 && w.st.rec[w.st.nrec - 1].result == AT_WARM_RES_FALLBACK);
    /* the heater never started (the room was warm enough): in range */
    at_warm_fired(&w, fired, start, &p, 20, true, 19.5f, AT_WARM_RES_PENDING, 3000);
    at_warm_watch(&w, start + 30 * 60, true, 19.5f, 20, false);
    CHECK(w.st.nerr == 3 && w.st.rec[w.st.nrec - 1].result == AT_WARM_RES_IN_RANGE);
    /* no clock: nothing is decided */
    at_warm_fired(&w, fired, start, &p, 20, true, 15, AT_WARM_RES_PENDING, 3000);
    at_warm_watch(&w, 0, true, 25, 20, true);
    CHECK(w.armed);
    at_warm_abandon(&w, 0);
    CHECK(!w.armed && w.st.rec[w.st.nrec - 1].result == AT_WARM_RES_STOPPED);
    /* only 5 errors are kept */
    for (int i = 0; i < 4; i++) {
        at_warm_fired(&w, fired, start, &p, 20, true, 15, AT_WARM_RES_PENDING, 3000);
        at_warm_watch(&w, fired + 60, true, 15, 20, true);
        at_warm_watch(&w, start + 60, true, 20, 20, true);
    }
    CHECK(w.st.nerr == AT_WARM_ERR_KEEP && w.st.err_min[AT_WARM_ERR_KEEP - 1] == 1);
    /* the lead median */
    uint32_t lead;
    CHECK(at_warm_typical_lead(&w, &lead) && lead == 50);

    /* store: saved, restored, refused when it makes no sense, cleared by Reset learning */
    at_warm_t w2;
    at_warm_init(&w2);
    CHECK(at_warm_restore(&w2, &w.st) && !memcmp(&w2.st, &w.st, sizeof(w.st)) && !w2.dirty);
    at_warm_store_t bad = w.st;
    bad.version = 2;
    CHECK(!at_warm_restore(&w2, &bad));
    bad = w.st;
    bad.ngross[0] = 1;
    bad.gross[0][0] = NAN;
    CHECK(!at_warm_restore(&w2, &bad));
    bad = w.st;
    bad.nerr = 6;
    CHECK(!at_warm_restore(&w2, &bad));
    at_warm_reset(&w2);
    CHECK(w2.dirty && w2.st.nerr == 0 && w2.st.nrec == 0 && w2.st.version == AT_WARM_VERSION);
}

/* Through a warming window with no run ending, nothing saved changes (so the backup is not sent to the cloud again). */
static void test_warm_store_steady(void)
{
    printf("warm by: the saved store holds still through a window\n");
    at_warm_t w;
    at_warm_init(&w);
    set_gross(&w, 0.1f, 3);
    w.dirty = false;
    at_warm_store_t before = w.st;
    float temp = 16;
    uint32_t now = 1000000;
    at_warm_in_t in = {true, 16, AT_SENS_EXT, 20, 0, false, 0};
    at_warm_pred_t p;
    for (int m = 0; m < 120; m++) {
        now = idle_feed(&w, now, &temp, 1, 0.02f, AT_SENS_EXT);
        in.temp = temp;
        at_warm_predict(&w, &in, &p);
    }
    CHECK(!memcmp(&before, &w.st, sizeof(before)) && !w.dirty);
}

static void test_warm_timer_rules(void)
{
    printf("warm by: timer rules\n");
    int32_t mon = at_days_from_civil(2026, 10, 26);
    at_localtime_t now = lt_at(mon, 0);
    at_timer_t a[3];

    /* refused on preheat */
    a[0] = rep("p", 420, 60, MON);
    a[0].warm_by = 1;
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 1).err == AT_TERR_WARM_PREHEAT);
    a[0].mode = AT_TMODE_THERMOSTAT;
    CHECK(is_ok(at_timers_validate(a, 1, NULL, 0, &now, 1)));

    /* absent keeps the stored value; an explicit false clears it; a new id is false; a preheat is false */
    at_timer_t stored[2];
    stored[0] = a[0];
    stored[1] = rep("q", 900, 60, TUE);
    stored[1].mode = AT_TMODE_THERMOSTAT;
    at_timer_t in[4];
    in[0] = stored[0];
    in[0].warm_by = AT_TIMER_WARM_ABSENT;
    in[1] = stored[1];
    in[1].warm_by = AT_TIMER_WARM_ABSENT;
    in[2] = rep("new", 1200, 60, SUN);
    in[2].mode = AT_TMODE_THERMOSTAT;
    in[2].warm_by = AT_TIMER_WARM_ABSENT;
    at_timers_resolve_warm_by(in, 3, stored, 2);
    CHECK(in[0].warm_by == 1 && in[1].warm_by == 0 && in[2].warm_by == 0);
    in[0] = stored[0];
    in[0].warm_by = 0;
    at_timers_resolve_warm_by(in, 1, stored, 2);
    CHECK(in[0].warm_by == 0);
    in[0] = stored[0];
    in[0].mode = AT_TMODE_PREHEAT;
    in[0].warm_by = AT_TIMER_WARM_ABSENT;
    at_timers_resolve_warm_by(in, 1, stored, 2);
    CHECK(in[0].warm_by == 0 && is_ok(at_timers_validate(in, 1, stored, 2, &now, 1)));
    /* an unresolved absent value is never stored */
    in[0] = stored[0];
    in[0].warm_by = AT_TIMER_WARM_ABSENT;
    CHECK(!is_ok(at_timers_validate(in, 1, NULL, 0, &now, 1)));

    /* the wider gap: a warm-by 07:00 counts from 05:00, so a timer ending at 04:45 (gap to 05:15) clashes */
    a[0] = rep("w", 420, 60, MON);
    a[0].mode = AT_TMODE_THERMOSTAT;
    a[0].warm_by = 1;
    a[1] = rep("e", 225, 60, MON);   /* 03:45 - 04:45 */
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 1).err == AT_TERR_TOO_CLOSE);
    a[0].warm_by = 0;
    CHECK(is_ok(at_timers_validate(a, 2, NULL, 0, &now, 1)));
    /* one-offs too */
    at_timer_t o[2] = {once("o1", mon + 7, 420, 60), once("o2", mon + 7, 225, 60)};
    o[0].mode = AT_TMODE_THERMOSTAT;
    o[0].warm_by = 1;
    CHECK(at_timers_validate(o, 2, NULL, 0, &now, 1).err == AT_TERR_TOO_CLOSE);
    /* and across midnight, a warm-by 01:00 against a timer ending at 23:45 the day before */
    a[0] = rep("w", 60, 60, TUE);
    a[0].mode = AT_TMODE_THERMOSTAT;
    a[0].warm_by = 1;
    a[1] = rep("e", 22 * 60 + 45, 60, MON);
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 1).err == AT_TERR_TOO_CLOSE);

    /* saved before and now clashing only because of warm by: left alone. Edited: refused */
    a[0] = rep("w", 420, 60, MON);
    a[0].mode = AT_TMODE_THERMOSTAT;
    a[0].warm_by = 1;
    a[1] = rep("e", 225, 60, MON);
    CHECK(is_ok(at_timers_validate(a, 2, a, 2, &now, 1)));
    at_timer_t ed[2] = {a[0], a[1]};
    ed[1].duration_min = 61;
    CHECK(at_timers_validate(ed, 2, a, 2, &now, 1).err == AT_TERR_TOO_CLOSE);
    /* a plain clash is never let through, saved or not */
    a[1] = rep("e", 400, 60, MON);
    CHECK(at_timers_validate(a, 2, a, 2, &now, 1).err == AT_TERR_TOO_CLOSE);
}

/* ---- the warming window ---------------------------------------------------------------------------------------- */

typedef struct {
    fake_t f;                  /* first: the plain callbacks count into it */
    uint32_t lead_s;
    bool capped;
    int plans, warm_begins;
    uint8_t last_hint;
    int64_t last_start;
    bool frost;
    int64_t dst_shift_s;       /* local_to_epoch: starts after dst_from_lm are this much earlier in epoch terms */
    int64_t dst_from_lm;
    bool have_l2e;
} wfake_t;

static bool w_plan(void *c, const at_timer_t *t, int64_t start, at_warm_plan_t *o)
{
    wfake_t *f = c;
    f->plans++;
    *o = (at_warm_plan_t){f->lead_s, f->capped, AT_WARM_EST_MODEL, true};
    return true;
}
static bool w_begin(void *c, const at_timer_t *t, int64_t start, const at_warm_plan_t *p, uint8_t hint, float *room)
{
    wfake_t *f = c;
    if (f->f.fail) return false;
    f->warm_begins++;
    f->last_hint = hint;
    f->last_start = start;
    *room = 14.2f;
    return true;
}
static bool w_frost(void *c) { return ((wfake_t *)c)->frost; }
static int64_t w_l2e(void *c, int64_t lm)
{
    wfake_t *f = c;
    return lm * 60 - (lm >= f->dst_from_lm ? f->dst_shift_s : 0);
}

static at_timer_ops_t wops(wfake_t *f)
{
    at_timer_ops_t o = {f_preheat, f_begin, f_end, f, NULL, w_plan, w_begin, w_frost, f->have_l2e ? w_l2e : NULL};
    return o;
}

/* tick once a minute from local `from` to `to` on day `day`; snap_at gives the heater state per minute */
static int warm_walk(at_runner_t *r, at_timer_t *tl, int n, int32_t day, int from, int to, uint32_t *ms,
                     at_snapshot_t (*snap_at)(int minute), bool auto_on, at_timer_ops_t *ops, int *fired_at)
{
    int fired = -1;
    for (int m = from; m <= to; m++) {
        at_localtime_t t = lt_at(day, m);
        bool was = r->active;
        at_snapshot_t s = snap_at(m);
        /* a thermostat timer's run arms Auto: on while it is active */
        at_runner_tick(r, tl, n, &t, *ms += 60000, &s, auto_on || (r->active && r->mode == AT_TMODE_THERMOSTAT), ops);
        if (!was && r->active && fired < 0) fired = m;
    }
    if (fired_at) *fired_at = fired;
    return fired;
}

static at_snapshot_t s_idle(int m) { (void)m; return mk_snap(15, 0, 0); }
static at_snapshot_t s_busy(int m) { (void)m; return mk_snap(15, 3, 0); }
static at_snapshot_t s_busy_till_630(int m) { return mk_snap(15, m < 390 ? 3 : 0, 0); }

static void test_warm_window(void)
{
    printf("warm by: the warming window\n");
    int32_t mon = at_days_from_civil(2026, 10, 26);
    at_timer_t tl[1];
    tl[0] = rep("wb", 420, 60, WEEKDAYS);   /* 07:00 for an hour */
    tl[0].mode = AT_TMODE_THERMOSTAT;
    tl[0].warm_by = 1;
    at_runner_t r;
    uint32_t ms = 1000000;
    int at;

    /* fires at now + lead = start (07:00 - 48 min = 06:12), once: not again at the start minute */
    wfake_t f = {0};
    f.lead_s = 48 * 60;
    at_timer_ops_t ops = wops(&f);
    at_runner_init(&r);
    warm_walk(&r, tl, 1, mon, 4 * 60, 8 * 60, &ms, s_idle, false, &ops, &at);
    CHECK(at == 6 * 60 + 12 && f.warm_begins == 1 && f.last_hint == AT_WARM_RES_PENDING && f.f.begins == 0);
    CHECK(f.last_start == (int64_t)mon * 86400 + 420 * 60);
    CHECK(strstr(r.note, "last run finished") != NULL);   /* ended with the window, 08:00 */
    CHECK(f.f.ends == 1);
    /* the window was 06:12 to 08:00 */
    at_runner_init(&r);
    memset(&f.f, 0, sizeof(f.f));
    f.warm_begins = 0;
    warm_walk(&r, tl, 1, mon, 4 * 60, 7 * 60 + 59, &ms, s_idle, true, &ops, NULL);
    CHECK(f.warm_begins == 0);   /* Auto on all along: the user's own Auto wins */
    at_runner_init(&r);
    warm_walk(&r, tl, 1, mon, 4 * 60, 7 * 60 + 59, &ms, s_idle, false, &ops, NULL);
    CHECK(r.active && f.f.ends == 0);
    warm_walk(&r, tl, 1, mon, 8 * 60, 8 * 60, &ms, s_idle, false, &ops, NULL);
    CHECK(!r.active && f.f.ends == 1);
    /* the status: expected start in the window */
    CHECK(r.warm_on == false);   /* past the start: out of the window */

    /* the lead is checked once a minute, not every poll */
    at_runner_init(&r);
    f.plans = 0;
    for (int s = 0; s < 120; s++) {
        at_localtime_t t = lt_utc((int64_t)mon * 86400 + 5 * 3600 + s, 0);
        at_snapshot_t sn = s_idle(0);
        at_runner_tick(&r, tl, 1, &t, ms += 1000, &sn, false, &ops);
    }
    CHECK(f.plans == 2 && r.warm_on && r.warm_expected_epoch == (int64_t)mon * 86400 + 420 * 60 - 48 * 60);

    /* capped */
    at_runner_init(&r);
    f.capped = true;
    f.lead_s = 120 * 60;
    f.warm_begins = 0;
    warm_walk(&r, tl, 1, mon, 4 * 60, 7 * 60, &ms, s_idle, false, &ops, &at);
    CHECK(at == 5 * 60 && f.last_hint == AT_WARM_RES_CAPPED && strstr(r.note, "capped at 2 h"));
    f.capped = false;

    /* a busy heater that frees up inside the window: waits, then fires, delayed */
    at_runner_init(&r);
    f.lead_s = 48 * 60;
    f.warm_begins = 0;
    warm_walk(&r, tl, 1, mon, 4 * 60, 6 * 60 + 20, &ms, s_busy_till_630, false, &ops, &at);
    CHECK(at < 0 && strstr(r.note, "waiting (heater already running)"));
    warm_walk(&r, tl, 1, mon, 6 * 60 + 21, 7 * 60 + 30, &ms, s_busy_till_630, false, &ops, &at);
    CHECK(at == 6 * 60 + 30 && f.warm_begins == 1 && f.last_hint == AT_WARM_RES_DELAYED);

    /* one that does not: skipped for good at the start time (a start by hand during the window counts the same) */
    at_runner_init(&r);
    f.warm_begins = 0;
    warm_walk(&r, tl, 1, mon, 4 * 60, 8 * 60, &ms, s_busy, false, &ops, &at);
    CHECK(at < 0 && f.warm_begins == 0 && f.f.begins == 0 && strstr(r.note, "skipped: heater already running"));

    /* frost protection's run is taken over */
    at_runner_init(&r);
    f.frost = true;
    warm_walk(&r, tl, 1, mon, 4 * 60, 7 * 60, &ms, s_busy, false, &ops, &at);
    CHECK(at == 6 * 60 + 12 && f.warm_begins == 1);
    f.frost = false;
    /* ...but a plain thermostat timer still skips it */
    at_timer_t plain = tl[0];
    plain.warm_by = 0;
    at_runner_init(&r);
    f.frost = true;
    memset(&f.f, 0, sizeof(f.f));
    warm_walk(&r, &plain, 1, mon, 4 * 60, 8 * 60, &ms, s_busy, false, &ops, &at);
    CHECK(at < 0 && f.f.begins == 0);
    f.frost = false;

    /* the prediction never gets there (lead 0): fires at the start as normal, logged as at_start */
    at_runner_init(&r);
    f.lead_s = 0;
    f.warm_begins = 0;
    warm_walk(&r, tl, 1, mon, 4 * 60, 7 * 60 + 10, &ms, s_idle, false, &ops, &at);
    CHECK(at == 7 * 60 && f.warm_begins == 1 && f.last_hint == AT_WARM_RES_AT_START);
    f.lead_s = 48 * 60;

    /* the clock becoming trusted inside the window: an early start, not a missed one */
    at_runner_init(&r);
    f.warm_begins = 0;
    for (int m = 4 * 60; m < 6 * 60 + 20; m++) {
        at_localtime_t bad = {0};
        at_snapshot_t s = s_idle(m);
        at_runner_tick(&r, tl, 1, &bad, ms += 60000, &s, false, &ops);
    }
    warm_walk(&r, tl, 1, mon, 6 * 60 + 20, 6 * 60 + 25, &ms, s_idle, false, &ops, &at);
    CHECK(at == 6 * 60 + 20 && f.warm_begins == 1);
    /* ...but not once its start has passed */
    at_runner_init(&r);
    f.warm_begins = 0;
    warm_walk(&r, tl, 1, mon, 7 * 60 + 1, 7 * 60 + 30, &ms, s_idle, false, &ops, &at);
    CHECK(at < 0 && f.warm_begins == 0);

    /* daylight saving inside the window: the lead is on epoch seconds. Local 07:00 is an hour earlier in epoch terms
     * than the naive local clock says, so it fires at 05:12 local-naive */
    at_runner_init(&r);
    f.have_l2e = true;
    f.dst_shift_s = 3600;
    f.dst_from_lm = (int64_t)mon * 1440 + 6 * 60;
    ops = wops(&f);
    f.warm_begins = 0;
    warm_walk(&r, tl, 1, mon, 3 * 60, 6 * 60, &ms, s_idle, false, &ops, &at);
    CHECK(at == 5 * 60 + 12 && f.warm_begins == 1);
    f.have_l2e = false;
    ops = wops(&f);

    /* without a warm_plan op, a warm-by timer fires at its start like any thermostat timer */
    at_runner_init(&r);
    at_timer_ops_t plain_ops = {f_preheat, f_begin, f_end, &f, NULL, NULL, NULL, NULL, NULL};
    memset(&f.f, 0, sizeof(f.f));
    warm_walk(&r, tl, 1, mon, 4 * 60, 7 * 60 + 5, &ms, s_idle, false, &plain_ops, &at);
    CHECK(at == 7 * 60 && f.f.begins == 1);

    /* a timer saved before that clashes: held back to the end of the other's gap */
    at_timer_t two[2];
    two[0] = tl[0];
    two[1] = rep("e", 5 * 60 + 15, 30, WEEKDAYS);   /* 05:15 - 05:45, gap to 06:15 */
    two[1].mode = AT_TMODE_PREHEAT;
    at_runner_init(&r);
    f.lead_s = 100 * 60;   /* wants 05:20 */
    f.warm_begins = 0;
    memset(&f.f, 0, sizeof(f.f));
    for (int m = 4 * 60; m <= 7 * 60; m++) {
        at_localtime_t t = lt_at(mon, m);
        /* the preheat leaves the heater running until 05:45 */
        at_snapshot_t s = mk_snap(15, m > 5 * 60 + 15 && m <= 5 * 60 + 45 ? 3 : 0, 0);
        bool was = r.active && !strcmp(r.id, "wb");
        at_runner_tick(&r, two, 2, &t, ms += 60000, &s, r.active && r.mode == AT_TMODE_THERMOSTAT, &ops);
        if (!was && r.active && !strcmp(r.id, "wb")) at = m;
    }
    CHECK(f.f.preheats == 1 && f.warm_begins == 1 && at == 6 * 60 + 15 && f.last_hint == AT_WARM_RES_DELAYED);
}

/* at_ctl: the one start inside the range a warm-by timer may make */
static void test_warm_in_range(void)
{
    printf("warm by: one start inside the range\n");
    at_ctl_t t;
    hsim_t m;
    /* room 20.7, range 20..21: Auto alone waits (its start is halfway, 20.5, with nothing learned: 1.60) */
    hsim_init(&m, &t, 20.7f);
    m.rate_off = 0;
    hsim_run(&m, 30000);
    hsim_arm(&m);
    hsim_run(&m, 10 * MIN_MS);
    CHECK(m.l.starts == 0);
    /* armed by a warm-by timer that may start inside the range: starts at once */
    hsim_init(&m, &t, 20.7f);
    hsim_run(&m, 30000);
    at_ctl_allow_warm_start(&t, true);
    hsim_arm(&m);
    CHECK(m.l.starts == 1 && !t.warm_in_range);
    /* runs to the top with the 30 minute lock, then stops; the next start waits for the bottom again */
    m.rate_on = 0.02f;
    CHECK(HSIM_UNTIL(&m, m.state == 0 && m.l.stops >= 1, 120 * MIN_MS));
    CHECK(m.room >= 20.95f);
    m.rate_off = 0.01f;
    hsim_run(&m, 30 * MIN_MS);
    CHECK(m.l.starts == 1);   /* 21 less 0.3: above Auto's halfway start (20.5), no second start */
    /* above the stop point: no start even when allowed */
    hsim_init(&m, &t, 21.2f);
    hsim_run(&m, 30000);
    at_ctl_allow_warm_start(&t, true);
    hsim_arm(&m);
    hsim_run(&m, 5 * MIN_MS);
    CHECK(m.l.starts == 0);
    /* the allowance goes with Auto off */
    at_snapshot_t s = hsim_snap(&m);
    at_ctl_auto_off(&t, true, AT_HOLD_THERMO_OFF, AT_SRC_APP, &s, m.now, &m.ops);
    CHECK(!t.warm_in_range);

    /* a thermostat run feeds warm by its light-up time and rise rate */
    hsim_init(&m, &t, 19.0f);
    hsim_run(&m, 30 * MIN_MS);   /* idle readings first */
    hsim_arm(&m);
    m.rate_on = 0.03f;
    CHECK(HSIM_UNTIL(&m, m.l.stops >= 1 && m.state == 0, 180 * MIN_MS));
    CHECK(t.warm.st.nlight == 1 && t.warm.st.nrise[AT_SENS_EXT] == 1 && t.warm.st.ngross[AT_SENS_EXT] == 1);
    CHECK(NEAR(t.warm.st.rise[AT_SENS_EXT][0], 0.03f, 0.004f));
    CHECK(NEAR(t.warm.st.gross[AT_SENS_EXT][0], 0.03f + 0.02f, 0.008f));
}

static void test_warm(void)
{
    test_warm_tracker();
    test_warm_rates();
    test_warm_predict();
    test_warm_errors();
    test_warm_store_steady();
    test_warm_timer_rules();
    test_warm_window();
    test_warm_in_range();
}
