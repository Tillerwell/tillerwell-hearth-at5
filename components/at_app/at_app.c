#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "at_app.h"
#include "at_runlog.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
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
static SemaphoreHandle_t s_persist_lock; /* guards s_rest, s_learn_save and their NVS writes; taken before s_ctl_lock */
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
    bool probed = false;
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
        /* Once only: borrowing the pin glitches the UART into reading a junk
         * byte, and every second of that upset the Comfort Controller. The
         * idle voltage does not change while running anyway. */
        if (probed) {
            continue;
        }
        probed = true;
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

/* ---- Heater Care persistence ------------------------------------------------------------------------------ */

#define KEY_LEARN "thermo_learn"
#define KEY_REST  "ctl_rest"
#define KEY_COOL  "ctl_cool"
#define KEY_COAST "ctl_coast"
#define KEY_RUNS  "ctl_runs"
#define REST_VERSION 2

typedef struct {
    uint8_t version;
    uint8_t run_mark;
    uint8_t have_stop;
    uint8_t pad;
    int64_t stop_epoch;
    int64_t flame_epoch;        /* v2 (1.51): wall clock of the running run's flame established, 0 unknown */
} rest_blob_t;

/* The v1 blob, before flame_epoch: read once on the first boot after the update. */
typedef struct {
    uint8_t version;
    uint8_t run_mark;
    uint8_t have_stop;
    uint8_t pad;
    int64_t stop_epoch;
} rest_blob_v1_t;

static rest_blob_t s_rest;
static at_learn_store_t s_learn_save;   /* a copy to write outside the lock (too big for the task's stack) */
static at_cd_store_t s_cool_save;      /* learned cooldown lengths (1.52) */
static at_coast_store_t s_coast_save;  /* learned rise after a stop at the top (1.53) */
static at_runlog_store_t s_runs_save;  /* Heater Care's run log (1.55): the "Why?" screen survives a restart */

/* The firmware's build date (a clock earlier than this is wrong), from __DATE__ "Mmm dd yyyy". */
static int64_t build_epoch(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    int m = 0;
    for (int i = 0; i < 12; i++) {
        if (!strncmp(d, months + i * 3, 3)) m = i + 1;
    }
    int day = atoi(d + 4), year = atoi(d + 7);
    if (!m || !day || year < 2024) return 0;
    return (int64_t)at_days_from_civil(year, m, day) * 86400;
}

/* Called from the thermostat task after each tick: the writes the control loop asked for. */
static void persist_ctl(void)
{
    bool w_learn = false, w_rest = false, w_cool = false, w_coast = false, w_runs = false;
    /* called from the thermostat task and the HTTP task (Reset learning, a backup restore): one at a time, so the
     * copies are not overwritten while NVS is writing them */
    xSemaphoreTake(s_persist_lock, portMAX_DELAY);
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    if (s_ctl.learn.dirty) {
        s_learn_save = s_ctl.learn.st;
        s_ctl.learn.dirty = false;
        w_learn = true;
    }
    if (s_ctl.cd.dirty) {
        s_cool_save = s_ctl.cd.st;
        s_ctl.cd.dirty = false;
        w_cool = true;
    }
    if (s_ctl.coast.dirty) {
        s_coast_save = s_ctl.coast.st;
        s_ctl.coast.dirty = false;
        w_coast = true;
    }
    if (at_runlog_changed(&s_ctl.learn, &s_runs_save)) {
        at_runlog_save(&s_ctl.learn, &s_runs_save);
        w_runs = true;
    }
    if (s_ctl.save_run_mark) {
        s_ctl.save_run_mark = false;
        if (!s_rest.run_mark) {
            s_rest.run_mark = 1;
            w_rest = true;
        }
    }
    if (s_ctl.save_flame) {
        s_ctl.save_flame = false;
        if (s_rest.flame_epoch != s_ctl.save_flame_epoch) {
            s_rest.flame_epoch = s_ctl.save_flame_epoch;
            w_rest = true;
        }
    }
    if (s_ctl.save_stop) {
        s_ctl.save_stop = false;
        s_rest.run_mark = 0;
        s_rest.flame_epoch = 0;
        /* stop_epoch 0: stopped while the clock was not set; the next boot still owes a rest, counted from boot */
        s_rest.have_stop = 1;
        s_rest.stop_epoch = s_ctl.save_stop_epoch;
        w_rest = true;
    }
    xSemaphoreGive(s_ctl_lock);
    if (w_learn) at_settings_blob_save(KEY_LEARN, &s_learn_save, sizeof(s_learn_save));
    if (w_cool) at_settings_blob_save(KEY_COOL, &s_cool_save, sizeof(s_cool_save));
    if (w_coast) at_settings_blob_save(KEY_COAST, &s_coast_save, sizeof(s_coast_save));
    if (w_runs) at_settings_blob_save(KEY_RUNS, &s_runs_save, sizeof(s_runs_save));
    if (w_rest) {
        s_rest.version = REST_VERSION;
        at_settings_blob_save(KEY_REST, &s_rest, sizeof(s_rest));
    }
    xSemaphoreGive(s_persist_lock);
}

