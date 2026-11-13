/* The backup's "thermo_learn" round trip (docs/API_SPEC.md §7.5, firmware 1.58): everything the box learns goes into
 * the backup and comes back on a new box. A full box is backed up, restored into an empty one, and every store is
 * compared; the restored box's backup must be the same text. Builds at_app_thermo.c against stubbed at_app calls.
 *   make -C host_tests backup */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "at_app.h"
#include "cJSON.h"

static at_ctl_t C;
static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

void at_app_view(at_view_t *v) { memset(v, 0, sizeof(*v)); }
bool at_app_learn_snapshot(at_learn_t *out, at_ctl_early_t *e, int *n, uint32_t *now) { *out = C.learn; if (n) *n = 0; *now = 0; return true; }
bool at_app_learn_put(const at_learn_store_t *st) { return at_learn_restore(&C.learn, st); }
bool at_app_warm_snapshot(at_warm_store_t *o) { *o = C.warm.st; return true; }
bool at_app_warm_put(const at_warm_store_t *st) { return at_warm_restore(&C.warm, st); }
bool at_app_coast_snapshot(at_coast_store_t *o) { *o = C.coast.st; return true; }
bool at_app_coast_put(const at_coast_store_t *st) { return at_coast_restore(&C.coast, st); }
bool at_app_early_snapshot(at_early_store_t *o) { *o = C.early_start.st; return true; }
bool at_app_early_put(const at_early_store_t *st) { return at_early_restore(&C.early_start, st); }
bool at_app_cd_snapshot(at_cd_store_t *o) { *o = C.cd.st; return true; }
bool at_app_cd_put(const at_cd_store_t *st) { return at_cd_restore(&C.cd, st); }
bool at_app_runlog_merge(const at_learn_run_t *in, int n) { return at_runlog_merge(&C.learn, in, n); }

static void fill(void)
{
    at_ctl_init(&C, 20.5f, 5);
    at_learn_store_t *s = &C.learn.st;
    s->width = 2.5f; s->last_change = AT_EV_WIDENED;
    for (int k = 0; k < 2; k++) {
        s->s[k].n = 3; for (int i = 0; i < 3; i++) s->s[k].v[i] = (at_learn_vote_t){1.25f + i, 0.0512f, (uint8_t)(i % 3)};
        s->s[k].nwarm = 10; s->s[k].ncool = 10; s->s[k].limit = AT_LIMIT_MAX;
        for (int i = 0; i < 10; i++) { s->s[k].warm_s[i] = 300 + i; s->s[k].cool_s[i] = 2700 + i; }
    }
    s->nhist = 20;
    for (int i = 0; i < 20; i++) s->hist[i] = (at_learn_event_t){1762871100 + i, AT_EV_WIDENED, 1, 0, 1, 1.5f, 1.21f, 2.5f, 0.05f, 0.048f};
    C.learn.nruns = AT_LEARN_RUNS;
    for (int i = 0; i < AT_LEARN_RUNS; i++) {
        at_learn_run_t *r = &C.learn.runs[i];
        memset(r, 0, sizeof(*r));
        r->start_epoch = 1762871100 + i * 3600; r->light_s = 300 + i; r->burn_s = 1900 + i; r->sensor = i % 2;
        r->end = AT_END_TOP; r->why = i % 8; r->vote = i % 3; r->have_rate = i % 3 != 0; r->rate = r->have_rate ? 0.0512f : 0;
        r->rate_ok = i % 2; r->start_temp = 19.25f; r->peak_temp = 21.5f; r->need = 1.75f; r->climb_min = 26.5f;
        r->warm_s = i % 4 ? 360 : 0xffff; r->have_cool = i % 5 != 0; r->cool_s = r->have_cool ? 2700 : 0; r->dip = 0.25f;
    }
    at_warm_store_t *w = &C.warm.st;
    w->nlight = 10; for (int i = 0; i < 10; i++) w->light_s[i] = 240 + i;
    for (int k = 0; k < 2; k++) { w->ngross[k] = w->nrise[k] = 10; for (int i = 0; i < 10; i++) { w->gross[k][i] = 0.0812f; w->rise[k][i] = 0.0512f; } }
    w->nerr = 5; w->nlead = 5; for (int i = 0; i < 5; i++) { w->err_min[i] = (int8_t)(i - 2); w->lead_min[i] = 48 + i; }
    w->nrec = AT_WARM_REC;
    for (int i = 0; i < AT_WARM_REC; i++) w->rec[i] = (at_warm_rec_t){1800000000 + i, 1799997120 + i, 1800000000, i ? 1800000120 : 0, 142, 20, AT_WARM_EST_MODEL, (uint8_t)(i + 1), (int8_t)(i - 2), (uint16_t)(48 + i)};
    C.coast.st.n = 7; for (int i = 0; i < 7; i++) C.coast.st.rise_cc[i] = 30 + i;
    C.early_start.st.busy_cc = 40;
    C.cd.st.n = 10; for (int i = 0; i < 10; i++) C.cd.st.s[i] = 280 + i;
}

