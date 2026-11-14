#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "at_app.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static void add_num_or_null(cJSON *o, const char *k, bool have, double v)
{
    if (have) {
        cJSON_AddNumberToObject(o, k, v);
    } else {
        cJSON_AddNullToObject(o, k);
    }
}

/* Settings are kept as floats: 0.15f would print as 0.150000006 */
static double round3(double v)
{
    return round(v * 1000.0) / 1000.0;
}

static void add_str_or_null(cJSON *o, const char *k, const char *s)
{
    if (s) {
        cJSON_AddStringToObject(o, k, s);
    } else {
        cJSON_AddNullToObject(o, k);
    }
}

/* ---- timers ------------------------------------------------------------------------------------------ */

static cJSON *timer_to_json(const at_timer_t *t)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", t->id);
    cJSON_AddBoolToObject(o, "enabled", t->enabled);
    cJSON_AddStringToObject(o, "mode", t->mode == AT_TMODE_THERMOSTAT ? "thermostat" : "preheat");
    cJSON_AddNumberToObject(o, "start_minute", t->start_minute);
    cJSON_AddNumberToObject(o, "duration_min", t->duration_min);
    cJSON *days = cJSON_AddArrayToObject(o, "days");
    if (!t->one_off) {
        for (int wd = 1; wd <= 7; wd++) {
            if (t->days & (1u << (wd - 1))) cJSON_AddItemToArray(days, cJSON_CreateNumber(wd));
        }
    }
    if (t->one_off) {
        char d[11];
        at_format_date(t->date_day, d);
        cJSON_AddStringToObject(o, "date", d);
    } else {
        cJSON_AddNullToObject(o, "date");
    }
    cJSON_AddNumberToObject(o, "low_c", t->low_c);
    cJSON_AddNumberToObject(o, "high_c", t->high_c);
    cJSON_AddBoolToObject(o, "warm_by", t->warm_by == 1);
    return o;
}

char *at_app_timers_json(void)
{
    at_timer_t *list = malloc(sizeof(at_timer_t) * AT_TIMERS_MAX);   /* ~1.8 KB: off the MQTT tasks' stacks */
    if (!list) return NULL;
    int n = at_app_timers_get(list, AT_TIMERS_MAX);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, timer_to_json(&list[i]));
    free(list);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}

static bool jint(const cJSON *item, int *out)
{
    if (!cJSON_IsNumber(item) || item->valuedouble != floor(item->valuedouble) || fabs(item->valuedouble) > 1e6) return false;
    *out = (int)item->valuedouble;
    return true;
}

/* One timer object into t; false with err filled (without the "timer N: " prefix). */
static bool timer_from_json(const cJSON *o, at_timer_t *t, char *err, size_t errn)
{
#define FAIL(msg) do { snprintf(err, errn, "%s", msg); return false; } while (0)
    memset(t, 0, sizeof(*t));
    t->enabled = true;
    t->low_c = 19;
    t->high_c = 22;
    if (!cJSON_IsObject(o)) FAIL("not an object");

    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, "id");
    if (!cJSON_IsString(i) || !i->valuestring[0] || strlen(i->valuestring) >= AT_TIMER_ID_LEN) FAIL("id must be a string of at most 36 characters");
    strcpy(t->id, i->valuestring);

    i = cJSON_GetObjectItemCaseSensitive(o, "enabled");
    if (i) {
        if (!cJSON_IsBool(i)) FAIL("enabled must be true or false");
        t->enabled = cJSON_IsTrue(i);
    }
    i = cJSON_GetObjectItemCaseSensitive(o, "mode");
    if (cJSON_IsString(i) && !strcmp(i->valuestring, "preheat")) t->mode = AT_TMODE_PREHEAT;
    else if (cJSON_IsString(i) && !strcmp(i->valuestring, "thermostat")) t->mode = AT_TMODE_THERMOSTAT;
    else FAIL("mode must be \"preheat\" or \"thermostat\"");

    int v;
    if (!jint(cJSON_GetObjectItemCaseSensitive(o, "start_minute"), &v) || v < 0 || v > 1439) FAIL("start_minute must be a whole number 0 to 1439");
    t->start_minute = (uint16_t)v;
    if (!jint(cJSON_GetObjectItemCaseSensitive(o, "duration_min"), &v) || v < 0 || v > 65535) FAIL("duration_min must be a whole number");
    t->duration_min = (uint16_t)v;

    i = cJSON_GetObjectItemCaseSensitive(o, "date");
    if (i && !cJSON_IsNull(i)) {
        if (!cJSON_IsString(i) || !at_parse_date(i->valuestring, &t->date_day)) FAIL("date must be a real yyyy-MM-dd or null");
        t->one_off = true;   /* days is ignored */
    } else {
        i = cJSON_GetObjectItemCaseSensitive(o, "days");
        if (!cJSON_IsArray(i)) FAIL("days must be an array of 1 (Monday) to 7 (Sunday)");
        const cJSON *d;
        cJSON_ArrayForEach(d, i) {
            if (!jint(d, &v) || v < 1 || v > 7) FAIL("days must be an array of 1 (Monday) to 7 (Sunday)");
            t->days |= (uint8_t)(1u << (v - 1));
        }
    }
    i = cJSON_GetObjectItemCaseSensitive(o, "low_c");
    if (i && (!jint(i, &v) || v < -128 || v > 127)) FAIL("low_c must be a whole number");
    if (i) t->low_c = (int8_t)v;
    i = cJSON_GetObjectItemCaseSensitive(o, "high_c");
    if (i && (!jint(i, &v) || v < -128 || v > 127)) FAIL("high_c must be a whole number");
    if (i) t->high_c = (int8_t)v;
    /* warm by (1.56): a sender that does not know it leaves it out, and the stored value is kept for that id. Only
     * an explicit false clears it */
    i = cJSON_GetObjectItemCaseSensitive(o, "warm_by");
    if (i && !cJSON_IsNull(i) && !cJSON_IsBool(i)) FAIL("warm_by must be true or false");
    t->warm_by = cJSON_IsBool(i) ? (cJSON_IsTrue(i) ? 1 : 0) : AT_TIMER_WARM_ABSENT;
    return true;
