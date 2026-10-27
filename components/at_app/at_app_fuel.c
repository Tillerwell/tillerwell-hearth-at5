/* Fuel estimate on the box: at_fuel.c's sums, fed from the status snapshot, kept in flash. */
#include <string.h>

#include "at_app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "fuel";

#define KEY_CFG  "fuel_cfg"
#define KEY_TOT  "fuel_tot"
#define KEY_LOG  "fuel_log"
#define SAVE_EVERY_MS (15u * 60u * 1000u) /* while burning; also at the end of every burn */
#define MAX_TICK_S    10.0                /* a longer gap (no status, a stall) is not counted */

static SemaphoreHandle_t s_lock;
static at_fuel_cfg_t s_cfg;
static at_fuel_totals_t s_tot;
static at_fuel_log_t s_log;
static at_fuel_run_t s_run;
static at_fuel_level_t s_level;
static bool s_have_tick, s_dirty;
static uint32_t s_last_tick_ms, s_last_save_ms;

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
    ESP_LOGI(TAG, "%.2f l estimated so far, %u days logged", s_tot.litres, s_log.count);
}

static void save_locked(void)
{
    at_settings_blob_save(KEY_TOT, &s_tot, sizeof(s_tot));
    at_settings_blob_save(KEY_LOG, &s_log, sizeof(s_log));
    s_dirty = false;
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
    s_level = at_fuel_tick(&s_tot, &s_log, &s_run, &s_cfg, snap->status.state_raw, snap->status.substate,
                           snap->status.burner_active, dt, day);
    if (s_level != AT_FUEL_OFF) s_dirty = true;
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
    out->rate_lph = at_fuel_rate_now(&s_cfg, s_level);
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