static bool wall_clock(int64_t *epoch)
{
    at_timer_status_t ts;
    at_app_timer_status(&ts);
    time_t now = time(NULL);
    *epoch = (int64_t)now;
    return ts.time_set && now > 1700000000;
}

static volatile float s_preheat_temp = AT_PREHEAT_TEMP_DEFAULT;
static volatile int s_pump_minutes = AT_PUMP_MINUTES_DEFAULT; /* water pump run time, minutes */
static uint32_t s_last_preheat_stop_ms;

static void ctl_task(void *arg)
{
    uint32_t panel_control_seq = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        at_settings_t cfg;
        at_settings_get(&cfg);
        at_snapshot_t snap;
        snapshot_locked(&snap);
        uint32_t now = at_now_ms();
        int64_t epoch;
        bool clock_ok = wall_clock(&epoch);

        xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
        at_ctl_set_clock(&s_ctl, clock_ok, epoch);
        if (snap.panel_control_seq != panel_control_seq) {
            panel_control_seq = snap.panel_control_seq;
            at_ctl_disarm_auto(&s_ctl, "physical controller took control -> Auto disabled");
            at_settings_t panel_cfg;
            at_settings_get(&panel_cfg);
            if (panel_cfg.auto_enabled) {
                panel_cfg.auto_enabled = false;
                at_settings_set(&panel_cfg);
            }
        }
        at_ctl_tick(&s_ctl, &snap, now, &s_ops);
        bool thermostat = s_ctl.autot.enabled;
        xSemaphoreGive(s_ctl_lock);
        persist_ctl();
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

static at_boot_mark_t s_marks[AT_BOOT_MARKS];
static int s_mark_count;

void at_app_boot_mark(const char *stage)
{
    if (s_mark_count >= AT_BOOT_MARKS) return;
    s_marks[s_mark_count++] = (at_boot_mark_t){stage, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                                               (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)};
}

int at_app_boot_marks(at_boot_mark_t *out, int max)
{
    int n = s_mark_count < max ? s_mark_count : max;
    memcpy(out, s_marks, sizeof(*out) * (size_t)n);
    return n;
}

esp_err_t at_app_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_core_lock = xSemaphoreCreateMutex();
    s_ctl_lock = xSemaphoreCreateMutex();
    s_persist_lock = xSemaphoreCreateMutex();

    at_settings_t cfg;
    at_settings_get(&cfg);

    ESP_ERROR_CHECK(uart_setup(HEATER_UART, CONFIG_AUTOTERM_HEATER_TX_GPIO, CONFIG_AUTOTERM_HEATER_RX_GPIO));
    ESP_ERROR_CHECK(uart_setup(PANEL_UART, CONFIG_AUTOTERM_PANEL_TX_GPIO, CONFIG_AUTOTERM_PANEL_RX_GPIO));

    at_port_t heater = {uart_port_read, uart_port_write, (void *)(intptr_t)HEATER_UART};
    at_port_t panel = {uart_port_read, uart_port_write, (void *)(intptr_t)PANEL_UART};
    at_core_init(&s_core, heater, panel);
    at_core_set_bypass(&s_core, cfg.bypass);
    at_core_set_use_room_temp(&s_core, cfg.use_external_temp);

    at_ctl_init(&s_ctl, cfg.auto_target, cfg.freeze_target);
    at_band_t band;
    if (at_settings_band_load(&band)) {
        /* one set temperature (1.49): the top of a saved range is the set temperature, the range is 1 C below it */
        at_ctl_set_auto_range(&s_ctl, band.high - AT_CTL_MIN_WIDTH, band.high);
    } else {
        at_ctl_set_auto_range(&s_ctl, 20.0f, 21.0f); /* a fresh box: a 1 C range, Heater Care widens it as needed */
    }
    if (at_settings_blob_load(KEY_LEARN, &s_learn_save, sizeof(s_learn_save))) {
        if (!at_learn_restore(&s_ctl.learn, &s_learn_save)) ESP_LOGW(TAG, "saved Heater Care learning rejected");
    }
    if (at_settings_blob_load(KEY_COOL, &s_cool_save, sizeof(s_cool_save))) {
        if (!at_cd_restore(&s_ctl.cd, &s_cool_save)) ESP_LOGW(TAG, "saved cooldown lengths rejected");
    }
    if (at_settings_blob_load(KEY_COAST, &s_coast_save, sizeof(s_coast_save))) {
        if (!at_coast_restore(&s_ctl.coast, &s_coast_save)) ESP_LOGW(TAG, "saved rise after a stop rejected");
    }
    /* a different size (the run's layout changed in an update) does not load: the log starts again, nothing else */
    if (at_settings_blob_load(KEY_RUNS, &s_runs_save, sizeof(s_runs_save))) {
        if (!at_runlog_restore(&s_ctl.learn, &s_runs_save)) ESP_LOGW(TAG, "saved run log rejected");
    }
    at_ctl_saved_t saved = {0};
    if (at_settings_blob_load(KEY_REST, &s_rest, sizeof(s_rest)) && s_rest.version == REST_VERSION) {
        saved.run_mark = s_rest.run_mark;
        saved.have_stop = s_rest.have_stop;
        saved.stop_epoch = s_rest.stop_epoch;
        saved.flame_epoch = s_rest.flame_epoch;
    } else {
        rest_blob_v1_t old;
        memset(&s_rest, 0, sizeof(s_rest));
        if (at_settings_blob_load(KEY_REST, &old, sizeof(old)) && old.version == 1) {
            /* an update from 1.50 or earlier: keep the owed rest, the running run's flame time is not known */
            s_rest.version = REST_VERSION;
            s_rest.run_mark = old.run_mark;
            s_rest.have_stop = old.have_stop;
            s_rest.stop_epoch = old.stop_epoch;
            saved.run_mark = old.run_mark;
            saved.have_stop = old.have_stop;
            saved.stop_epoch = old.stop_epoch;
        }
    }
    at_ctl_boot(&s_ctl, &saved, build_epoch(), at_now_ms());
    s_preheat_temp = at_settings_preheat_temp_load();
    uint16_t pump_min;
    if (at_settings_blob_load("pump_min", &pump_min, sizeof(pump_min)) && pump_min >= 1 && pump_min <= 1440) {
        s_pump_minutes = pump_min;
    }
    s_ctl.use_external = cfg.use_external_temp;
    s_ctl.autot.enabled = cfg.auto_enabled; /* no immediate action at boot, the loop decides */
    s_ctl.freeze.enabled = cfg.freeze_enabled;

    at_app_fuel_init();
    at_app_state_json_init();
    xTaskCreate(bridge_task, "bridge", 6144, NULL, configMAX_PRIORITIES - 4, NULL);
    xTaskCreate(ctl_task, "thermostat", 5120, NULL, 5, NULL); /* 1.47: Heater Care saves its learning from here */
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
    v->auto_target = s_ctl.autot.high;   /* the set temperature */
    v->auto_low = s_ctl.autot.low;
    v->auto_high = s_ctl.autot.high;
    v->hyst_min = 0.5f;   /* compatibility: older apps add these up for the narrowest range (1 C) */
    v->hyst_max = 0.5f;
    v->auto_start_at = at_ctl_auto_start_at(&s_ctl);
    at_ctl_status(&s_ctl, now, &v->thermo);
    v->clock_ok = s_ctl.clock_ok;
    v->epoch = s_ctl.epoch;
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
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_ctl_user_start(&s_ctl, at_now_ms());
    xSemaphoreGive(s_ctl_lock);
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

bool at_app_start_thermostat(void)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_ctl_user_start(&s_ctl, at_now_ms());
    xSemaphoreGive(s_ctl_lock);
    return core_cmd(at_cmd_start_thermostat);
}
bool at_app_stop(void) { return at_app_stop_from(AT_SRC_UNKNOWN); }