#undef FAIL
}

/* Validate and store a whole candidate list; the error and return code as for at_app_timers_put_json. */
static int store_list(const at_timer_t *list, int n, char *err, size_t err_len)
{
    at_timer_problem_t p;
    esp_err_t e = at_app_timers_set(list, n, &p);
    if (e == ESP_ERR_INVALID_ARG) {
        if (p.index >= 0 && p.other >= 0) {
            snprintf(err, err_len, "timer %d: %s (timer %d)", p.index + 1, at_timer_err_text(p.err), p.other + 1);
        } else if (p.index >= 0) {
            snprintf(err, err_len, "timer %d: %s", p.index + 1, at_timer_err_text(p.err));
        } else {
            snprintf(err, err_len, "%s", at_timer_err_text(p.err));
        }
        return 400;
    }
    if (e != ESP_OK) {
        snprintf(err, err_len, "could not save the timers");
        return 500;
    }
    return 200;
}

int at_app_timers_put_json(const char *json, char *err, size_t err_len)
{
    cJSON *arr = cJSON_Parse(json);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        snprintf(err, err_len, "body must be a JSON array of timers");
        return 400;
    }
    int n = cJSON_GetArraySize(arr);
    if (n > AT_TIMERS_MAX) {
        cJSON_Delete(arr);
        snprintf(err, err_len, "%s", at_timer_err_text(AT_TERR_TOO_MANY));
        return 400;
    }
    at_timer_t *list = malloc(sizeof(at_timer_t) * AT_TIMERS_MAX);   /* off the (possibly small) task stack */
    if (!list) {
        cJSON_Delete(arr);
        snprintf(err, err_len, "out of memory");
        return 500;
    }
    char why[96];
    for (int k = 0; k < n; k++) {
        if (!timer_from_json(cJSON_GetArrayItem(arr, k), &list[k], why, sizeof(why))) {
            snprintf(err, err_len, "timer %d: %s", k + 1, why);
            cJSON_Delete(arr);
            free(list);
            return 400;
        }
    }
    cJSON_Delete(arr);
    int code = store_list(list, n, err, err_len);
    free(list);
    return code;
}

/* One timer at a time, for the cloud (a whole list does not fit in a signed command): timer_set replaces the
 * timer with the same id or adds it at the end, timer_delete removes it. The rest of the list is the box's own. */
int at_app_timer_set_json(const cJSON *timer, char *err, size_t err_len)
{
    at_timer_t t;
    char why[96];
    if (!timer_from_json(timer, &t, why, sizeof(why))) {
        snprintf(err, err_len, "timer: %s", why);
        return 400;
    }
    at_timer_t *list = malloc(sizeof(at_timer_t) * AT_TIMERS_MAX);
    if (!list) {
        snprintf(err, err_len, "out of memory");
        return 500;
    }
    int n = at_app_timers_get(list, AT_TIMERS_MAX);
    int k = 0;
    while (k < n && strcmp(list[k].id, t.id) != 0) k++;
    if (k == AT_TIMERS_MAX) {
        free(list);
        snprintf(err, err_len, "%s", at_timer_err_text(AT_TERR_TOO_MANY));
        return 400;
    }
    list[k] = t;
    int code = store_list(list, k == n ? n + 1 : n, err, err_len);
    free(list);
    return code;
}

int at_app_timer_delete(const char *id, char *err, size_t err_len)
{
    at_timer_t *list = malloc(sizeof(at_timer_t) * AT_TIMERS_MAX);
    if (!list) {
        snprintf(err, err_len, "out of memory");
        return 500;
    }
    int n = at_app_timers_get(list, AT_TIMERS_MAX);
    int k = 0;
    while (id && k < n && strcmp(list[k].id, id) != 0) k++;
    if (!id || k == n) {
        free(list);
        snprintf(err, err_len, "no timer with that id");
        return 404;
    }
    memmove(&list[k], &list[k + 1], (size_t)(n - k - 1) * sizeof(at_timer_t));
    int code = store_list(list, n - 1, err, err_len);
    free(list);
    return code;
}

