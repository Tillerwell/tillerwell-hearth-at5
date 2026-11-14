/* Fuel estimate on the box: at_fuel.c's sums, fed from the status snapshot, kept in flash. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "at_app.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "fuel";

#define KEY_CFG  "fuel_cfg"
#define KEY_TOT  "fuel_tot"
#define KEY_LOG  "fuel_log"
#define KEY_TANK "fuel_tank"
#define KEY_PUMP "fuel_pump"
#define KEY_DET  "fuel_det" /* by-level breakdown of the last 31 days (1.64) */
#define SAVE_EVERY_MS (5u * 60u * 1000u) /* while burning; also at the end of every burn and before any restart */
#define MAX_TICK_S    10.0                /* a longer gap (no status, a stall) is not counted */

static SemaphoreHandle_t s_lock;
static at_fuel_cfg_t s_cfg;
static at_fuel_totals_t s_tot;
static at_fuel_log_t s_log;
static at_fuel_run_t s_run;
static at_fuel_tank_t s_tank;
static at_fuel_pump_t s_pump;
static at_fuel_detail_t s_det;
static at_fuel_detail_run_t s_det_run;
static float s_pump_hz;
static bool s_from_pump;
static at_fuel_level_t s_level;
static bool s_have_tick, s_dirty;
static uint32_t s_last_tick_ms, s_last_save_ms;

static void save_on_restart(void);

/* The backup's "fuel" section (1.65), cached so the backup's hash only moves on the hour, at the end of a burn or
 * when the price changes: the backup is re-sent to the cloud whenever its hash changes. */
static char *s_cloud;
static void save_locked(void);
static int64_t s_cloud_key = -1; /* local day * 24 + hour it was built for */
static bool s_cloud_stale = true;

void at_app_fuel_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!at_settings_blob_load(KEY_CFG, &s_cfg, sizeof(s_cfg)) || !at_fuel_cfg_valid(&s_cfg)) {
        at_fuel_cfg_defaults(&s_cfg);
    }
    if (!at_settings_blob_load(KEY_TOT, &s_tot, sizeof(s_tot)) || s_tot.version != AT_FUEL_TOTALS_VERSION) {
        at_fuel_totals_init(&s_tot);
    }
    if (!at_settings_blob_load(KEY_LOG, &s_log, sizeof(s_log)) || s_log.count > AT_FUEL_DAYS || s_log.head >= AT_FUEL_DAYS) {
        at_fuel_log_init(&s_log);
    }
    if (!at_settings_blob_load(KEY_TANK, &s_tank, sizeof(s_tank)) || s_tank.version != AT_FUEL_TANK_VERSION) {
        at_fuel_tank_init(&s_tank);
    }
    if (!at_settings_blob_load(KEY_PUMP, &s_pump, sizeof(s_pump)) || !at_fuel_pump_valid(&s_pump)) {
        at_fuel_pump_init(&s_pump);
    }
    if (!at_settings_blob_load(KEY_DET, &s_det, sizeof(s_det)) || !at_fuel_detail_valid(&s_det)) {
        at_fuel_detail_init(&s_det);
    }
    esp_register_shutdown_handler(save_on_restart);
    ESP_LOGI(TAG, "%.2f l estimated so far, %u days logged", s_tot.litres, s_log.count);
}

static void save_locked(void)
{
    at_settings_blob_save(KEY_TOT, &s_tot, sizeof(s_tot));
    at_settings_blob_save(KEY_LOG, &s_log, sizeof(s_log));
    at_settings_blob_save(KEY_PUMP, &s_pump, sizeof(s_pump));
    at_settings_blob_save(KEY_DET, &s_det, sizeof(s_det));
    s_dirty = false;
}

/* Runs inside esp_restart() (updates, Reboot, factory reset): keep what was counted since the last save. */
static void save_on_restart(void)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (s_dirty) save_locked();
        xSemaphoreGive(s_lock);
    }
}

