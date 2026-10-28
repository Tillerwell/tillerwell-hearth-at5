#include "at_access.h"
#include "at_app.h"
#include "at_led.h"
#include "sdkconfig.h"
#include "at_ble.h"
#include "at_matter.h"
#include "at_mqtt.h"
#include "at_net.h"
#include "at_web.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "main";

void at_console_start(void);

static void on_ip(bool connected)
{
    if (connected) {
        at_app_clock_sntp_start();   /* timers need real time */
        at_web_start();
        at_mqtt_start();
    }
}

#if CONFIG_AUTOTERM_FACTORY_RESET_GPIO >= 0
/* What happens is decided when the button is released:
 *   held under 5 s     release: nothing
 *   held 5 s .. 10 s   release: nothing (LED flashes slow yellow, a warning that a reset is coming)
 *   held 10 s .. 15 s  release: factory reset, back to Wi-Fi mode (LED flashes fast red)
 *   held 15 s or more  release: nothing, so a reset can be called off by holding on (LED back to the status)
 * The button never enters Matter mode: units are always paired over Wi-Fi. */
static void button_task(void *arg)
{
    const gpio_num_t pin = CONFIG_AUTOTERM_FACTORY_RESET_GPIO;
    gpio_config_t io = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    int held = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        const bool down = gpio_get_level(pin) == 0;
        if (down) {
            held++;
        } else if (held > 0) {
            const unsigned ms = held * 100;
            held = 0;
            at_led_button_held(0);
            if (ms < AT_BUTTON_WARN_MS) {
                at_access_button_pressed(); /* confirms a phone's request to become the admin, if one is waiting */
            } else if (ms >= AT_BUTTON_CANCEL_MS) {
                ESP_LOGI(TAG, "button released after %u s: held past %d s, cancelled", ms / 1000,
                         AT_BUTTON_CANCEL_MS / 1000);
            } else if (ms >= AT_BUTTON_RESET_MS) {
                ESP_LOGW(TAG, "button released after %u s: factory reset", ms / 1000);
                at_settings_factory_reset_to_mode(AT_MODE_WIFI);
            }
        }
        at_led_button_held(held * 100);
    }
}
#endif

void app_main(void)
{
    ESP_ERROR_CHECK(at_settings_init());
    at_access_start(); /* who may use the box: the user table in NVS */
    at_led_start(); /* status LED; not fatal if it fails */

    /* The relay comes first: the controller must keep working whatever the
     * network, Matter or MQTT are doing. */
    ESP_ERROR_CHECK(at_app_start());

    at_settings_t cfg;
    at_settings_get(&cfg);
    const bool use_matter = cfg.mode == AT_MODE_MATTER; /* chosen on the web page / console, default Wi-Fi */
    ESP_LOGI(TAG, "connection mode: %s", use_matter ? "Matter" : "Wi-Fi (web UI + MQTT)");

    ESP_ERROR_CHECK(at_net_init());
    /* Wi-Fi mode: we run Wi-Fi (and the setup access point). Matter mode: Matter owns
     * Wi-Fi and the web page / MQTT start once it has an address. */
    ESP_ERROR_CHECK(at_net_start(on_ip, !use_matter));
    if (!use_matter) {
        at_web_start(); /* also serves the setup page on the setup access point */
        if (at_ble_start() != ESP_OK) {
            ESP_LOGE(TAG, "Bluetooth setup failed to start");
        }
    }
    if (use_matter) {
        ESP_ERROR_CHECK(at_matter_start());
    }

#if CONFIG_AUTOTERM_CONSOLE
    at_console_start();
#endif
#if CONFIG_AUTOTERM_FACTORY_RESET_GPIO >= 0
    xTaskCreate(button_task, "button", 2048, NULL, 2, NULL);
#endif
    if (!use_matter) {
        /* The broker listens on every interface and our client dials it over loopback, so neither needs an
         * address: start them now rather than when Wi-Fi comes up, so MQTT is ready the moment it does (and the
         * event task is not held up starting them). Matter mode still waits for Wi-Fi: over Thread alone it
         * may never come, and Matter needs the memory more. */
        at_mqtt_start();
    }
}
