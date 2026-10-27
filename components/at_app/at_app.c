#include <string.h>

#include "at_app.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "app";

#define HEATER_UART UART_NUM_1
#define PANEL_UART  UART_NUM_0

static at_core_t s_core;
static SemaphoreHandle_t s_core_lock;   /* guards s_core */
static at_ctl_t s_ctl;
static SemaphoreHandle_t s_ctl_lock;    /* guards s_ctl; always taken before s_core_lock */
static bool s_started;
static uint32_t s_last_debug_ms;

uint32_t at_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- UART ports -------------------------------------------------------------- */

static size_t uart_port_read(void *ctx, uint8_t *buf, size_t max)
{
    uart_port_t u = (uart_port_t)(intptr_t)ctx;
    size_t avail = 0;
    uart_get_buffered_data_len(u, &avail);
    if (!avail) {
        return 0;
    }
    if (avail > max) {
        avail = max;
    }
    int n = uart_read_bytes(u, buf, avail, 0);
    return n > 0 ? (size_t)n : 0;
}

static void uart_port_write(void *ctx, const uint8_t *buf, size_t n)
{
    uart_write_bytes((uart_port_t)(intptr_t)ctx, buf, n);
}

static esp_err_t uart_setup(uart_port_t u, int tx, int rx)
{
    uart_config_t cfg = {
        .baud_rate = CONFIG_AUTOTERM_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(u, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(u, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(u, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* Idle high even when the far end only pulls low (open-drain TX that relies on the other side's pull-up) */
    gpio_pullup_en(rx);
    return ESP_OK;
}

/* ---- line probe: how each RX pin idles, for wiring checks ------------------------ */

static volatile int s_rx_high_pct[2], s_rx_edges[2]; /* [0] heater RX, [1] controller RX; last 1 s window */
static volatile int s_heater_rx_mv = -1; /* idle voltage at the heater RX pin, -1 = not read (pin has no ADC) */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static adc_channel_t s_adc_chan;

static void adc_probe_init(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(CONFIG_AUTOTERM_HEATER_RX_GPIO, &unit, &s_adc_chan) != ESP_OK) return;
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit};
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) return;
    adc_cali_curve_fitting_config_t ccfg = {.unit_id = unit, .chan = s_adc_chan, .atten = ADC_ATTEN_DB_12,
                                            .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&ccfg, &s_adc_cali) != ESP_OK) s_adc_cali = NULL;
}

/* Borrow the heater RX pin from the UART for well under a millisecond while the line is idle, read its
 * voltage, then hand it back. The pull-up is off while it is analog, so this is the divider's own output. */
static void adc_probe_read(void)
{
    if (!s_adc || !s_adc_cali) return;
    adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_config_channel(s_adc, s_adc_chan, &cc) != ESP_OK) return;
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_adc_chan, &raw) == ESP_OK && adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) == ESP_OK) {
            sum += mv;
            n++;
        }
    }
    /* RX only: re-routing TX could glitch the heater's input */
    uart_set_pin(HEATER_UART, UART_PIN_NO_CHANGE, CONFIG_AUTOTERM_HEATER_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_pullup_en(CONFIG_AUTOTERM_HEATER_RX_GPIO);
    if (n) s_heater_rx_mv = sum / n;
}

static void probe_task(void *arg)
{
    const int pins[2] = {CONFIG_AUTOTERM_HEATER_RX_GPIO, CONFIG_AUTOTERM_PANEL_RX_GPIO};
    adc_probe_init();
    for (;;) {
        int high[2] = {0, 0}, edges[2] = {0, 0}, last[2] = {gpio_get_level(pins[0]), gpio_get_level(pins[1])};
        for (int t = 0; t < 1000; t++) {
            for (int i = 0; i < 2; i++) {
                int l = gpio_get_level(pins[i]);
                high[i] += l;
                edges[i] += l != last[i];
                last[i] = l;
            }
            vTaskDelay(1);
        }
        for (int i = 0; i < 2; i++) {
            s_rx_high_pct[i] = high[i] / 10;
            s_rx_edges[i] = edges[i];
        }
        /* only between frames: the line must have stayed high for the last 60 ms */
        int quiet = 0;
        while (quiet < 60) {
            quiet = gpio_get_level(pins[0]) ? quiet + 1 : 0;
            vTaskDelay(1);
        }
        adc_probe_read();
    }
}