char *at_app_backup_json(void)
{
    at_view_t v;
    at_app_view(&v);
    at_fuel_view_t fv;
    at_app_fuel_view(&fv);
    at_timer_status_t ts;
    at_app_timer_status(&ts);
    char name[AT_DISPLAY_NAME_MAX];
    at_settings_display_name(name, sizeof(name));

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddStringToObject(o, "display_name", name);
    cJSON_AddNumberToObject(o, "auto_low", round3(v.auto_low));
    cJSON_AddNumberToObject(o, "auto_high", round3(v.auto_high));
    cJSON_AddNumberToObject(o, "auto_target", round3(v.auto_target));
    cJSON_AddNumberToObject(o, "hyst_min", round3(v.hyst_min));
    cJSON_AddNumberToObject(o, "hyst_max", round3(v.hyst_max));
    cJSON_AddBoolToObject(o, "use_external_temp", v.use_external_temp);
    cJSON_AddNumberToObject(o, "preheat_temp", round3(v.preheat_temp));
    cJSON_AddNumberToObject(o, "preheat_minutes", v.preheat_minutes);
    cJSON_AddNumberToObject(o, "pump_minutes", at_app_pump_minutes());
    cJSON_AddBoolToObject(o, "freeze_enabled", v.freeze_enabled);
    cJSON_AddNumberToObject(o, "freeze_target", round3(v.freeze_target));
    cJSON_AddStringToObject(o, "timezone", ts.timezone);
    cJSON_AddBoolToObject(o, "auto_update", at_settings_auto_update());
    cJSON_AddBoolToObject(o, "mqtt", at_settings_mqtt());
    cJSON_AddNumberToObject(o, "fuel_price", round3(fv.cfg.price));
    cJSON_AddNumberToObject(o, "fuel_factor", round3(fv.cfg.factor));
    cJSON *rates = cJSON_AddObjectToObject(o, "fuel_rates");
    for (int l = AT_FUEL_STARTING; l < AT_FUEL_LEVELS; l++) {
        cJSON_AddNumberToObject(rates, at_fuel_level_name((at_fuel_level_t)l), round3(fv.cfg.rate_lph[l]));
    }
    cJSON_AddNumberToObject(o, "ml_per_pulse", round(fv.pump.ml_per_pulse * 1e5) / 1e5);
    cJSON_AddNumberToObject(o, "tank_capacity", round3(fv.tank.capacity_l));
    at_battery_view_t bv;
    at_app_battery_view(&bv);
    cJSON_AddNumberToObject(o, "battery_factor", round3(bv.factor));

    cJSON *learn = at_app_thermo_learn_backup();
    if (learn) cJSON_AddItemToObject(o, "thermo_learn", learn);
    cJSON *care = at_app_thermo_care_backup();
    if (care) cJSON_AddItemToObject(o, "heater_care", care);

    char *timers = at_app_timers_json();
    cJSON *arr = timers ? cJSON_Parse(timers) : NULL;
    free(timers);
    cJSON_AddItemToObject(o, "timers", arr ? arr : cJSON_CreateArray());

    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    /* "fuel" (1.65): the whole fuel history, spliced in as text after printing, so its year of days never becomes
     * a cJSON tree on the box (at_app_fuel_cloud_json) */
    char *fuel = str ? at_app_fuel_cloud_json() : NULL;
    if (fuel) {
        size_t a = strlen(str), f = strlen(fuel);
        char *both = (a >= 2 && str[a - 1] == '}') ? malloc(a + f + 10) : NULL;
        if (both) {
            memcpy(both, str, a - 1);
            memcpy(both + a - 1, ",\"fuel\":", 8);
            memcpy(both + a + 7, fuel, f);
            memcpy(both + a + 7 + f, "}", 2);
            free(str);
            str = both;
        }
        free(fuel);
    }
    return str;
}

/* GET /api/fuel: the daily log, newest first, with local dates. */
char *at_app_fuel_json(void)
{
    at_fuel_day_t *days = malloc(sizeof(at_fuel_day_t) * AT_FUEL_DAYS);
    if (!days) return NULL;
    int n = at_app_fuel_log_copy(days, AT_FUEL_DAYS);
    int32_t det[AT_FUEL_DETAIL_DAYS];
    int nd = at_app_fuel_detail_days(det, AT_FUEL_DETAIL_DAYS);
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "days");
    for (int i = 0; i < n; i++) {
        int y, m, d;
        at_civil_from_days(days[i].day, &y, &m, &d);
        char date[12];
        snprintf(date, sizeof(date), "%04d-%02d-%02d", y, m, d);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "date", date);
        cJSON_AddNumberToObject(e, "litres", days[i].ml / 1000.0);
        cJSON_AddNumberToObject(e, "cost", days[i].cost_m / 1000.0);
        cJSON_AddNumberToObject(e, "burn_min", days[i].burn_min);
        cJSON_AddNumberToObject(e, "starts", days[i].starts);
        for (int k = 0; k < nd; k++) {
            if (det[k] == days[i].day) { cJSON_AddBoolToObject(e, "detail", true); break; } /* GET /api/fuel/day has it (1.64) */
        }
        cJSON_AddItemToArray(arr, e);
    }
    free(days);
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return str;
}

/* GET /api/fuel/day?date=: one day by power level (1.64). Each level's cost is its litres at the day's cost per litre
 * (as repriced), so a part day (the day of the update) charges each level only for its own fuel. `hours` is 24 rows of minutes at
 * starting, low, middle, high, by local hour. */
char *at_app_fuel_day_json(int32_t day)
{
    at_fuel_detail_day_t det;
    at_fuel_day_t log;
    if (!at_app_fuel_detail(day, &det, &log)) return NULL;
    char date[11];
    at_format_date(day, date);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "date", date);
    cJSON_AddNumberToObject(o, "litres", log.ml / 1000.0);
    cJSON_AddNumberToObject(o, "cost", log.cost_m / 1000.0);
    cJSON_AddNumberToObject(o, "burn_min", log.burn_min);
    cJSON_AddNumberToObject(o, "starts", log.starts);
    cJSON *lv = cJSON_AddArrayToObject(o, "levels");
    for (int i = 0; i < AT_FUEL_BURN_LEVELS; i++) {
        unsigned mins = 0;
        for (int h = 0; h < 24; h++) mins += det.min[h][i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "level", at_fuel_level_name((at_fuel_level_t)(AT_FUEL_STARTING + i)));
        cJSON_AddNumberToObject(e, "minutes", mins);
        cJSON_AddNumberToObject(e, "litres", det.ml[i] / 1000.0);
        cJSON_AddNumberToObject(e, "cost", round3(at_fuel_level_cost(&log, &det, i)));
        cJSON_AddItemToArray(lv, e);
    }
    cJSON *hrs = cJSON_AddArrayToObject(o, "hours");
    for (int h = 0; h < 24; h++) {
        cJSON *row = cJSON_CreateArray();
        for (int i = 0; i < AT_FUEL_BURN_LEVELS; i++) cJSON_AddItemToArray(row, cJSON_CreateNumber(det.min[h][i]));
        cJSON_AddItemToArray(hrs, row);
    }
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return str;
}