bool at_app_stop_from(at_src_t src)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_ctl_user_stop(&s_ctl, src, at_now_ms());
    at_ctl_disarm_auto(&s_ctl, "manual Hearth stop");
    xSemaphoreGive(s_ctl_lock);
    at_settings_t cfg;
    at_settings_get(&cfg);
    if (cfg.auto_enabled) {
        cfg.auto_enabled = false;
        at_settings_set(&cfg);
    }
    return core_cmd(at_cmd_stop_manual);
}
int at_app_pump_minutes(void) { return s_pump_minutes; }

void at_app_set_pump_minutes(int minutes)
{
    if (minutes < 1) minutes = 1;
    if (minutes > 1440) minutes = 1440;
    s_pump_minutes = minutes;
    uint16_t v = (uint16_t)minutes;
    at_settings_blob_save("pump_min", &v, sizeof(v));
}

bool at_app_start_pump(int minutes)
{
    if (minutes <= 0) minutes = s_pump_minutes;
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    bool ok = at_cmd_start_pump(&s_core, minutes, at_now_ms());
    xSemaphoreGive(s_core_lock);
    return ok;
}

/* Fuel priming and the fan test run the heater's parts by hand: only from idle, and never with the box's own
 * thermostat armed, which would start the heater on top of them. */
