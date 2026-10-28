#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "at_app.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"
#include "cJSON.h"
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
    return true;
#undef FAIL
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
    at_timer_problem_t p;
    esp_err_t e = at_app_timers_set(list, n, &p);
    free(list);
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

/* GET /api/fuel: the daily log, newest first, with local dates. */
char *at_app_fuel_json(void)
{
    at_fuel_day_t *days = malloc(sizeof(at_fuel_day_t) * AT_FUEL_DAYS);
    if (!days) return NULL;
    int n = at_app_fuel_log_copy(days, AT_FUEL_DAYS);
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
        cJSON_AddItemToArray(arr, e);
    }
    free(days);
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return str;
}

static char *build_state_json(void)
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

    add_num_or_null(o, "effective_temp", v.have_effective_temp, v.effective_temp);
    cJSON_AddBoolToObject(o, "external_temp_active", v.have_effective_temp && v.effective_is_external);
    add_num_or_null(o, "external_temp", v.have_external, v.external_temp);
    cJSON_AddBoolToObject(o, "use_external_temp", v.use_external_temp);

    cJSON_AddBoolToObject(o, "auto_enabled", v.auto_enabled);
    cJSON_AddNumberToObject(o, "auto_target", v.auto_target);
    cJSON_AddNumberToObject(o, "auto_low", v.auto_low);
    cJSON_AddNumberToObject(o, "auto_high", v.auto_high);
    cJSON_AddNumberToObject(o, "hyst_min", v.hyst_min);
    cJSON_AddNumberToObject(o, "hyst_max", v.hyst_max);
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

    /* one buffer the size of a typical document, rather than doubling from 256 bytes as it prints */
    char *str = cJSON_PrintBuffered(o, 3072, false);
    cJSON_Delete(o);
    return str;
}

/* The status document is ~110 cJSON nodes, 10 KB or more of heap while it is built. The web API, the app, Home
 * Assistant and MQTT all ask for it, often in the same second; each building its own tree at once is what ran the
 * heap out and turned /api/status into a 500. So it is built by one caller at a time, and callers within
 * STATE_FRESH_US share that build. */
#define STATE_FRESH_US 500000
#define STATE_STALE_OK_US 10000000 /* if a build fails for lack of memory, a copy this old is better than a 500 */

static SemaphoreHandle_t s_state_lock;
static char *s_state_cache;
static int64_t s_state_us;

void at_app_state_json_init(void)
{
    if (!s_state_lock) s_state_lock = xSemaphoreCreateMutex();
}

char *at_app_state_json(void)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    if (!s_state_cache || now - s_state_us >= STATE_FRESH_US) {
        char *fresh = build_state_json();
        if (fresh) {
            free(s_state_cache);
            s_state_cache = fresh;
            s_state_us = now;
        } else if (s_state_cache && now - s_state_us > STATE_STALE_OK_US) {
            free(s_state_cache);
            s_state_cache = NULL;
        }
    }
    char *out = s_state_cache ? strdup(s_state_cache) : NULL;
    xSemaphoreGive(s_state_lock);
    return out;
}