/* "heater_info" in the status: what the heater said about itself, decoded (docs/API_SPEC.md §9.8). Only once
 * something has been read or overheard, so a box that never asked sends nothing extra. */
static void add_heater_info(cJSON *o)
{
    at_heater_info_t *hi = malloc(sizeof(*hi));
    if (!hi) return;
    uint32_t now;
    at_app_heater_info(hi, &now);
    bool report = hi->pages == (1u << AT_EEPROM_PAGES) - 1;
    if (!hi->have_version && !hi->have_settings && !report && !hi->have_read && !hi->have_unlock) {
        free(hi);
        return;
    }
    cJSON *h = cJSON_AddObjectToObject(o, "heater_info");
    cJSON_AddBoolToObject(h, "reading", hi->pending > 0);
    add_num_or_null(h, "read_age_s", hi->have_read, (now - hi->read_ms) / 1000);
    if (hi->have_version) {
        char v[24];
        snprintf(v, sizeof(v), "%u.%u.%u.%u", hi->version[0], hi->version[1], hi->version[2], hi->version[3]);
        cJSON_AddStringToObject(h, "version", v);
        cJSON_AddNumberToObject(h, "device_type", hi->version[0]);
    }
    at_heater_settings_t hs;
    if (hi->have_settings && at_decode_heater_settings(hi->settings, hi->settings_len, &hs)) {
        static const char *const warm[] = {"off", "auto", "manual"};
        cJSON *st = cJSON_AddObjectToObject(h, "settings");
        cJSON_AddNumberToObject(st, "wait_temp", hs.wait_temp);
        cJSON_AddStringToObject(st, "warmup", hs.warmup_mode >= 0 && hs.warmup_mode <= 2 ? warm[hs.warmup_mode] : "unknown");
        cJSON_AddNumberToObject(st, "warmup_wait_temp", hs.warmup_wait_temp);
        cJSON_AddStringToObject(st, "relay", hs.relay_auto ? "auto" : "off");
        cJSON_AddNumberToObject(st, "relay_on_temp", hs.relay_on_temp);
        cJSON_AddStringToObject(st, "pump_signal", hs.pump_impulse ? "impulse" : "constant");
        cJSON_AddBoolToObject(st, "pump_with_engine", hs.pump_with_engine);
        cJSON_AddBoolToObject(st, "pump_standby", hs.pump_standby);
        cJSON_AddBoolToObject(st, "signal_channel", hs.signal_channel);
        cJSON_AddNumberToObject(st, "age_s", (now - hi->settings_ms) / 1000);
    }
    if (report) {
        at_heater_report_t r;
        at_decode_heater_report(hi->eeprom, &r);
        cJSON *rp = cJSON_AddObjectToObject(h, "report");
        cJSON_AddNumberToObject(rp, "product_code", r.product_code);
        cJSON_AddNumberToObject(rp, "serial", r.serial);
        cJSON_AddNumberToObject(rp, "starts", r.starts);
        cJSON_AddNumberToObject(rp, "starts_ok", r.starts_ok);
        cJSON_AddNumberToObject(rp, "starts_fault", r.starts_fault);
        cJSON_AddNumberToObject(rp, "first_attempt", r.first_attempt);
        cJSON_AddNumberToObject(rp, "second_attempt", r.second_attempt);
        cJSON_AddNumberToObject(rp, "no_ignition", r.no_ignition);
        cJSON_AddNumberToObject(rp, "power_cuts", r.power_cuts);
        cJSON_AddNumberToObject(rp, "water_pump_runs", r.water_pump_runs);
        cJSON_AddNumberToObject(rp, "warmup_runs", r.warmup_runs);
        cJSON_AddNumberToObject(rp, "total_s", r.total_s);
        cJSON_AddNumberToObject(rp, "high_s", r.high_s);
        cJSON_AddNumberToObject(rp, "middle_s", r.middle_s);
        cJSON_AddNumberToObject(rp, "low_s", r.low_s);
        cJSON_AddNumberToObject(rp, "wait_s", r.wait_s);
        cJSON_AddNumberToObject(rp, "pump_only_s", r.pump_only_s);
        cJSON_AddNumberToObject(rp, "last_fault_column", r.last_fault_column);
        cJSON_AddNumberToObject(rp, "age_s", (now - hi->eeprom_ms) / 1000);
        at_heater_fault_t f[AT_FAULT_HISTORY_N];
        int nf = at_decode_heater_faults(hi->eeprom, f, AT_FAULT_HISTORY_N);
        cJSON *fa = cJSON_AddArrayToObject(rp, "faults");
        for (int i = 0; i < nf; i++) {
            cJSON *e = cJSON_CreateObject();
            const char *name = at_fault_name(f[i].code);
            cJSON_AddNumberToObject(e, "code", f[i].code);
            cJSON_AddStringToObject(e, "name", name ? name : "unknown");
            cJSON_AddNumberToObject(e, "repeats", f[i].repeats);
            cJSON_AddStringToObject(e, "mode", at_mode_name(f[i].state, f[i].substate));
            cJSON_AddNumberToObject(e, "run_s", f[i].run_s);
            cJSON_AddNumberToObject(e, "mode_s", f[i].mode_s);
            cJSON_AddNumberToObject(e, "voltage", round3(f[i].voltage));
            cJSON_AddNumberToObject(e, "flame_temp", f[i].flame_temp);
            cJSON_AddNumberToObject(e, "liquid_temp", f[i].liquid_temp);
            cJSON_AddNumberToObject(e, "overheat_temp", f[i].overheat_temp);
            cJSON_AddNumberToObject(e, "board_temp", f[i].board_temp);
            cJSON_AddNumberToObject(e, "second_attempts", f[i].second_attempts);
            cJSON_AddNumberToObject(e, "flame_breaks", f[i].flame_breaks);
            cJSON_AddItemToArray(fa, e);
        }
    }
    if (hi->have_unlock) {
        cJSON_AddNumberToObject(h, "unlock_age_s", (now - hi->unlock_ms) / 1000);
        cJSON_AddBoolToObject(h, "unlock_replied", hi->unlock_replied);
    }
    free(hi);
}