static bool idle_for_service(void)
{
    at_settings_t cfg;
    at_settings_get(&cfg);
    at_snapshot_t s;
    at_core_snapshot(&s_core, at_now_ms(), &s);
    return !cfg.auto_enabled && s.have_status && !s.stale && s.status.state_raw == 0 && !s.hearth_run;
}

static bool service_cmd(bool (*fn)(at_core_t *, uint32_t))
{
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    bool ok = idle_for_service() && fn(&s_core, at_now_ms());
    xSemaphoreGive(s_core_lock);
    return ok;
}

bool at_app_fuel_prime(void) { return service_cmd(at_cmd_fuel_prime); }
bool at_app_fan_test(void) { return service_cmd(at_cmd_fan_test); }

bool at_app_write_heater_settings(int wait_temp, int pump_standby)
{
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    bool ok = at_cmd_write_heater_settings(&s_core, wait_temp, pump_standby, at_now_ms());
    xSemaphoreGive(s_core_lock);
    return ok;
}
bool at_app_send_pubr0(void) { return core_cmd(at_cmd_send_pubr0); }

bool at_app_read_heater_info(void) { return core_cmd(at_cmd_read_heater_info); }

bool at_app_unlock(void)
{
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    at_snapshot_t s;
    at_core_snapshot(&s_core, at_now_ms(), &s);
    /* 0 idle, 1 locked / waiting: never in the middle of a run or its cooldown */
    bool ok = s.have_status && !s.stale && s.status.state_raw <= 1 && !s.hearth_run &&
              at_cmd_unlock(&s_core, at_now_ms());
    xSemaphoreGive(s_core_lock);
    return ok;
}

bool at_app_diag_stream(bool on)
{
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    bool ok = at_cmd_diag_stream(&s_core, on, at_now_ms());
    xSemaphoreGive(s_core_lock);
    return ok;
}

void at_app_heater_info(at_heater_info_t *out, uint32_t *now_ms)
{
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    at_core_heater_info(&s_core, out);
    *now_ms = at_now_ms();
    xSemaphoreGive(s_core_lock);
}

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

void at_app_set_auto(bool enabled) { at_app_set_auto_from(enabled, true, AT_SRC_UNKNOWN); }

void at_app_set_auto_from(bool enabled, bool finish_run, at_src_t src)
{
    at_snapshot_t snap;
    snapshot_locked(&snap);
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    if (enabled) {
        at_ctl_set_auto(&s_ctl, true, &snap, at_now_ms(), &s_ops);
    } else {
        at_ctl_auto_off(&s_ctl, finish_run, AT_HOLD_THERMO_OFF, src, &snap, at_now_ms(), &s_ops);
    }
    xSemaphoreGive(s_ctl_lock);
    persist(m_auto, &enabled);
}

void at_app_thermo_learn_reset(void)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_learn_reset(&s_ctl.learn);
    at_coast_reset(&s_ctl.coast);
    xSemaphoreGive(s_ctl_lock);
    ESP_LOGI(TAG, "Heater Care learning reset");
    persist_ctl();
}