void at_app_fuel_tick(const at_snapshot_t *snap, uint32_t now_ms)
{
    if (!s_lock) return;
    double dt = s_have_tick ? (now_ms - s_last_tick_ms) / 1000.0 : 0;
    s_have_tick = true;
    s_last_tick_ms = now_ms;
    if (!snap->have_status || snap->stale) return;
    if (dt > MAX_TICK_S) dt = 0;

    int32_t day;
    int minute = 0;
    if (!at_app_local_time(&day, &minute)) day = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    at_fuel_level_t was = s_level;
    double litres_before = s_tot.litres;
    /* the pump's frequency counts only while the extended frames are arriving; otherwise it is the estimate */
    float hz = snap->have_ext && snap->ext_active ? snap->ext.fuel_pump_hz : -1.0f;
    s_pump_hz = hz > 0 ? hz : 0;
    s_from_pump = hz >= 0 && s_pump.ml_per_pulse > 0;
    s_level = at_fuel_tick_pump(&s_tot, &s_log, &s_run, &s_cfg, &s_pump, hz, snap->status.state_raw,
                                snap->status.substate, snap->status.burner_active, dt, day);
    at_fuel_detail_add(&s_det, &s_det_run, day, minute / 60, s_level, dt, s_tot.litres - litres_before);
    if (s_level != AT_FUEL_OFF || hz > 0) s_dirty = true;
    bool burn_ended = was != AT_FUEL_OFF && s_level == AT_FUEL_OFF;
    if (burn_ended) s_cloud_stale = true;
    if (s_dirty && (burn_ended || now_ms - s_last_save_ms >= SAVE_EVERY_MS)) {
        save_locked();
        s_last_save_ms = now_ms;
    }
    xSemaphoreGive(s_lock);
}

void at_app_fuel_view(at_fuel_view_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_lock) {
        at_fuel_cfg_defaults(&out->cfg);
        at_fuel_totals_init(&out->totals);
        return;
    }
    int32_t day;
    bool clock = at_app_local_day(&day);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->cfg = s_cfg;
    out->totals = s_tot;
    out->level = s_level;
    out->pump = s_pump;
    out->from_pump = s_from_pump;
    out->pump_hz = s_pump_hz;
    out->rate_lph = s_from_pump ? at_fuel_pump_rate_lph(&s_pump, s_pump_hz) : at_fuel_rate_now(&s_cfg, s_level);
    out->tank = s_tank;
    out->have_tank_left = at_fuel_tank_remaining(&s_tank, s_tot.litres, &out->tank_left_l);
    const at_fuel_day_t *t = clock ? at_fuel_log_find(&s_log, day) : NULL;
    if (t) {
        out->have_today = true;
        out->today = *t;
    }
    xSemaphoreGive(s_lock);
}

int at_app_fuel_log_copy(at_fuel_day_t *out, int max)
{
    if (!s_lock) return 0;
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (const at_fuel_day_t *e; n < max && (e = at_fuel_log_at(&s_log, n)); n++) out[n] = *e;
    xSemaphoreGive(s_lock);
    return n;
}

bool at_app_fuel_detail(int32_t day, at_fuel_detail_day_t *detail, at_fuel_day_t *log_day)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const at_fuel_detail_day_t *e = at_fuel_detail_find(&s_det, day);
    const at_fuel_day_t *l = at_fuel_log_find(&s_log, day);
    if (e) *detail = *e;
    if (l) *log_day = *l;
    else memset(log_day, 0, sizeof(*log_day));
    xSemaphoreGive(s_lock);
    return e != NULL;
}

int at_app_fuel_detail_days(int32_t *out, int max)
{
    if (!s_lock) return 0;
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (; n < s_det.count && n < max; n++) out[n] = s_det.d[(s_det.head + AT_FUEL_DETAIL_DAYS - n) % AT_FUEL_DETAIL_DAYS].day;
    xSemaphoreGive(s_lock);
    return n;
}

/* {"v":1,"log":"...","detail":"...","totals":{...},"tank_left":l,"tank_at":epoch,"battery":{...},"pump_pulses":n}
 * (at_fuel.h has the two strings' format). */