int at_app_heater_rx_mv(void) { return s_heater_rx_mv; }

void at_app_rx_probe(int high_pct[2], int edges[2])
{
    for (int i = 0; i < 2; i++) {
        high_pct[i] = s_rx_high_pct[i];
        edges[i] = s_rx_edges[i];
    }
}

/* ---- tasks ------------------------------------------------------------------- */

static void bridge_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_core_lock, portMAX_DELAY);
        at_core_step(&s_core, at_now_ms());
        xSemaphoreGive(s_core_lock);
        vTaskDelay(1); /* 1 ms tick: a byte at 2400 baud takes ~4 ms */
    }
}

static bool core_cmd(bool (*fn)(at_core_t *, uint32_t))
{
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    bool ok = fn(&s_core, at_now_ms());
    xSemaphoreGive(s_core_lock);
    return ok;
}

static void snapshot_locked(at_snapshot_t *s)
{
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    at_core_snapshot(&s_core, at_now_ms(), s);
    xSemaphoreGive(s_core_lock);
}

static void boot_burst_task(void *arg);

static void ops_start(void *ctx) { (void)ctx; core_cmd(at_cmd_start_thermostat); }
static void ops_stop(void *ctx) { (void)ctx; core_cmd(at_cmd_stop); }
static const at_ctl_ops_t s_ops = {ops_start, ops_stop, NULL};

static volatile float s_preheat_temp = AT_PREHEAT_TEMP_DEFAULT;
static uint32_t s_last_preheat_stop_ms;

static void ctl_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        at_settings_t cfg;
        at_settings_get(&cfg);
        at_snapshot_t snap;
        snapshot_locked(&snap);
        uint32_t now = at_now_ms();

        xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
        at_ctl_tick(&s_ctl, &snap, now, &s_ops);
        bool thermostat = s_ctl.autot.enabled;
        xSemaphoreGive(s_ctl_lock);
        at_app_fuel_tick(&snap, now);

        /* A timed preheat also ends when the coolant is hot enough; the thermostat has its own limits. */
        if (!thermostat && !cfg.bypass && snap.have_status && !snap.stale &&
            (snap.status.state_raw == 0x02 || snap.status.state_raw == 0x03) &&
            snap.status.coolant_temp >= s_preheat_temp && now - s_last_preheat_stop_ms > 60000u) {
            s_last_preheat_stop_ms = now;
            ESP_LOGI(TAG, "coolant %.1f >= preheat temperature %.1f -> stop", snap.status.coolant_temp, (double)s_preheat_temp);
            core_cmd(at_cmd_stop);
        }

        if (cfg.debug_mode && !cfg.bypass &&
            now - s_last_debug_ms >= (uint32_t)(cfg.debug_interval_s < 5 ? 5 : cfg.debug_interval_s) * 1000u) {
            s_last_debug_ms = now;
            core_cmd(at_cmd_send_pubr0);
        }
    }
}

