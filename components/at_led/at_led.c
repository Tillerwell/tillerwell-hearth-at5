#include "at_led.h"

#include <stdint.h>

#include "at_app.h"
#include "at_net.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"

#if CONFIG_AUTOTERM_LED_GPIO >= 0

static const char *TAG = "led";

typedef struct { uint8_t r, g, b; } colour_t;

/* Colour table (full brightness; scaled by the brightness settings). */
static const colour_t UNCONFIGURED = {0, 0, 255}; /* blue: no Wi-Fi network saved yet */
static const colour_t PAIRED = {0, 255, 0};       /* green: on the network */
static const colour_t DROPPED = {255, 150, 0};    /* yellow: Wi-Fi dropped (the WS2812's green is strong, so less of it) */
static const colour_t MATTER = {255, 150, 0}; /* yellow: Matter mode (the WS2812's green is strong, so less of it) */
static const colour_t WARN = {255, 150, 0};    /* yellow: button held 5 s, keep going to reset */
static const colour_t RESET = {255, 0, 0};    /* red: releasing resets */

#define PAIRED_BRIGHTNESS_PCT 10 /* solid green in normal working */
#define TICK_MS 50
#define SLOW_HALF_PERIOD_MS 1000    /* slow flash: unconfigured, button warning */
#define OFFLINE_HALF_PERIOD_MS 1500 /* slow blink: Matter paired but not connected */
#define FAST_HALF_PERIOD_MS 150     /* fast flash: Wi-Fi dropped, button will reset */
#define PULSE_PERIOD_MS 2000        /* pulse: Matter mode waiting to be paired */
#define SETUP_BRIGHT_US (15 * 60 * 1000000LL) /* waiting to be set up: bright for this long after power-up */

static led_strip_handle_t s_strip;
static volatile unsigned s_held_ms;
static volatile bool s_setup_link;

void at_led_button_held(unsigned held_ms) { s_held_ms = held_ms; }
void at_led_setup_link(bool connected) { s_setup_link = connected; }

static bool in_matter_mode(void)
{
    at_settings_t cfg;
    at_settings_get(&cfg);
    return cfg.mode == AT_MODE_MATTER;
}

/* level 0..100 on top of the brightness setting; bright picks the setup/button brightness, full ignores the
 * settings altogether (100%) */
static void show_pct(colour_t c, unsigned level, unsigned pct)
{
    const uint32_t b = pct * level;
    /* This board's LED takes red and green the other way round to the usual WS2812 order (red showed as green),
     * and the driver sends its first argument first, so swap them here. */
    if (level) led_strip_set_pixel(s_strip, 0, c.g * b / 10000, c.r * b / 10000, c.b * b / 10000);
    else led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
}

static void show_at(colour_t c, unsigned level, bool bright, bool full)
{
    show_pct(c, level, full ? 100 : bright ? CONFIG_AUTOTERM_LED_SETUP_BRIGHTNESS : CONFIG_AUTOTERM_LED_BRIGHTNESS);
}

static void show(colour_t c, unsigned level, bool bright) { show_at(c, level, bright, false); }

static unsigned flash(uint32_t t, unsigned half) { return (t / half) % 2 == 0 ? 100 : 0; }

/* Fades up and down, never quite off so it does not read as a blink */
static unsigned pulse(uint32_t t)
{
    const unsigned p = t % PULSE_PERIOD_MS, half = PULSE_PERIOD_MS / 2;
    const unsigned ramp = p < half ? p : PULSE_PERIOD_MS - p; /* 0..half */
    return 10 + 90 * ramp / half;
}

static void show_status(uint32_t t)
{
    /* waiting to be set up is bright at first, then drops to normal so an unused box is not a beacon */
    const bool setup_bright = esp_timer_get_time() < SETUP_BRIGHT_US;
    if (in_matter_mode()) {
        at_matter_info_t m;
        at_matter_get_info(&m);
        if (!m.commissioned) show(MATTER, pulse(t), setup_bright);
        else if (m.online) show(MATTER, 100, false);
        else show(MATTER, flash(t, OFFLINE_HALF_PERIOD_MS), false);
        return;
    }
    if (at_net_connected()) show_pct(PAIRED, 100, PAIRED_BRIGHTNESS_PCT);
    else if (at_net_has_saved_network()) show_at(DROPPED, flash(t, FAST_HALF_PERIOD_MS), true, true);
    else show(UNCONFIGURED, flash(t, s_setup_link ? FAST_HALF_PERIOD_MS : SLOW_HALF_PERIOD_MS), setup_bright);
}

static void led_task(void *arg)
{
    uint32_t t = 0; /* ms */
    for (;;) {
        const unsigned held = s_held_ms;
        if (held >= AT_BUTTON_CANCEL_MS) {
            show_status(t); /* held too long: cancelled, releasing does nothing */
        } else if (held >= AT_BUTTON_RESET_MS) {
            show(RESET, flash(t, FAST_HALF_PERIOD_MS), true);
        } else if (held >= AT_BUTTON_WARN_MS) {
            show(WARN, flash(t, SLOW_HALF_PERIOD_MS), true);
        } else {
            show_status(t); /* also while held under 5 s */
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        t += TICK_MS;
    }
}

esp_err_t at_led_start(void)
{
    led_strip_config_t sc = {
        .strip_gpio_num = CONFIG_AUTOTERM_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rc = {.resolution_hz = 10 * 1000 * 1000};
    esp_err_t err = led_strip_new_rmt_device(&sc, &rc, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED init failed: %s", esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);
    xTaskCreate(led_task, "led", 3072, NULL, 1, NULL);
    return ESP_OK;
}

#else

esp_err_t at_led_start(void) { return ESP_OK; }
void at_led_button_held(unsigned held_ms) { (void)held_ms; }
void at_led_setup_link(bool connected) { (void)connected; }

#endif
