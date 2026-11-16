/* The heater's own record (firmware 1.70): the operation report decoded from a real heater's EEPROM, the date each
 * fault was first seen, the backup's JSON round trip and its restore (twice changes nothing).
 *   make -C host_tests hrec */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "at_hrec.h"
#include "at_hrec_json.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

/* The cabin's Autoterm Flow 5D, read 2026-11-15 (GET /api/info heater_raw.eeprom, firmware 1.69) */
static const char *REAL =
    "55aa01138f1ed7ab000000000001c0015c0192002500090026083c000d3b0500161cd900000089000001a701690003000c0500000000"
    "00000d0502040108005b0072016c1a1a30000025020101002000200082012818181800000d01020400f5005b007a016c1e1f2000000f"
    "030203002b000a005a012f17171c00001e020302039f02b8007d022032372300004e030400447e00000080023d4a4d3400014e030400"
    "0a5200000081022047493100020d06020400f5005b00800150161621000025020101002000200081012b1c1c1a00004e0204030d2000"
    "00007a01723f3f330001000000001cfd0015000000000000000000000000005855002801000044fc";

static void unhex(const char *s, uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) sscanf(s + 2 * i, "%2hhx", &p[i]);
}

static void info_from(at_heater_info_t *hi, const uint8_t *e, uint32_t ms)
{
    static const uint8_t blk[] = {0x58, 0x00, 0x55, 0x01, 0x28, 0x00, 0x00, 0x00, 0x00};
    memset(hi, 0, sizeof(*hi));
    hi->have_version = true; hi->version[0] = 10; hi->version[1] = 1; hi->version[2] = 13; hi->version[3] = 3;
    hi->have_settings = true; memcpy(hi->settings, blk, sizeof(blk)); hi->settings_len = sizeof(blk); hi->settings_ms = ms;
    memcpy(hi->eeprom, e, AT_EEPROM_LEN); hi->pages = 0x0F; hi->eeprom_ms = ms;
    hi->have_read = true; hi->read_ms = ms;
}

static void report(void)
{
    printf("operation report from a real heater\n");
    CHECK(strlen(REAL) == AT_EEPROM_LEN * 2);
    uint8_t e[AT_EEPROM_LEN];
    unhex(REAL, e, sizeof(e));
    at_heater_report_t r;
    at_decode_heater_report(e, &r);
    CHECK(r.starts == 448 && r.starts_ok == 402 && r.starts_fault == 37 && r.power_cuts == 9);
    CHECK(r.starts == r.starts_ok + r.starts_fault + r.power_cuts);
    CHECK(r.first_attempt == 361 && r.second_attempt == 3 && r.no_ignition == 12);
    CHECK(r.total_s == 2492476 && r.high_s == 867077 && r.middle_s == 1449177 && r.low_s == 137 && r.wait_s == 423);
    CHECK(r.high_s + r.middle_s + r.low_s + r.wait_s <= r.total_s); /* the rest is lighting and purging */
    CHECK(r.pump_only_s == 7421 && r.water_pump_runs == 21 && r.warmup_runs == 0);
    CHECK(r.last_fault_column == 6);
    CHECK(r.product_code == 0x138f && r.serial == 0x1ed7ab);

    at_heater_fault_t f[AT_FAULT_HISTORY_N];
    int cols[AT_FAULT_HISTORY_N];
    int n = at_decode_heater_faults_cols(e, f, cols, AT_FAULT_HISTORY_N);
    CHECK(n == 10);
    CHECK(cols[0] == 5 && cols[1] == 4 && cols[5] == 0 && cols[6] == 9 && cols[9] == 6);
    CHECK(f[0].code == 78 && f[0].repeats == 3 && f[0].run_s == 17534 && fabsf(f[0].voltage - 12.8f) < 0.01f);
    CHECK(f[0].flame_temp == 300 && f[0].liquid_temp == 74 && f[0].overheat_temp == 77 && f[0].board_temp == 52);
    CHECK(f[0].flame_breaks == 1 && f[0].second_attempts == 0);
    for (int i = 0; i < n; i++) {
        CHECK(f[i].voltage >= 9.0f && f[i].voltage <= 13.5f);      /* every entry is a sane supply voltage */
        CHECK(f[i].liquid_temp < 100 && f[i].flame_temp > -40 && f[i].flame_temp < 800);
        if (f[i].code == 78) CHECK(f[i].flame_breaks >= 1);         /* flame break faults count their breaks */
        else CHECK(f[i].flame_breaks == 0);
    }
    /* the low voltage fault (15) was at 9.0 V; a cold-start overheat lock (37) with every sensor at 23-24 C */
    bool lowv = false, cold = false;
    for (int i = 0; i < n; i++) {
        if (f[i].code == 15) lowv = lowv || fabsf(f[i].voltage - 9.0f) < 0.01f;
        if (f[i].code == 37 && f[i].run_s == 32) cold = cold || (f[i].flame_temp == 23 && f[i].liquid_temp == 24);
    }
    CHECK(lowv && cold);
}