/* Heater Care (docs/API_SPEC.md, "thermo"): the working range, the locks and what has been learned. */
static void add_thermo(cJSON *root, const at_view_t *v)
{
    const at_ctl_status_t *t = &v->thermo;
    const int64_t ep = v->clock_ok ? v->epoch : 0;
    cJSON *o = cJSON_AddObjectToObject(root, "thermo");
    if (!o) return;
    cJSON_AddNumberToObject(o, "work_low", round3(t->work_low));
    cJSON_AddNumberToObject(o, "work_high", round3(t->work_high));
    cJSON_AddNumberToObject(o, "learned_width", round3(t->learned_width));
    cJSON_AddNumberToObject(o, "min_width", round3(t->min_width));
    cJSON_AddStringToObject(o, "sensor", at_learn_sensor_name((at_sensor_t)t->sensor));
    cJSON_AddNumberToObject(o, "ceiling", round3(t->ceiling));
    add_str_or_null(o, "at_limit", at_learn_limit_name((at_limit_t)t->at_limit));
    cJSON_AddStringToObject(o, "reason", t->reason);
    cJSON_AddNumberToObject(o, "min_run_left_s", t->min_run_left_s);
    cJSON_AddNumberToObject(o, "min_off_left_s", t->min_off_left_s);
    add_str_or_null(o, "holding", at_ctl_hold_name((at_hold_t)t->holding));
    add_num_or_null(o, "run_on_at", t->running && ep, (double)(ep - t->run_on_ago_s));
    add_num_or_null(o, "flame_on_at", t->running && t->flame && ep, (double)(ep - t->flame_on_ago_s));
    cJSON_AddBoolToObject(o, "run_adopted", t->run_adopted);
    cJSON_AddBoolToObject(o, "sensor_stuck", t->sensor_stuck);
    cJSON_AddBoolToObject(o, "no_reading", t->no_reading);
    add_num_or_null(o, "rise_rate", t->have_rate, round(t->rise_rate * 10000.0) / 10000.0);
    add_num_or_null(o, "warmup_typical_s", t->have_warm, t->warmup_typical_s);
    add_num_or_null(o, "cool_typical_s", t->have_cool, t->cool_typical_s);
    cJSON_AddNumberToObject(o, "stop_at", round(t->stop_at * 10.0) / 10.0);
    cJSON_AddNumberToObject(o, "stop_early", round(t->stop_early * 10.0) / 10.0);
    add_num_or_null(o, "coast_typical", t->have_coast, round(t->coast_typical * 100.0) / 100.0);
    cJSON_AddNumberToObject(o, "coast_samples", t->coast_samples);
    /* early start inside the range (1.60, docs/EARLY_START.md) */
    cJSON *es = cJSON_AddObjectToObject(o, "early");
    if (es) {
        cJSON_AddNumberToObject(es, "start_at", round(t->early.start_at * 10.0) / 10.0);
        cJSON_AddStringToObject(es, "est", at_early_est_name((at_early_est_t)t->early.est));
        add_num_or_null(es, "net_rate", t->early.have_rate, round(t->early.net_rate * 10000.0) / 10000.0);
        add_num_or_null(es, "climb", t->early.have_rate, round(t->early.climb * 100.0) / 100.0);
        cJSON_AddNumberToObject(es, "busy_w", round(t->early.busy_w * 100.0) / 100.0);
    }
    cJSON_AddNumberToObject(o, "dip_below", round3(t->dip_below));
    cJSON_AddNumberToObject(o, "early_stop_unseen", t->early_stop_unseen);
    add_str_or_null(o, "last_run_end", t->last_run_end ? at_learn_end_name((at_run_end_t)t->last_run_end) : NULL);
    if (t->have_last_early) {
        cJSON *e = cJSON_AddObjectToObject(o, "last_early_stop");
        add_num_or_null(e, "at", ep != 0, (double)(ep - t->last_early_ago_s));
        cJSON_AddNumberToObject(e, "after_s", t->last_early.after_s);
        cJSON_AddStringToObject(e, "source", at_ctl_src_name((at_src_t)t->last_early.src));
    } else {
        cJSON_AddNullToObject(o, "last_early_stop");
    }
}

