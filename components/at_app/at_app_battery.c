/* Battery-use estimate on the box (1.62): at_battery.c's sums, fed from the status snapshot, kept in flash. */
#include <string.h>

#include "at_app.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "battery";

#define KEY_TOT "batt_tot"
#define KEY_CFG "batt_cfg"
#define SAVE_EVERY_MS (5u * 60u * 1000u) /* while drawing; also when the heater goes idle and before any restart */
#define MAX_TICK_S    10.0                /* a longer gap (no status, a stall) is not counted */

typedef struct {
    uint32_t version;
    float factor;
} batt_cfg_t;
#define BATT_CFG_VERSION 1

static SemaphoreHandle_t s_lock;
static at_batt_totals_t s_tot;
static at_batt_run_t s_run;
static batt_cfg_t s_cfg;
static at_batt_phase_t s_phase;
static float s_volts = AT_BATT_NOMINAL_V;
static bool s_have_tick, s_dirty;
static uint32_t s_last_tick_ms, s_last_save_ms;

static void save_on_restart(void);

void at_app_battery_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!at_settings_blob_load(KEY_TOT, &s_tot, sizeof(s_tot)) || !at_batt_totals_valid(&s_tot)) {
        at_batt_totals_init(&s_tot);
    }
    if (!at_settings_blob_load(KEY_CFG, &s_cfg, sizeof(s_cfg)) || s_cfg.version != BATT_CFG_VERSION ||
        !at_batt_factor_valid(s_cfg.factor)) {
        s_cfg.version = BATT_CFG_VERSION;
        s_cfg.factor = 1.0f;
    }
    esp_register_shutdown_handler(save_on_restart);
    ESP_LOGI(TAG, "%.2f Ah estimated so far", s_tot.ah);
}

static void save_locked(void)
{
    at_settings_blob_save(KEY_TOT, &s_tot, sizeof(s_tot));
    s_dirty = false;
}

static void save_on_restart(void)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (s_dirty) save_locked();
        xSemaphoreGive(s_lock);
    }
}

void at_app_battery_tick(const at_snapshot_t *snap, uint32_t now_ms)
{
    if (!s_lock) return;
    double dt = s_have_tick ? (now_ms - s_last_tick_ms) / 1000.0 : 0;
    s_have_tick = true;
    s_last_tick_ms = now_ms;
    if (!snap->have_status || snap->stale) return;
    if (dt > MAX_TICK_S) dt = 0;

    int32_t day;
    if (!at_app_local_day(&day)) day = -1;
    float v = snap->have_ext && snap->ext_active ? snap->ext.voltage : snap->status.supply_v;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    at_batt_phase_t was = s_phase;
    s_volts = at_batt_volts(v);
    s_phase = at_batt_tick(&s_tot, &s_run, s_cfg.factor, snap->status.state_raw, snap->status.substate,
                           snap->status.burner_active, v, dt, day);
    if (s_phase != AT_BATT_OFF) s_dirty = true;
    bool ended = was != AT_BATT_OFF && s_phase == AT_BATT_OFF;
    if (s_dirty && (ended || now_ms - s_last_save_ms >= SAVE_EVERY_MS)) {
        save_locked();
        s_last_save_ms = now_ms;
    }
    xSemaphoreGive(s_lock);
}

void at_app_battery_view(at_battery_view_t *out)
{
    memset(out, 0, sizeof(*out));
    out->factor = 1.0f;
    if (!s_lock) return;
    int32_t day;
    bool clock = at_app_local_day(&day);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->phase = s_phase;
    out->factor = s_cfg.factor;
    out->watts = at_batt_watts(s_phase) * s_cfg.factor;
    out->volts = s_volts;
    out->ah_total = s_tot.ah;
    out->wh_total = s_tot.wh;
    out->ah_run = s_tot.ah_run;
    out->ah_today = clock ? at_batt_ah_today(&s_tot, day) : 0;
    xSemaphoreGive(s_lock);
}

bool at_app_battery_set_factor(float factor)
{
    if (!s_lock || !at_batt_factor_valid(factor)) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.factor = factor;
    at_settings_blob_save(KEY_CFG, &s_cfg, sizeof(s_cfg));
    xSemaphoreGive(s_lock);
    return true;
}