static char *build_cloud_locked(void)
{
    size_t cap = AT_FUEL_LOG_TEXT_MAX + AT_FUEL_DETAIL_TEXT_MAX + 640;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t n = (size_t)snprintf(buf, cap, "{\"v\":1,\"log\":\"");
    size_t w = at_fuel_log_encode(&s_log, buf + n, cap - n);
    if (!w && s_log.count) { free(buf); return NULL; }
    n += w;
    n += (size_t)snprintf(buf + n, cap - n, "\",\"detail\":\"");
    w = at_fuel_detail_encode(&s_det, buf + n, cap - n);
    if (!w && s_det.count) { free(buf); return NULL; }
    n += w;
    n += (size_t)snprintf(buf + n, cap - n, "\",\"totals\":{\"litres\":%.4f,\"cost\":%.4f,\"level_s\":[%lu,%lu,%lu,%lu],\"starts\":%lu}",
                          s_tot.litres, s_tot.cost, (unsigned long)s_tot.level_s[AT_FUEL_STARTING], (unsigned long)s_tot.level_s[AT_FUEL_LOW],
                          (unsigned long)s_tot.level_s[AT_FUEL_MIDDLE], (unsigned long)s_tot.level_s[AT_FUEL_HIGH], (unsigned long)s_tot.starts);
    float left;
    if (at_fuel_tank_remaining(&s_tank, s_tot.litres, &left) && n < cap)
        n += (size_t)snprintf(buf + n, cap - n, ",\"tank_left\":%.2f,\"tank_at\":%lld", left, (long long)s_tank.set_epoch);
    double ah, wh, ah_day;
    int32_t bday;
    at_app_battery_totals(&ah, &wh, &bday, &ah_day);
    if (n < cap)
        n += (size_t)snprintf(buf + n, cap - n, ",\"battery\":{\"ah\":%.3f,\"wh\":%.2f,\"day\":%ld,\"ah_day\":%.3f}", ah, wh, (long)bday, ah_day);
    if (n < cap) n += (size_t)snprintf(buf + n, cap - n, ",\"pump_pulses\":%.0f}", s_pump.pulses);
    if (n >= cap) { free(buf); return NULL; }
    return buf;
}

char *at_app_fuel_cloud_json(void)
{
    if (!s_lock) return NULL;
    int32_t day;
    int minute;
    if (!at_app_local_time(&day, &minute)) return NULL; /* no dates without a clock */
    int64_t key = (int64_t)day * 24 + minute / 60;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cloud_stale || key != s_cloud_key || !s_cloud) {
        char *next = build_cloud_locked();
        if (next) {
            free(s_cloud);
            s_cloud = next;
            s_cloud_key = key;
            s_cloud_stale = false;
        }
    }
    char *out = s_cloud ? strdup(s_cloud) : NULL;
    xSemaphoreGive(s_lock);
    return out;
}

static double jnum_or(const cJSON *o, const char *k, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

bool at_app_fuel_restore(const cJSON *fuel)
{
    if (!s_lock || !cJSON_IsObject(fuel) || jnum_or(fuel, "v", 0) != 1) return false;
    const cJSON *log = cJSON_GetObjectItemCaseSensitive(fuel, "log");
    const cJSON *det = cJSON_GetObjectItemCaseSensitive(fuel, "detail");
    const cJSON *tot = cJSON_GetObjectItemCaseSensitive(fuel, "totals");
    const cJSON *bat = cJSON_GetObjectItemCaseSensitive(fuel, "battery");
    bool ok = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (cJSON_IsString(log)) ok = at_fuel_log_merge_text(&s_log, log->valuestring) >= 0 && ok;
    if (cJSON_IsString(det)) ok = at_fuel_detail_merge_text(&s_det, det->valuestring) >= 0 && ok;
    float own_left;
    bool had_level = at_fuel_tank_remaining(&s_tank, s_tot.litres, &own_left);
    if (cJSON_IsObject(tot)) {
        at_fuel_totals_t r;
        at_fuel_totals_init(&r);
        r.litres = jnum_or(tot, "litres", 0);
        r.cost = jnum_or(tot, "cost", 0);
        r.starts = (uint32_t)jnum_or(tot, "starts", 0);
        const cJSON *ls = cJSON_GetObjectItemCaseSensitive(tot, "level_s");
        for (int i = 0; i < AT_FUEL_BURN_LEVELS && cJSON_IsArray(ls); i++) {
            const cJSON *x = cJSON_GetArrayItem(ls, i);
            if (cJSON_IsNumber(x) && x->valuedouble >= 0) r.level_s[AT_FUEL_STARTING + i] = (uint32_t)x->valuedouble;
        }
        if (r.litres >= 0 && r.cost >= 0) {
            at_fuel_totals_merge(&s_tot, &r);
            /* the tank's level counts down from the total: keep what it showed across the change */
            if (had_level) at_fuel_tank_set_level(&s_tank, own_left, s_tot.litres, s_tank.set_epoch);
        }
    }
    /* what was left in the old box's tank, unless this box has been told a level of its own */
    double left = jnum_or(fuel, "tank_left", -1);
    if (!had_level && left >= 0) at_fuel_tank_set_level(&s_tank, (float)left, s_tot.litres, (int64_t)jnum_or(fuel, "tank_at", 0));
    double pulses = jnum_or(fuel, "pump_pulses", 0);
    if (pulses > s_pump.pulses) s_pump.pulses += pulses;
    save_locked();
    at_settings_blob_save(KEY_TANK, &s_tank, sizeof(s_tank));
    s_cloud_stale = true;
    xSemaphoreGive(s_lock);
    if (cJSON_IsObject(bat)) {
        at_app_battery_restore(jnum_or(bat, "ah", 0), jnum_or(bat, "wh", 0), (int32_t)jnum_or(bat, "day", -1), jnum_or(bat, "ah_day", 0));
    }
    ESP_LOGI(TAG, "fuel restored: %u days logged, %.2f l in all", s_log.count, s_tot.litres);
    return ok;
}

static bool set_cfg(const at_fuel_cfg_t *next)
{
    if (!s_lock || !at_fuel_cfg_valid(next)) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *next;
    at_settings_blob_save(KEY_CFG, &s_cfg, sizeof(s_cfg));
    xSemaphoreGive(s_lock);
    return true;
}

static at_fuel_cfg_t cfg_copy(void)
{
    at_fuel_cfg_t c;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c = s_cfg;
    xSemaphoreGive(s_lock);
    return c;
}

bool at_app_fuel_set_price(float per_litre)
{
    if (!s_lock) return false;
    at_fuel_cfg_t c = cfg_copy();
    c.price = per_litre;
    bool ok = set_cfg(&c);
    if (ok) s_cloud_stale = true;
    return ok;
}

bool at_app_fuel_set_price_from(float per_litre, int32_t from_day)
{
    if (!s_lock) return false;
    int32_t today;
    if (!at_app_local_day(&today) || from_day > today || from_day < today - (AT_FUEL_DAYS - 1)) return false;
    at_fuel_cfg_t c = cfg_copy();
    c.price = per_litre;
    if (!set_cfg(&c)) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = at_fuel_log_reprice(&s_log, &s_tot, &s_run, from_day, per_litre);
    save_locked();
    s_cloud_stale = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "price %.3f from day %ld: %d days costed again", per_litre, (long)from_day, n);
    return true;
}

