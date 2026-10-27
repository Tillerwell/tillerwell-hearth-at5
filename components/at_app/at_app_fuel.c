/* Fuel estimate on the box: at_fuel.c's sums, fed from the status snapshot, kept in flash. */
#include <string.h>
#include <time.h>

#include "at_app.h"
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
#define SAVE_EVERY_MS (5u * 60u * 1000u) /* while burning; also at the end of every burn and before any restart */
#define MAX_TICK_S    10.0                /* a longer gap (no status, a stall) is not counted */

static SemaphoreHandle_t s_lock;
static at_fuel_cfg_t s_cfg;
static at_fuel_totals_t s_tot;
static at_fuel_log_t s_log;
static at_fuel_run_t s_run;
static at_fuel_tank_t s_tank;
static at_fuel_pump_t s_pump;
static float s_pump_hz;
static bool s_from_pump;
static at_fuel_level_t s_level;
static bool s_have_tick, s_dirty;
static uint32_t s_last_tick_ms, s_last_save_ms;

static void save_on_restart(void);

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
    esp_register_shutdown_handler(save_on_restart);
    ESP_LOGI(TAG, "%.2f l estimated so far, %u days logged", s_tot.litres, s_log.count);
}

static void save_locked(void)
{
    at_settings_blob_save(KEY_TOT, &s_tot, sizeof(s_tot));
    at_settings_blob_save(KEY_LOG, &s_log, sizeof(s_log));
    at_settings_blob_save(KEY_PUMP, &s_pump, sizeof(s_pump));
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
    if (!at_app_local_day(&day)) day = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    at_fuel_level_t was = s_level;
    /* the pump's frequency counts only while the extended frames are arriving; otherwise it is the estimate */
    float hz = snap->have_ext && snap->ext_active ? snap->ext.fuel_pump_hz : -1.0f;
    s_pump_hz = hz > 0 ? hz : 0;
    s_from_pump = hz >= 0 && s_pump.ml_per_pulse > 0;
    s_level = at_fuel_tick_pump(&s_tot, &s_log, &s_run, &s_cfg, &s_pump, hz, snap->status.state_raw,
                                snap->status.substate, snap->status.burner_active, dt, day);
    if (s_level != AT_FUEL_OFF || hz > 0) s_dirty = true;
    bool burn_ended = was != AT_FUEL_OFF && s_level == AT_FUEL_OFF;
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
    return set_cfg(&c);
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