esp_err_t at_app_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_core_lock = xSemaphoreCreateMutex();
    s_ctl_lock = xSemaphoreCreateMutex();

    at_settings_t cfg;
    at_settings_get(&cfg);

    ESP_ERROR_CHECK(uart_setup(HEATER_UART, CONFIG_AUTOTERM_HEATER_TX_GPIO, CONFIG_AUTOTERM_HEATER_RX_GPIO));
    ESP_ERROR_CHECK(uart_setup(PANEL_UART, CONFIG_AUTOTERM_PANEL_TX_GPIO, CONFIG_AUTOTERM_PANEL_RX_GPIO));

    at_port_t heater = {uart_port_read, uart_port_write, (void *)(intptr_t)HEATER_UART};
    at_port_t panel = {uart_port_read, uart_port_write, (void *)(intptr_t)PANEL_UART};
    at_core_init(&s_core, heater, panel);
    at_core_set_bypass(&s_core, cfg.bypass);

    at_ctl_init(&s_ctl, cfg.auto_target, cfg.freeze_target);
    at_band_t band;
    if (at_settings_band_load(&band)) {
        at_ctl_set_auto_range(&s_ctl, band.low, band.high);
        /* settings saved before the limits existed may sit outside them */
        at_ctl_set_hysteresis(&s_ctl, band.hyst_min < 2 ? 2 : (band.hyst_min > 7 ? 7 : band.hyst_min),
                              band.hyst_max < 1 ? 1 : (band.hyst_max > 3 ? 3 : band.hyst_max));
    } else {
        at_ctl_set_auto_range(&s_ctl, 19.0f, 22.0f); /* narrowest default band that fits the default hysteresis */
        at_ctl_set_hysteresis(&s_ctl, 2.0f, 1.0f);   /* defaults: start 2 C below the range, stop 1 C above it */
    }
    s_preheat_temp = at_settings_preheat_temp_load();
    s_ctl.use_external = cfg.use_external_temp;
    s_ctl.autot.enabled = cfg.auto_enabled; /* no immediate action at boot, the loop decides */
    s_ctl.freeze.enabled = cfg.freeze_enabled;

    at_app_fuel_init();
    xTaskCreate(bridge_task, "bridge", 6144, NULL, configMAX_PRIORITIES - 4, NULL);
    xTaskCreate(ctl_task, "thermostat", 4096, NULL, 5, NULL);
    xTaskCreate(probe_task, "probe", 3072, NULL, 2, NULL);
    if (cfg.debug_mode && !cfg.bypass) {
        xTaskCreate(boot_burst_task, "boot-burst", 2048, NULL, 4, NULL);
    }
    s_started = true;
    at_timers_start();
    ESP_LOGI(TAG, "bridge running: heater UART%d (RX %d TX %d), controller UART%d (RX %d TX %d), %d baud",
             HEATER_UART, CONFIG_AUTOTERM_HEATER_RX_GPIO, CONFIG_AUTOTERM_HEATER_TX_GPIO, PANEL_UART,
             CONFIG_AUTOTERM_PANEL_RX_GPIO, CONFIG_AUTOTERM_PANEL_TX_GPIO, CONFIG_AUTOTERM_BAUD);
    return ESP_OK;
}

/* ---- view -------------------------------------------------------------------- */

void at_app_view(at_view_t *v)
{
    memset(v, 0, sizeof(*v));
    at_settings_t cfg;
    at_settings_get(&cfg);
    snapshot_locked(&v->snap);
    uint32_t now = at_now_ms();

    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    v->auto_enabled = s_ctl.autot.enabled;
    v->auto_target = s_ctl.autot.target;
    v->auto_low = s_ctl.autot.low;
    v->auto_high = s_ctl.autot.high;
    v->hyst_min = s_ctl.autot.hyst_min;
    v->hyst_max = s_ctl.autot.hyst_max;
    v->preheat_temp = s_preheat_temp;
    v->freeze_enabled = s_ctl.freeze.enabled;
    v->freeze_target = s_ctl.freeze.target;
    memcpy(v->auto_note, s_ctl.autot.note, sizeof(v->auto_note));
    memcpy(v->freeze_note, s_ctl.freeze.note, sizeof(v->freeze_note));
    uint32_t age;
    v->have_effective_temp =
        at_ctl_effective_temp(&s_ctl, &v->snap, now, &v->effective_temp, &age, &v->effective_is_external);
    v->have_external = s_ctl.have_external && now - s_ctl.external_ms <= AT_CTL_EXT_STALE_MS;
    v->external_temp = s_ctl.external_temp;
    xSemaphoreGive(s_ctl_lock);

    v->preheat_minutes = cfg.preheat_minutes;
    v->debug_mode = cfg.debug_mode;
    v->debug_interval_s = cfg.debug_interval_s;
    v->use_external_temp = cfg.use_external_temp;
}

/* ---- commands ------------------------------------------------------------------ */

bool at_app_start_preheat(int minutes)
{
    if (minutes <= 0) {
        at_settings_t cfg;
        at_settings_get(&cfg);
        minutes = cfg.preheat_minutes;
    }
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    bool ok = at_cmd_start_preheat(&s_core, minutes, at_now_ms());
    xSemaphoreGive(s_core_lock);
    return ok;
}