static cJSON *build_state_tree(void)
{
    at_view_t v;
    at_app_view(&v);
    const at_snapshot_t *s = &v.snap;
    cJSON *o = cJSON_CreateObject();

    add_str_or_null(o, "state", s->have_status ? (s->state_name ? s->state_name : "unknown") : NULL);
    add_num_or_null(o, "state_raw", s->have_status, s->status.state_raw);
    add_num_or_null(o, "substate", s->have_status, s->status.substate);
    add_num_or_null(o, "fault", s->have_status, s->status.fault);
    add_str_or_null(o, "fault_name", s->have_status && s->status.fault ? (s->fault_name ? s->fault_name : "unknown") : NULL);
    cJSON_AddBoolToObject(o, "fault_active", s->have_status && s->status.fault);
    add_num_or_null(o, "coolant_temp", s->have_status, s->status.coolant_temp);
    add_num_or_null(o, "elapsed_min", s->have_status, s->status.elapsed_min);
    cJSON_AddBoolToObject(o, "burner_active", s->have_status && s->status.burner_active);
    add_num_or_null(o, "cabin_temp", s->have_cabin, s->cabin_temp);
    cJSON_AddBoolToObject(o, "stale", s->stale);
    add_num_or_null(o, "status_age_s", s->have_status, s->status_age_ms / 1000.0);

    add_num_or_null(o, "last_fault_code", s->have_last_fault, s->last_fault_code);
    add_str_or_null(o, "last_fault_name", s->have_last_fault ? (s->last_fault_name ? s->last_fault_name : "unknown") : NULL);
    add_num_or_null(o, "last_fault_ago_s", s->have_last_fault, s->last_fault_ago_ms / 1000.0);

    /* the heater's own cooldown (1.52): when it began (wall clock, so the apps and the cloud count down from the
     * same moment however late they look) and how long it usually takes, learned from past cooldowns */
    {
        const at_ctl_status_t *t = &v.thermo;
        const int64_t ep = v.clock_ok ? v.epoch : 0;
        cJSON_AddBoolToObject(o, "cooling", t->cooling);
        add_num_or_null(o, "cooldown_at", t->have_cooldown_elapsed && ep, (double)(ep - t->cooldown_elapsed_s));
        add_num_or_null(o, "cooldown_elapsed_s", t->have_cooldown_elapsed, t->cooldown_elapsed_s);
        cJSON_AddNumberToObject(o, "cooldown_typical_s", t->cooldown_typical_s);
        cJSON_AddBoolToObject(o, "cooldown_learned", t->cooldown_learned);
        cJSON_AddNumberToObject(o, "cooldown_samples", t->cooldown_samples);
    }

    add_num_or_null(o, "effective_temp", v.have_effective_temp, v.effective_temp);
    cJSON_AddBoolToObject(o, "external_temp_active", v.have_effective_temp && v.effective_is_external);
    add_num_or_null(o, "external_temp", v.have_external, v.external_temp);
    cJSON_AddBoolToObject(o, "use_external_temp", v.use_external_temp);
    /* what the heater's own thermostat is told, controller's reading or the room sensor in its place */
    add_num_or_null(o, "heater_temp", s->room_to_heater || s->have_cabin, s->room_to_heater ? s->room_temp : s->cabin_temp);
    cJSON_AddBoolToObject(o, "heater_temp_external", s->room_to_heater);

    cJSON_AddBoolToObject(o, "auto_enabled", v.auto_enabled);
    cJSON_AddNumberToObject(o, "auto_target", v.auto_target);
    cJSON_AddNumberToObject(o, "auto_low", v.auto_low);
    cJSON_AddNumberToObject(o, "auto_high", v.auto_high);
    cJSON_AddNumberToObject(o, "hyst_min", v.hyst_min);
    cJSON_AddNumberToObject(o, "hyst_max", v.hyst_max);
    cJSON_AddNumberToObject(o, "auto_start_at", round(v.auto_start_at * 10.0) / 10.0);
    add_thermo(o, &v);
    cJSON_AddNumberToObject(o, "preheat_temp", v.preheat_temp);
    cJSON_AddStringToObject(o, "model", CONFIG_AUTOTERM_MODEL);
    cJSON_AddStringToObject(o, "hw_rev", CONFIG_AUTOTERM_HW_REV);
    cJSON_AddStringToObject(o, "os_name", "RoamOS");
    cJSON_AddStringToObject(o, "os_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "auto_note", v.auto_note);
    cJSON_AddBoolToObject(o, "prevent_freezing_enabled", v.freeze_enabled);
    cJSON_AddNumberToObject(o, "prevent_freezing_target", v.freeze_target);
    cJSON_AddStringToObject(o, "prevent_freezing_note", v.freeze_note);
    cJSON_AddNumberToObject(o, "preheat_minutes", v.preheat_minutes);
    cJSON_AddBoolToObject(o, "auto_update", at_settings_auto_update());
    cJSON_AddBoolToObject(o, "mqtt", at_settings_mqtt());
    cJSON_AddNumberToObject(o, "pump_minutes", at_app_pump_minutes());
    add_heater_info(o);

    cJSON_AddBoolToObject(o, "bypass_mode", s->bypass);
    cJSON_AddBoolToObject(o, "debug_mode", v.debug_mode);
    cJSON_AddNumberToObject(o, "debug_interval", v.debug_interval_s);
    cJSON_AddBoolToObject(o, "extended_active", s->have_ext && s->ext_active);
    cJSON_AddNumberToObject(o, "ext_frames", s->have_ext ? s->ext_frames : 0);
    if (s->have_ext) cJSON_AddNumberToObject(o, "ext_first_s", s->ext_first_ms / 1000.0);

    add_str_or_null(o, "last_command", s->have_last_cmd ? s->last_cmd : NULL);
    cJSON_AddBoolToObject(o, "last_command_replied", s->have_last_cmd && s->last_cmd_replied);
    cJSON_AddNumberToObject(o, "queue", s->queue_len);
    cJSON_AddBoolToObject(o, "panel_stop_hold", s->panel_stop_hold);
    cJSON_AddBoolToObject(o, "hearth_run", s->hearth_run);
    add_str_or_null(o, "run_mode", s->run_mode == AT_RUN_THERMOSTAT ? "thermostat"
                                   : s->run_mode == AT_RUN_PREHEAT  ? "preheat"
                                                                    : NULL);

    cJSON_AddBoolToObject(o, "controller_seen", s->seen_panel);
    cJSON_AddBoolToObject(o, "heater_seen", s->seen_heater);
    cJSON_AddBoolToObject(o, "standalone", s->standalone);
    cJSON_AddNumberToObject(o, "own_polls", s->own_polls);
    cJSON_AddNumberToObject(o, "frames_controller", s->frames_panel);
    cJSON_AddNumberToObject(o, "frames_heater", s->frames_heater);
    cJSON_AddNumberToObject(o, "bad_crc_controller", s->bad_crc_panel);
    cJSON_AddNumberToObject(o, "bad_crc_heater", s->bad_crc_heater);
    cJSON_AddNumberToObject(o, "resyncs_controller", s->resyncs_panel);
    cJSON_AddNumberToObject(o, "resyncs_heater", s->resyncs_heater);
    cJSON_AddNumberToObject(o, "inject_ok", s->inject_ok);
    cJSON_AddNumberToObject(o, "inject_fail", s->inject_fail);
    int rx_high[2], rx_edges[2];
    at_app_rx_probe(rx_high, rx_edges);
    cJSON_AddNumberToObject(o, "rx_heater_high_pct", rx_high[0]);
    cJSON_AddNumberToObject(o, "rx_heater_edges", rx_edges[0]);
    cJSON_AddNumberToObject(o, "rx_controller_high_pct", rx_high[1]);
    cJSON_AddNumberToObject(o, "rx_controller_edges", rx_edges[1]);
    cJSON_AddNumberToObject(o, "rx_heater_idle_mv", at_app_heater_rx_mv());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));

    at_timer_status_t ts;
    at_app_timer_status(&ts);
    cJSON_AddBoolToObject(o, "clock_ok", ts.clock_ok);
    cJSON_AddStringToObject(o, "timezone", ts.timezone);
    if (ts.have_next) {
        cJSON *nx = cJSON_AddObjectToObject(o, "next_timer");
        cJSON_AddStringToObject(nx, "id", ts.next_id);
        cJSON_AddNumberToObject(nx, "start_epoch", (double)ts.next_start_epoch);
        cJSON_AddBoolToObject(nx, "warm_by", ts.next_warm_by);
        if (ts.next_warm_by) {
            /* changes by the minute: status only, never the backup (the cloud copy is sent when the backup changes) */
            cJSON_AddNumberToObject(nx, "expected_start_epoch", (double)ts.next_expected_epoch);
            add_str_or_null(nx, "estimate", at_warm_est_name((at_warm_est_t)ts.next_estimate));
        }
    } else {
        cJSON_AddNullToObject(o, "next_timer");
    }
    add_str_or_null(o, "active_timer_id", ts.active ? ts.active_id : NULL);
    cJSON_AddStringToObject(o, "timer_note", ts.note);

    if (s->have_ext && s->ext_active) {
        const at_ext_t *e = &s->ext;
        cJSON_AddStringToObject(o, "ext_mode_name", e->mode_name);
        cJSON_AddNumberToObject(o, "ext_mode_code", e->mode_code);
        cJSON_AddNumberToObject(o, "ext_flame_temp_c", e->flame_temp_c);
        cJSON_AddNumberToObject(o, "ext_liquid_temp_c", e->liquid_temp_c);
        cJSON_AddNumberToObject(o, "ext_overheat_temp_c", e->overheat_temp_c);
        cJSON_AddNumberToObject(o, "ext_board_temp_c", e->board_temp_c);
        cJSON_AddNumberToObject(o, "ext_running_time_s", e->running_time_s);
        cJSON_AddNumberToObject(o, "ext_defined_rev", e->defined_rev);
        cJSON_AddNumberToObject(o, "ext_measured_rev", e->measured_rev);
        cJSON_AddBoolToObject(o, "ext_glow_plug", e->glow_plug);
        cJSON_AddNumberToObject(o, "ext_fuel_pump_hz", e->fuel_pump_hz);
        cJSON_AddNumberToObject(o, "ext_voltage", e->voltage);
        cJSON_AddNumberToObject(o, "ext_fault_code", e->fault_code);
        const char *fn = at_fault_name(e->fault_code);
        cJSON_AddStringToObject(o, "ext_fault_name", fn ? fn : "unknown");
        cJSON_AddNumberToObject(o, "ext_output_pct", e->output_pct);
        cJSON_AddNumberToObject(o, "ext_engine_state", e->engine_state);
        cJSON_AddNumberToObject(o, "ext_relay_state", e->relay_state);
        cJSON_AddNumberToObject(o, "ext_fan_current_ma", e->fan_current_ma);
    }

    /* Battery voltage, always: the extended frame's reading when it is on, else the status frame's byte [6] */
    if (s->have_ext && s->ext_active) {
        add_num_or_null(o, "supply_voltage", true, s->ext.voltage);
    } else {
        const bool ok = s->have_status && !s->stale && s->status.supply_v >= 6.0f && s->status.supply_v <= 36.0f;
        add_num_or_null(o, "supply_voltage", ok, s->status.supply_v);
    }

    /* What the status frame says the heater is doing, and the fuel estimate built on it */
    add_str_or_null(o, "mode_name", s->have_status ? at_mode_name(s->status.state_raw, s->status.substate) : NULL);
    at_fuel_view_t fv;
    at_app_fuel_view(&fv);
    cJSON_AddStringToObject(o, "power_level", at_fuel_level_name(fv.level));
    cJSON_AddNumberToObject(o, "fuel_rate_lph", round3(fv.rate_lph));
    cJSON_AddStringToObject(o, "fuel_source", fv.from_pump ? "pump" : "estimate");
    cJSON_AddNumberToObject(o, "fuel_ml_per_pulse", round(fv.pump.ml_per_pulse * 1e5) / 1e5);
    cJSON_AddNumberToObject(o, "fuel_pump_pulses", (double)(long long)fv.pump.pulses);
    cJSON_AddNumberToObject(o, "fuel_litres_total", fv.totals.litres);
    cJSON_AddNumberToObject(o, "fuel_cost_total", fv.totals.cost);
    cJSON_AddNumberToObject(o, "fuel_litres_today", fv.have_today ? fv.today.ml / 1000.0 : 0);
    cJSON_AddNumberToObject(o, "fuel_cost_today", fv.have_today ? fv.today.cost_m / 1000.0 : 0);
    cJSON_AddNumberToObject(o, "fuel_price", round3(fv.cfg.price));
    cJSON_AddNumberToObject(o, "fuel_factor", round3(fv.cfg.factor));
    cJSON *rates = cJSON_AddObjectToObject(o, "fuel_rates");
    for (int l = AT_FUEL_STARTING; l < AT_FUEL_LEVELS; l++) {
        cJSON_AddNumberToObject(rates, at_fuel_level_name((at_fuel_level_t)l), round3(fv.cfg.rate_lph[l]));
    }
    uint32_t burn_s = 0;
    for (int l = AT_FUEL_STARTING; l < AT_FUEL_LEVELS; l++) burn_s += fv.totals.level_s[l];
    cJSON_AddNumberToObject(o, "burn_hours_total", burn_s / 3600.0);
    cJSON_AddNumberToObject(o, "hours_low", fv.totals.level_s[AT_FUEL_LOW] / 3600.0);
    cJSON_AddNumberToObject(o, "hours_middle", fv.totals.level_s[AT_FUEL_MIDDLE] / 3600.0);
    cJSON_AddNumberToObject(o, "hours_high", fv.totals.level_s[AT_FUEL_HIGH] / 3600.0);
    cJSON_AddNumberToObject(o, "starts_total", fv.totals.starts);
    add_num_or_null(o, "tank_capacity", fv.tank.capacity_l > 0, round3(fv.tank.capacity_l));
    add_num_or_null(o, "tank_litres", fv.have_tank_left, round3(fv.tank_left_l));
    add_num_or_null(o, "tank_pct", fv.have_tank_left && fv.tank.capacity_l > 0,
                    round(fv.tank_left_l / (fv.tank.capacity_l > 0 ? fv.tank.capacity_l : 1) * 1000.0) / 10.0);
    /* 1.62: the battery-use estimate (at_battery.h) */
    at_battery_view_t bv;
    at_app_battery_view(&bv);
    cJSON_AddNumberToObject(o, "battery_w", round(bv.watts * 10.0) / 10.0);
    cJSON_AddNumberToObject(o, "battery_ah_run", round3(bv.ah_run));
    cJSON_AddNumberToObject(o, "battery_ah_today", round3(bv.ah_today));
    cJSON_AddNumberToObject(o, "battery_ah_total", round3(bv.ah_total));
    cJSON_AddNumberToObject(o, "battery_factor", round3(bv.factor));
    add_num_or_null(o, "tank_set_epoch", fv.tank.have_level && fv.tank.set_epoch > 0, (double)fv.tank.set_epoch);

    if (!(s->have_ext && s->ext_active)) {
        /* keep the keys present (null) so Home Assistant templates never see an undefined field */
        static const char *const ext_keys[] = {
            "ext_mode_name", "ext_mode_code", "ext_flame_temp_c", "ext_liquid_temp_c", "ext_overheat_temp_c",
            "ext_board_temp_c", "ext_running_time_s", "ext_defined_rev", "ext_measured_rev", "ext_glow_plug",
            "ext_fuel_pump_hz", "ext_voltage", "ext_fault_code", "ext_fault_name", "ext_output_pct",
            "ext_engine_state", "ext_relay_state", "ext_fan_current_ma"};
        for (size_t i = 0; i < sizeof(ext_keys) / sizeof(ext_keys[0]); i++) {
            cJSON_AddNullToObject(o, ext_keys[i]);
        }
    }

    return o;
}