static void record(void)
{
    printf("heater record: fault dates, backup and restore\n");
    uint8_t e[AT_EEPROM_LEN];
    unhex(REAL, e, sizeof(e));
    at_heater_info_t hi;
    at_hrec_t r;
    at_hrec_init(&r);
    CHECK(at_hrec_preheat_temp(&r, 85.0f) == 85.0f);

    /* a read before the report is whole takes only the version and settings */
    info_from(&hi, e, 1000);
    hi.pages = 0x07;
    CHECK(at_hrec_take(&r, &hi, 1763200000));
    CHECK(r.have == (AT_HREC_HAVE_VER | AT_HREC_HAVE_SET) && r.settings_at == 1763200000 && r.read_at == 0);
    CHECK(at_hrec_preheat_temp(&r, 85.0f) == 88.0f);

    /* first whole report: every fault dated with it, "on or before" */
    info_from(&hi, e, 2000);
    CHECK(at_hrec_take(&r, &hi, 1763200100));
    CHECK(r.have == (AT_HREC_HAVE_VER | AT_HREC_HAVE_SET | AT_HREC_HAVE_REP) && r.read_at == 1763200100);
    for (int k = 0; k < 10; k++) CHECK(r.seen[k] == 1763200100);
    CHECK(r.first_read == 0x3FF);

    /* the same report later: dates stand, read_at moves */
    CHECK(at_hrec_take(&r, &hi, 1763290000));
    CHECK(r.seen[3] == 1763200100 && r.read_at == 1763290000 && r.first_read == 0x3FF);
    CHECK(!at_hrec_take(&r, &hi, 1763290000)); /* nothing new */

    /* a new fault in column 6 (the next one after the newest): only it gets the new date */
    uint8_t *c6 = e + AT_FAULT_HISTORY_AT + AT_FAULT_ENTRY_LEN * 6;
    memset(c6, 0, AT_FAULT_ENTRY_LEN);
    c6[0] = 13; c6[1] = 1; c6[9] = 0x7d;
    e[49] = 6;
    info_from(&hi, e, 3000);
    CHECK(at_hrec_take(&r, &hi, 1763300000));
    CHECK(r.seen[6] == 1763300000 && !(r.first_read & (1u << 6)) && r.seen[5] == 1763200100 && (r.first_read & (1u << 5)));
    /* the same fault repeating (its count goes up) dates it again */
    c6[1] = 2;
    info_from(&hi, e, 4000);
    CHECK(at_hrec_take(&r, &hi, 1763400000) && r.seen[6] == 1763400000);

    /* first report while the clock is not set: no dates yet, the next read with a clock gives them */
    at_hrec_t nc;
    at_hrec_init(&nc);
    CHECK(at_hrec_take(&nc, &hi, 0) && nc.seen[0] == 0 && nc.first_read == 0x3FF);
    CHECK(at_hrec_take(&nc, &hi, 1763500000) && nc.seen[0] == 1763500000 && nc.first_read == 0x3FF);

    /* JSON round trip */
    cJSON *j = at_hrec_to_json(&r);
    CHECK(j != NULL);
    char *txt = cJSON_PrintUnformatted(j);
    CHECK(txt && strlen(txt) < 800);
    at_hrec_t back;
    CHECK(at_hrec_from_json(j, &back));
    CHECK(!memcmp(&back, &r, sizeof(r)));

    /* restore into an empty replacement box: everything comes back; twice changes nothing */
    at_hrec_t nb;
    at_hrec_init(&nb);
    CHECK(at_hrec_restore(&nb, &back));
    CHECK(!memcmp(&nb, &r, sizeof(r)));
    CHECK(!at_hrec_restore(&nb, &back));
    cJSON *j2 = at_hrec_to_json(&nb);
    char *txt2 = cJSON_PrintUnformatted(j2);
    CHECK(txt2 && !strcmp(txt, txt2));

    /* the replacement box read the heater itself first (later dates): the backup's earlier dates win, its own
     * read and settings stand; a second restore changes nothing */
    at_hrec_t own;
    at_hrec_init(&own);
    info_from(&hi, e, 5000);
    hi.settings[0] = 80;
    at_hrec_take(&own, &hi, 1763600000);
    CHECK(own.seen[6] == 1763600000 && own.first_read == 0x3FF);
    CHECK(at_hrec_restore(&own, &back));
    CHECK(own.seen[6] == 1763400000 && !(own.first_read & (1u << 6)) && own.seen[0] == 1763200100 && (own.first_read & 1));
    CHECK(own.read_at == 1763600000 && own.settings[0] == 80 && at_hrec_preheat_temp(&own, 85.0f) == 80.0f);
    CHECK(!at_hrec_restore(&own, &back));

    /* a column that differs on the new box keeps its own date */
    at_hrec_t diff;
    at_hrec_init(&diff);
    c6[1] = 5;
    info_from(&hi, e, 6000);
    at_hrec_take(&diff, &hi, 1763700000);
    at_hrec_restore(&diff, &back);
    CHECK(diff.seen[6] == 1763700000 && diff.seen[5] == 1763200100);

    /* 1.73: a fault seen appearing in the heater's status dates its new entry to that moment, exactly */
    {
        uint8_t e2[AT_EEPROM_LEN];
        unhex(REAL, e2, sizeof(e2));
        at_hrec_t x;
        at_hrec_init(&x);
        info_from(&hi, e2, 7000);
        at_hrec_take(&x, &hi, 1763800000);
        CHECK(x.exact == 0 && x.first_read == 0x3FF);
        at_hrec_lives_t lv = {0};
        at_hrec_live_add(&lv, 78, 1763800500, 8000); /* a flame break, 11 s before the read */
        at_hrec_live_add(&lv, 15, 1763800400, 7900); /* a different code: not this column's */
        uint8_t *n6 = e2 + AT_FAULT_HISTORY_AT + AT_FAULT_ENTRY_LEN * 6;
        memset(n6, 0, AT_FAULT_ENTRY_LEN);
        n6[0] = 78; n6[1] = 1; n6[9] = 0x80;
        e2[49] = 6;
        info_from(&hi, e2, 8010);
        CHECK(at_hrec_take_live(&x, &hi, 1763800511, &lv));
        CHECK(x.seen[6] == 1763800500 && (x.exact & (1u << 6)) && !(x.first_read & (1u << 6)));
        CHECK(lv.n == 1 && lv.f[0].code == 15); /* the used one is taken out, the other waits */
        at_hrec_live_drop_before(&lv, 8010);
        CHECK(lv.n == 0);
        /* the same entry later, with no live time: it stays as it was */
        CHECK(!at_hrec_take_live(&x, &hi, 1763800511, &lv) && x.seen[6] == 1763800500 && (x.exact & (1u << 6)));
        /* it repeats while the box was not watching: dated by the read, no longer exact */
        n6[1] = 2;
        info_from(&hi, e2, 9000);
        CHECK(at_hrec_take_live(&x, &hi, 1763900000, &lv) && x.seen[6] == 1763900000 && !(x.exact & (1u << 6)));
        /* two new faults of one code in one read take the live times oldest column first */
        uint8_t *n7 = e2 + AT_FAULT_HISTORY_AT + AT_FAULT_ENTRY_LEN * 7;
        memset(n7, 0, AT_FAULT_ENTRY_LEN);
        n7[0] = 13; n7[1] = 1; n7[9] = 0x7f;
        uint8_t *n8 = e2 + AT_FAULT_HISTORY_AT + AT_FAULT_ENTRY_LEN * 8;
        memset(n8, 0, AT_FAULT_ENTRY_LEN);
        n8[0] = 13; n8[1] = 1; n8[9] = 0x7e;
        e2[49] = 8;
        at_hrec_live_add(&lv, 13, 1763950000, 9500);
        at_hrec_live_add(&lv, 13, 1763950300, 9800);
        info_from(&hi, e2, 9900);
        CHECK(at_hrec_take_live(&x, &hi, 1763950400, &lv));
        CHECK(x.seen[7] == 1763950000 && x.seen[8] == 1763950300 && (x.exact & (3u << 7)) == (3u << 7) && lv.n == 0);
        /* a live time later than the read cannot be this entry's */
        at_hrec_t y;
        at_hrec_init(&y);
        unhex(REAL, e2, sizeof(e2));
        info_from(&hi, e2, 1);
        at_hrec_take(&y, &hi, 1764000000);
        memset(n6, 0, AT_FAULT_ENTRY_LEN);
        n6[0] = 78; n6[1] = 1;
        e2[49] = 6;
        at_hrec_lives_t late = {0};
        at_hrec_live_add(&late, 78, 1764000900, 2);
        info_from(&hi, e2, 3);
        at_hrec_take_live(&y, &hi, 1764000800, &late);
        CHECK(y.seen[6] == 1764000800 && !(y.exact & (1u << 6)) && late.n == 1);
        /* a full list drops its oldest */
        at_hrec_lives_t full = {0};
        for (int i = 0; i < AT_HREC_LIVE_N + 2; i++) at_hrec_live_add(&full, (uint8_t)(i + 1), 1764000000u + (uint32_t)i, (uint32_t)i);
        CHECK(full.n == AT_HREC_LIVE_N && full.f[0].code == 3);
        /* exact survives the backup round trip and a restore, and restoring twice changes nothing */
        cJSON *jx = at_hrec_to_json(&x);
        at_hrec_t bx;
        CHECK(at_hrec_from_json(jx, &bx) && !memcmp(&bx, &x, sizeof(x)));
        at_hrec_t rx;
        at_hrec_init(&rx);
        CHECK(at_hrec_restore(&rx, &bx) && rx.exact == x.exact && !at_hrec_restore(&rx, &bx));
        /* a replacement box that read the heater later: the backup's earlier, exact date wins with its bit */
        at_hrec_t ox;
        at_hrec_init(&ox);
        info_from(&hi, x.eeprom, 1);
        memcpy(hi.eeprom, x.eeprom, AT_EEPROM_LEN);
        at_hrec_take(&ox, &hi, 1764500000);
        CHECK(at_hrec_restore(&ox, &bx) && ox.seen[7] == 1763950000 && (ox.exact & (1u << 7)) && !at_hrec_restore(&ox, &bx));
        cJSON_Delete(jx);
        /* a version 1 blob (exact was padding) comes up with exact cleared */
        at_hrec_t v1 = x;
        v1.version = 1;
        v1.exact = 0xFFFF;
        CHECK(at_hrec_upgrade(&v1) && v1.version == AT_HREC_VERSION && v1.exact == 0);
        v1.version = 9;
        CHECK(!at_hrec_upgrade(&v1));
    }

    /* junk in a backup is refused, not half taken */
    cJSON *bad = cJSON_Parse("{\"report\":\"zz\",\"settings\":\"58\"}");
    at_hrec_t bj;
    CHECK(!at_hrec_from_json(bad, &bj));
    cJSON_Delete(bad);

    cJSON_Delete(j);
    cJSON_Delete(j2);
    cJSON_free(txt);
    cJSON_free(txt2);
}

int main(void)
{
    report();
    record();
    printf(fails ? "%d FAILED\n" : "all heater record checks passed\n", fails);
    return fails ? 1 : 0;
}