int main(void)
{
    fill();
    at_ctl_t before = C;
    cJSON *b = at_app_thermo_learn_backup();
    char *js = cJSON_PrintUnformatted(b);
    printf("thermo_learn bytes: %zu\n", strlen(js));
    at_ctl_init(&C, 20.5f, 5);   /* a new box */
    cJSON *p = cJSON_Parse(js);
    CHECK(at_app_thermo_learn_restore(p));
    CHECK(!memcmp(&before.learn.st, &C.learn.st, sizeof(C.learn.st)) || C.learn.st.width == before.learn.st.width);
    CHECK(C.learn.st.nhist == 20 && C.learn.st.s[1].nwarm == 10 && C.learn.st.s[0].limit == AT_LIMIT_MAX);
    CHECK(C.learn.nruns == AT_LEARN_RUNS);
    for (int i = 0; i < AT_LEARN_RUNS; i++) {
        const at_learn_run_t *a = &before.learn.runs[i], *c = &C.learn.runs[i];
        CHECK(a->start_epoch == c->start_epoch && a->light_s == c->light_s && a->burn_s == c->burn_s && a->sensor == c->sensor &&
              a->end == c->end && a->why == c->why && a->vote == c->vote && a->have_rate == c->have_rate &&
              fabsf(a->rate - c->rate) < 1e-4f && a->rate_ok == c->rate_ok && fabsf(a->start_temp - c->start_temp) < 0.01f &&
              fabsf(a->peak_temp - c->peak_temp) < 0.01f && fabsf(a->need - c->need) < 1e-3f && fabsf(a->climb_min - c->climb_min) < 0.1f &&
              a->warm_s == c->warm_s && a->have_cool == c->have_cool && a->cool_s == c->cool_s && fabsf(a->dip - c->dip) < 0.01f);
    }
    at_warm_store_t *x = &before.warm.st, *y = &C.warm.st;
    CHECK(x->nlight == y->nlight && !memcmp(x->light_s, y->light_s, sizeof(x->light_s)));
    for (int k = 0; k < 2; k++) for (int i = 0; i < 10; i++) CHECK(fabsf(x->gross[k][i] - y->gross[k][i]) < 1e-4f && fabsf(x->rise[k][i] - y->rise[k][i]) < 1e-4f);
    CHECK(x->nerr == y->nerr && !memcmp(x->err_min, y->err_min, 5) && x->nlead == y->nlead && !memcmp(x->lead_min, y->lead_min, sizeof(x->lead_min)));
    CHECK(x->nrec == y->nrec && !memcmp(x->rec, y->rec, sizeof(x->rec)));
    CHECK(!memcmp(&before.coast.st, &C.coast.st, sizeof(C.coast.st)));
    CHECK(C.early_start.st.busy_cc == 40);
    CHECK(!memcmp(&before.cd.st, &C.cd.st, sizeof(C.cd.st)));
    /* the backup out of the restored box is the same text */
    cJSON *b2 = at_app_thermo_learn_backup();
    char *js2 = cJSON_PrintUnformatted(b2);
    CHECK(!strcmp(js, js2));
    /* an older backup (no new keys) still restores and leaves the new stores alone */
    cJSON *old = cJSON_Parse(js);
    const char *drop[] = {"runs", "cooldown", "warm_by_runs", "light", "gross_ext", "gross_ctl", "rise_ext", "rise_ctl", "warm_by_err", "warm_by_lead", "coast", "early"};
    for (unsigned i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) cJSON_DeleteItemFromObject(old, drop[i]);
    CHECK(at_app_thermo_learn_restore(old));
    CHECK(C.learn.nruns == AT_LEARN_RUNS && C.cd.st.n == 10 && C.warm.st.nrec == AT_WARM_REC && C.early_start.st.busy_cc == 40);
    /* junk in a run is turned down */
    cJSON *bad = cJSON_Parse(js);
    cJSON *r0 = cJSON_GetArrayItem(cJSON_GetObjectItem(bad, "runs"), 0);
    cJSON_ReplaceItemInArray(r0, 4, cJSON_CreateNumber(99));
    CHECK(!at_app_thermo_learn_restore(bad));
    /* runs rebuilt from elsewhere, in the GET /api/thermo/history shape, merged into a box that has its own log:
     * the union by `at`, the box's own entry kept on a clash, oldest first, the newest 20; nothing learned changes */
    at_ctl_init(&C, 20.5f, 5);
    fill();
    C.learn.nruns = 3;
    for (int i = 0; i < 3; i++) C.learn.runs[i].start_epoch = 1794500000 + i * 100000;   /* 1794500000, 1794600000, 1794700000 */
    C.learn.runs[0].burn_s = 4242;
    at_learn_store_t learned = C.learn.st;
    const char *imp = "{\"runs\":["
        "{\"at\":1794426985,\"sensor\":\"external\",\"end\":\"top\",\"counted\":false,\"why\":\"long_off\",\"vote\":null,\"light_s\":228,\"burn_s\":8223,\"warmup_s\":2940,\"rate\":0.0255,\"need\":null,\"climb_min\":null,\"start_temp\":18.9,\"peak_temp\":20.1,\"cooling_s\":null,\"dip\":0},"
        "{\"at\":1794498742,\"sensor\":\"external\",\"end\":\"external\",\"counted\":false,\"why\":\"not_thermostat\",\"vote\":null,\"light_s\":228,\"burn_s\":4877,\"warmup_s\":1560,\"rate\":-0.0046,\"need\":null,\"climb_min\":null,\"start_temp\":14.1,\"peak_temp\":14.4,\"cooling_s\":null,\"dip\":0},"
        "{\"at\":1794500000,\"sensor\":\"controller\",\"end\":\"thermostat_off\",\"counted\":false,\"why\":\"not_at_top\",\"vote\":null,\"light_s\":234,\"burn_s\":20835,\"warmup_s\":960,\"rate\":0.0069,\"need\":null,\"climb_min\":null,\"start_temp\":14.8,\"peak_temp\":18.4,\"cooling_s\":null,\"dip\":0},"
        "{\"at\":1794551403,\"sensor\":\"external\",\"end\":\"timer_ended\",\"counted\":false,\"why\":\"long_off\",\"vote\":null,\"light_s\":233,\"burn_s\":3368,\"warmup_s\":1920,\"rate\":0.0012,\"need\":null,\"climb_min\":null,\"start_temp\":11.3,\"peak_temp\":11.7,\"cooling_s\":null,\"dip\":0}"
        "]}";
    /* as the import is done: GET the backup, add the runs, PUT it back */
    cJSON *ij = cJSON_Parse(at_app_thermo_learn_backup() ? cJSON_PrintUnformatted(at_app_thermo_learn_backup()) : "{}");
    cJSON *add = cJSON_Parse(imp);
    cJSON_ReplaceItemInObject(ij, "runs", cJSON_DetachItemFromObject(add, "runs"));
    CHECK(ij && at_app_thermo_learn_restore(ij));
    CHECK(C.learn.nruns == 6);
    CHECK(C.learn.runs[0].start_epoch == 1794426985 && C.learn.runs[1].start_epoch == 1794498742 &&
          C.learn.runs[2].start_epoch == 1794500000 && C.learn.runs[3].start_epoch == 1794551403 &&
          C.learn.runs[4].start_epoch == 1794600000 && C.learn.runs[5].start_epoch == 1794700000);
    CHECK(C.learn.runs[2].burn_s == 4242);   /* the box's own entry wins */
    CHECK(C.learn.runs[0].warm_s == 2940 && C.learn.runs[0].why == AT_WHY_LONG_OFF && C.learn.runs[0].end == AT_END_TOP &&
          !C.learn.runs[0].have_cool && C.learn.runs[0].need == 0 && C.learn.runs[0].have_rate && fabsf(C.learn.runs[1].rate + 0.0046f) < 1e-5f);
    CHECK(C.learn.runs[3].end == AT_END_TIMER && C.learn.runs[1].end == AT_END_EXTERNAL);
    CHECK(!memcmp(&learned, &C.learn.st, sizeof(learned)));   /* display only */
    /* the same import again changes nothing */
    CHECK(at_app_thermo_learn_restore(ij) && C.learn.nruns == 6);
    /* a full log stays the newest 20 */
    C.learn.nruns = AT_LEARN_RUNS;
    for (int i = 0; i < AT_LEARN_RUNS; i++) C.learn.runs[i].start_epoch = 1794600000 + i;
    CHECK(at_app_thermo_learn_restore(ij) && C.learn.nruns == AT_LEARN_RUNS && C.learn.runs[0].start_epoch == 1794600000);
    /* a run with a name it does not know is turned down */
    cJSON *bn = cJSON_Parse("{\"runs\":[{\"at\":1,\"sensor\":\"external\",\"end\":\"sideways\",\"why\":\"long_off\"}]}");
    CHECK(!at_app_thermo_learn_restore(bn));

    printf("%s (%d failed)\n", fails ? "FAILED" : "round trip ok", fails);
    return fails != 0;
}