/* The status document is ~110 cJSON nodes, 10 KB or more of heap while it is built. The web API, the app, Home
 * Assistant and MQTT all ask for it, often in the same second; each building its own tree at once is what ran the
 * heap out and turned /api/status into a 500. So it is built by one caller at a time, and callers within
 * STATE_FRESH_US share that build.
 *
 * It is printed into one buffer reserved at boot (1.0045). Printing into the heap needed a 3 KB block, then a
 * bigger one alongside it once heater_info pushed the document past 3 KB, then a copy per caller: on a fragmented
 * heap (largest block 2 to 8 KB with the cloud link retrying) those failed and /api/status answered 500. The tree's
 * small nodes still come from the heap, but they fit in the gaps. */
#define STATE_FRESH_US 1000000  /* the apps poll every 2 s: one build serves them and the cloud link (1.0046) */
#define STATE_STALE_OK_US 10000000 /* if a build fails for lack of memory, a copy this old is better than a 500 */
#define STATE_BUF_MAX 6144         /* about 2.7 KB normally, 4 KB with heater_info and ten faults */

static SemaphoreHandle_t s_state_lock;
static char s_state_buf[STATE_BUF_MAX];
static bool s_state_ok;
static int64_t s_state_us;

void at_app_state_json_init(void)
{
    if (!s_state_lock) s_state_lock = xSemaphoreCreateMutex();
}