void at_app_early_stop_seen(void)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    at_ctl_early_seen(&s_ctl);
    xSemaphoreGive(s_ctl_lock);
}

bool at_app_learn_snapshot(at_learn_t *out, at_ctl_early_t *early, int *n_early, uint32_t *now_ms)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    *out = s_ctl.learn;
    if (early && n_early) {
        *n_early = s_ctl.n_early;
        memcpy(early, s_ctl.early, sizeof(s_ctl.early[0]) * (size_t)s_ctl.n_early);
    }
    *now_ms = at_now_ms();
    xSemaphoreGive(s_ctl_lock);
    return true;
}

bool at_app_learn_put(const at_learn_store_t *st)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    bool ok = at_learn_restore(&s_ctl.learn, st);
    if (ok) {
        at_learn_check_limits(&s_ctl.learn, s_ctl.autot.high - s_ctl.autot.low);
        s_ctl.learn.dirty = true;
    }
    xSemaphoreGive(s_ctl_lock);
    if (ok) persist_ctl();
    return ok;
}

static void save_band(void)
{
    at_band_t b;
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    b.low = s_ctl.autot.low;
    b.high = s_ctl.autot.high;
    b.hyst_min = 0.5f;
    b.hyst_max = 0.5f;
    xSemaphoreGive(s_ctl_lock);
    at_settings_band_save(&b);
}

/* The user sets one temperature. It is the top of the range, and the range is always 1 C below it (Heater Care
 * lowers the working bottom further by itself). Call with s_ctl_lock held. */
static void set_temp_locked(float temp)
{
    if (temp < AT_CTL_MIN_WIDTH) temp = AT_CTL_MIN_WIDTH;
    if (temp > 30) temp = 30;
    at_ctl_set_auto_range(&s_ctl, temp - AT_CTL_MIN_WIDTH, temp);
}

void at_app_set_auto_target(float target) { at_app_set_auto_range_from(0, target, true, AT_SRC_UNKNOWN); }

void at_app_set_auto_range(float low, float high) { at_app_set_auto_range_from(low, high, true, AT_SRC_UNKNOWN); }

void at_app_set_auto_range_from(float low, float high, bool finish_run, at_src_t src)
{
    (void)low;   /* kept for older callers: only the top, the set temperature, is used */
    bool stop_now = false;
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    set_temp_locked(high);
    uint32_t now = at_now_ms();
    /* lowered below the room inside the minimum run: "Stop anyway" stops now; "Let it finish" lets the run reach
     * its minimum run, then the thermostat stops it at the new top as usual */
    if (!finish_run && s_ctl.autot.enabled && s_ctl.room_ok && s_ctl.room >= s_ctl.autot.high &&
        at_ctl_min_run_left_ms(&s_ctl, now) > 0) {
        at_ctl_user_stop(&s_ctl, src, now);
        stop_now = true;
    }
    float mid = s_ctl.autot.high;
    xSemaphoreGive(s_ctl_lock);
    if (stop_now) core_cmd(at_cmd_stop);
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
    (void)low_c;
    set_temp_locked((float)high_c);
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
        /* inside the run's minimum run the timer's stop waits for it (Heater Care) */
        at_ctl_auto_off(&s_ctl, true, AT_HOLD_TIMER_ENDED, AT_SRC_TIMER, &snap, at_now_ms(), &s_ops);
    }
    if (s_tm_have && s_ctl.autot.low == s_tm_low && s_ctl.autot.high == s_tm_high) {
        at_ctl_set_auto_range(&s_ctl, s_tm_prev_low, s_tm_prev_high);
    }
    s_tm_have = false;
    xSemaphoreGive(s_ctl_lock);
}

bool at_app_timer_can_take_over(void)
{
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    bool ok = s_ctl.held == AT_HOLD_TIMER_ENDED;
    xSemaphoreGive(s_ctl_lock);
    return ok;
}

float at_app_band_min_gap(void) { return AT_CTL_MIN_WIDTH; }

void at_app_set_preheat_temp(float celsius)
{
    if (celsius < 20) celsius = 20;
    if (celsius > 95) celsius = 95;
    s_preheat_temp = celsius;
    at_settings_preheat_temp_save(celsius);
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
    xSemaphoreTake(s_core_lock, portMAX_DELAY);
    at_core_set_use_room_temp(&s_core, on);
    xSemaphoreGive(s_core_lock);
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