bool at_app_fuel_set_factor(float factor)
{
    if (!s_lock) return false;
    at_fuel_cfg_t c = cfg_copy();
    c.factor = factor;
    return set_cfg(&c);
}

bool at_app_fuel_set_rates(float starting, float low, float middle, float high)
{
    if (!s_lock) return false;
    at_fuel_cfg_t c = cfg_copy();
    c.rate_lph[AT_FUEL_STARTING] = starting;
    c.rate_lph[AT_FUEL_LOW] = low;
    c.rate_lph[AT_FUEL_MIDDLE] = middle;
    c.rate_lph[AT_FUEL_HIGH] = high;
    return set_cfg(&c);
}

static bool save_tank_locked(bool ok)
{
    if (ok) at_settings_blob_save(KEY_TANK, &s_tank, sizeof(s_tank));
    return ok;
}

bool at_app_fuel_pump_reset_count(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    at_fuel_pump_reset_count(&s_pump);
    at_settings_blob_save(KEY_PUMP, &s_pump, sizeof(s_pump));
    xSemaphoreGive(s_lock);
    return true;
}

static bool save_pump_locked(bool ok)
{
    if (ok) at_settings_blob_save(KEY_PUMP, &s_pump, sizeof(s_pump));
    return ok;
}

bool at_app_fuel_pump_calibrate(float litres)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = save_pump_locked(at_fuel_pump_calibrate(&s_pump, litres));
    xSemaphoreGive(s_lock);
    return ok;
}

bool at_app_fuel_set_ml_per_pulse(float ml)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = save_pump_locked(at_fuel_pump_set_ml(&s_pump, ml));
    xSemaphoreGive(s_lock);
    return ok;
}

bool at_app_fuel_set_tank_capacity(float litres)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = save_tank_locked(at_fuel_tank_set_capacity(&s_tank, litres));
    xSemaphoreGive(s_lock);
    return ok;
}

bool at_app_fuel_set_tank_level(float litres)
{
    if (!s_lock) return false;
    int32_t day;
    int64_t epoch = at_app_local_day(&day) ? (int64_t)time(NULL) : 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = save_tank_locked(at_fuel_tank_set_level(&s_tank, litres, s_tot.litres, epoch));
    xSemaphoreGive(s_lock);
    return ok;
}

bool at_app_fuel_tank_filled(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    float cap = s_tank.capacity_l;
    xSemaphoreGive(s_lock);
    return cap > 0 && at_app_fuel_set_tank_level(cap);
}