bool at_app_set_preheat_duration(int minutes)
{
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    bool ok = at_cmd_set_preheat_duration(&s_core, minutes, at_now_ms());
    xSemaphoreGive(s_core_lock);
    return ok;
}

bool at_app_start_thermostat(void) { return core_cmd(at_cmd_start_thermostat); }
bool at_app_stop(void) { return core_cmd(at_cmd_stop); }
bool at_app_start_pump(void) { return core_cmd(at_cmd_start_pump); }
bool at_app_send_pubr0(void) { return core_cmd(at_cmd_send_pubr0); }

bool at_app_send_pubr0_variant(int variant)
{
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    bool ok = at_cmd_send_pubr0_variant(&s_core, variant, at_now_ms());
    xSemaphoreGive(s_core_lock);
    return ok;
}

/* Debug mode on at boot: a PUBR0 handshake every second for the first 30 s, in case the heater only
 * takes it while it is starting up (the box powers up with it, off the same red wire). */
static void boot_burst_task(void *arg)
{
    for (int i = 0; i < 30; i++) {
        core_cmd(at_cmd_send_pubr0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

static void persist(void (*mutate)(at_settings_t *, const void *), const void *arg)
{
    at_settings_t cfg;
    at_settings_get(&cfg);
    mutate(&cfg, arg);
    at_settings_set(&cfg);
}

static void m_auto(at_settings_t *c, const void *a) { c->auto_enabled = *(const bool *)a; }
static void m_auto_t(at_settings_t *c, const void *a) { c->auto_target = *(const float *)a; }
static void m_frz(at_settings_t *c, const void *a) { c->freeze_enabled = *(const bool *)a; }
static void m_frz_t(at_settings_t *c, const void *a) { c->freeze_target = *(const float *)a; }
static void m_pre(at_settings_t *c, const void *a) { c->preheat_minutes = (uint16_t)*(const int *)a; }
static void m_byp(at_settings_t *c, const void *a) { c->bypass = *(const bool *)a; }
static void m_ext(at_settings_t *c, const void *a) { c->use_external_temp = *(const bool *)a; }
static void m_dbg_on(at_settings_t *c, const void *a) { c->debug_mode = *(const bool *)a; }
static void m_dbg_iv(at_settings_t *c, const void *a) { c->debug_interval_s = (uint16_t)*(const int *)a; }

void at_app_set_auto(bool enabled)
{
    at_snapshot_t snap;
    snapshot_locked(&snap);
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_ctl_set_auto(&s_ctl, enabled, &snap, at_now_ms(), &s_ops);
    xSemaphoreGive(s_ctl_lock);
    persist(m_auto, &enabled);
}

static void save_band(void)
{
    at_band_t b;
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    b.low = s_ctl.autot.low;
    b.high = s_ctl.autot.high;
    b.hyst_min = s_ctl.autot.hyst_min;
    b.hyst_max = s_ctl.autot.hyst_max;
    xSemaphoreGive(s_ctl_lock);
    at_settings_band_save(&b);
}

void at_app_set_auto_target(float target)
{
    if (target < 0) target = 0;
    if (target > 30) target = 30;
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_ctl_set_auto_target(&s_ctl, target);
    xSemaphoreGive(s_ctl_lock);
    persist(m_auto_t, &target);
    save_band();
}

/* The range must be at least hyst_min + hyst_max wide (call with s_ctl_lock held). */
static void widen_band_locked(void)
{
    float gap = s_ctl.autot.hyst_min + s_ctl.autot.hyst_max;
    float low = s_ctl.autot.low, high = s_ctl.autot.high;
    if (high - low >= gap) return;
    high = low + gap;
    if (high > 30) {
        high = 30;
        low = high - gap;
        if (low < 0) low = 0;
    }
    at_ctl_set_auto_range(&s_ctl, low, high);
}

void at_app_set_auto_range(float low, float high)
{
    if (low < 0) low = 0;
    if (high > 30) high = 30;
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_ctl_set_auto_range(&s_ctl, low, high);
    widen_band_locked();
    float mid = s_ctl.autot.target;
    xSemaphoreGive(s_ctl_lock);
    persist(m_auto_t, &mid);
    save_band();
}

/* ---- thermostat runs owned by a timer ------------------------------------------------------------ */

static float s_tm_prev_low, s_tm_prev_high, s_tm_low, s_tm_high;
static bool s_tm_have;

bool at_app_timer_thermostat_begin(int low_c, int high_c)
{
    at_snapshot_t snap;
    snapshot_locked(&snap);
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    s_tm_prev_low = s_ctl.autot.low;
    s_tm_prev_high = s_ctl.autot.high;
    at_ctl_set_auto_range(&s_ctl, (float)low_c, (float)high_c);
    widen_band_locked();
    s_tm_low = s_ctl.autot.low;
    s_tm_high = s_ctl.autot.high;
    s_tm_have = true;
    at_ctl_set_auto(&s_ctl, true, &snap, at_now_ms(), &s_ops);
    xSemaphoreGive(s_ctl_lock);
    return true;
}

void at_app_timer_thermostat_end(bool disarm)
{
    at_snapshot_t snap;
    snapshot_locked(&snap);
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    if (disarm) {
        at_ctl_set_auto(&s_ctl, false, &snap, at_now_ms(), &s_ops);
    }
    if (s_tm_have && s_ctl.autot.low == s_tm_low && s_ctl.autot.high == s_tm_high) {
        at_ctl_set_auto_range(&s_ctl, s_tm_prev_low, s_tm_prev_high);
    }
    s_tm_have = false;
    xSemaphoreGive(s_ctl_lock);
}

float at_app_band_min_gap(void)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    float gap = s_ctl.autot.hyst_min + s_ctl.autot.hyst_max;
    xSemaphoreGive(s_ctl_lock);
    return gap;
}

void at_app_set_preheat_temp(float celsius)
{
    if (celsius < 20) celsius = 20;
    if (celsius > 95) celsius = 95;
    s_preheat_temp = celsius;
    at_settings_preheat_temp_save(celsius);
}

void at_app_set_hysteresis(float hyst_min, float hyst_max)
{
    if (hyst_min < 2) hyst_min = 2;
    if (hyst_min > 7) hyst_min = 7;
    if (hyst_max < 1) hyst_max = 1;
    if (hyst_max > 3) hyst_max = 3;
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_ctl_set_hysteresis(&s_ctl, hyst_min, hyst_max);
    widen_band_locked();
    float mid = s_ctl.autot.target;
    xSemaphoreGive(s_ctl_lock);
    persist(m_auto_t, &mid);
    save_band();
}

void at_app_set_freeze(bool enabled)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_ctl_set_freeze(&s_ctl, enabled);
    xSemaphoreGive(s_ctl_lock);
    persist(m_frz, &enabled);
}

void at_app_set_freeze_target(float target)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_ctl_set_freeze_target(&s_ctl, target);
    target = s_ctl.freeze.target;
    xSemaphoreGive(s_ctl_lock);
    persist(m_frz_t, &target);
}