/* With the lock held: make s_state_buf current, or leave the last good copy while it is not too old. */
static void refresh_state(void)
{
    int64_t now = esp_timer_get_time();
    if (s_state_ok && now - s_state_us < STATE_FRESH_US) return;
    cJSON *o = build_state_tree();
    if (o) {
        /* cJSON asks for 5 bytes of slack past the longest document it may write */
        s_state_ok = cJSON_PrintPreallocated(o, s_state_buf, STATE_BUF_MAX - 5, false);
        cJSON_Delete(o);
        if (s_state_ok) {
            s_state_us = now;
            return;
        }
        ESP_LOGW("at_json", "status document longer than %d bytes", STATE_BUF_MAX - 5);
    } else if (s_state_ok && now - s_state_us > STATE_STALE_OK_US) {
        s_state_ok = false;
    }
}

esp_err_t at_app_state_json_with(esp_err_t (*fn)(const char *js, void *ctx), void *ctx)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    refresh_state();
    esp_err_t e = s_state_ok ? fn(s_state_buf, ctx) : ESP_ERR_NO_MEM;
    xSemaphoreGive(s_state_lock);
    return e;
}

char *at_app_state_json(void)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    refresh_state();
    char *out = s_state_ok ? strdup(s_state_buf) : NULL;
    xSemaphoreGive(s_state_lock);
    return out;
}