void at_app_set_preheat_minutes(int minutes)
{
    if (minutes < 1) minutes = 1;
    if (minutes > 600) minutes = 600;
    persist(m_pre, &minutes);
}

void at_app_set_bypass(bool on)
{
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    at_core_set_bypass(&s_core, on);
    xSemaphoreGive(s_core_lock);
    persist(m_byp, &on);
}

void at_app_set_debug(bool on, int interval_s)
{
    persist(m_dbg_on, &on);
    if (interval_s > 0) {
        if (interval_s < 5) interval_s = 5;
        if (interval_s > 3600) interval_s = 3600;
        persist(m_dbg_iv, &interval_s);
    }
}

void at_app_set_use_external_temp(bool on)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    s_ctl.use_external = on;
    xSemaphoreGive(s_ctl_lock);
    persist(m_ext, &on);
}

void at_app_set_external_temp(float celsius)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_ctl_set_external(&s_ctl, celsius, at_now_ms());
    xSemaphoreGive(s_ctl_lock);
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    at_core_set_room_temp(&s_core, celsius, at_now_ms());
    xSemaphoreGive(s_core_lock);
}

int at_app_log_copy(at_log_entry_t *out, int max)
{
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    int n = at_core_log_copy(&s_core, out, max);
    xSemaphoreGive(s_core_lock);
    return n;
}
