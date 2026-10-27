#include <string.h>

#include "at_app.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
#define NVS_NS   "autoterm"
#define NVS_KEY  "cfg"
#define CFG_VERSION 2

static at_settings_t s_cfg;
static SemaphoreHandle_t s_lock;

static void defaults(at_settings_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = CFG_VERSION;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(c->device_name, sizeof(c->device_name), "hearth-%02x%02x%02x", mac[3], mac[4], mac[5]);
    c->preheat_minutes = 30;
    c->auto_target = 20.0f;
    c->freeze_target = 5.0f;
    c->debug_interval_s = 60;
    c->use_external_temp = true;
}

esp_err_t at_settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    defaults(&s_cfg);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        at_settings_t tmp;
        size_t len = sizeof(tmp);
        if (nvs_get_blob(h, NVS_KEY, &tmp, &len) == ESP_OK && len == sizeof(tmp) && tmp.version == CFG_VERSION) {
            s_cfg = tmp;
        } else {
            ESP_LOGW(TAG, "no valid saved settings, using defaults");
        }
        nvs_close(h);
    }
    /* never trust strings read back from flash */
    s_cfg.device_name[sizeof(s_cfg.device_name) - 1] = 0;
    s_cfg.wifi_ssid[sizeof(s_cfg.wifi_ssid) - 1] = 0;
    s_cfg.wifi_pass[sizeof(s_cfg.wifi_pass) - 1] = 0;
    s_cfg.web_password[sizeof(s_cfg.web_password) - 1] = 0;
    return ESP_OK;
}

bool at_settings_blob_load(const char *key, void *out, size_t len)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t got = len;
        ok = nvs_get_blob(h, key, out, &got) == ESP_OK && got == len;
        nvs_close(h);
    }
    return ok;
}

esp_err_t at_settings_blob_save(const char *key, const void *in, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, in, len);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    return err;
}

/* The name the user gave the box in the app ("Autoterm 5D"). Kept in its own blob, not in at_settings_t, so adding it
 * doesn't invalidate saved settings on update; a factory reset erases it with the rest of NVS. */
#define DISPLAY_NAME_KEY "dispname"

void at_settings_display_name(char *out, size_t len)
{
    char buf[AT_DISPLAY_NAME_MAX] = {0};
    if (!at_settings_blob_load(DISPLAY_NAME_KEY, buf, sizeof(buf))) buf[0] = 0;
    buf[sizeof(buf) - 1] = 0;
    strlcpy(out, buf, len);
}

esp_err_t at_settings_display_name_set(const char *name)
{
    char buf[AT_DISPLAY_NAME_MAX] = {0};
    strlcpy(buf, name ? name : "", sizeof(buf));
    return at_settings_blob_save(DISPLAY_NAME_KEY, buf, sizeof(buf));
}

/* Network name (DHCP, mDNS): the display name made hostname-safe ("Autoterm 5D" -> "Autoterm-5D"), or the
 * hearth-xxxxxx id when no name is set or nothing usable is left of it. */
void at_settings_hostname(char *out, size_t len)
{
    char name[AT_DISPLAY_NAME_MAX];
    at_settings_display_name(name, sizeof(name));
    size_t n = 0;
    for (const char *p = name; *p && n < len - 1 && n < AT_HOSTNAME_MAX; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[n++] = c;
        else if ((c == ' ' || c == '-' || c == '_') && n > 0 && out[n - 1] != '-') out[n++] = '-';
    }
    while (n > 0 && out[n - 1] == '-') n--;
    out[n] = 0;
    if (n == 0) {
        at_settings_t cfg;
        at_settings_get(&cfg);
        strlcpy(out, cfg.device_name, len);
    }
}

float at_settings_preheat_temp_load(void)
{
    nvs_handle_t h;
    float v = AT_PREHEAT_TEMP_DEFAULT;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        float t;
        size_t len = sizeof(t);
        if (nvs_get_blob(h, "preheat_t", &t, &len) == ESP_OK && len == sizeof(t) && t >= 20 && t <= 95) v = t;
        nvs_close(h);
    }
    return v;
}

esp_err_t at_settings_preheat_temp_save(float celsius)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "preheat_t", &celsius, sizeof(celsius));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    return err;
}

bool at_settings_band_load(at_band_t *out)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(*out);
        ok = nvs_get_blob(h, "band", out, &len) == ESP_OK && len == sizeof(*out)
             && out->low <= out->high && out->low >= 0 && out->high <= 60 && out->hyst_min >= 0 && out->hyst_max >= 0;
        nvs_close(h);
    }
    return ok;
}

esp_err_t at_settings_band_save(const at_band_t *in)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "band", in, sizeof(*in));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    return err;
}

void at_settings_get(at_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t at_settings_set(const at_settings_t *in)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *in;
    s_cfg.version = CFG_VERSION;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &s_cfg, sizeof(s_cfg));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

void at_settings_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");
    nvs_flash_erase(); /* settings, and the Matter fabric/credentials in Matter builds */
    esp_restart();
}

/* Wipes everything (settings, Wi-Fi, the Matter pairing), then saves default settings with `mode` already chosen. */
void at_settings_factory_reset_to_mode(uint8_t mode)
{
    ESP_LOGW(TAG, "factory reset, then %s mode", mode == AT_MODE_MATTER ? "Matter" : "Wi-Fi");
    nvs_flash_erase();
    if (nvs_flash_init() == ESP_OK) {
        at_settings_t cfg;
        defaults(&cfg);
        cfg.mode = mode;
        at_settings_set(&cfg);
    }
    esp_restart();
}

static void factory_reset_cb(void *arg)
{
    at_settings_factory_reset();
}

void at_settings_factory_reset_soon(void)
{
    static esp_timer_handle_t t;
    esp_timer_create_args_t a = {.callback = factory_reset_cb, .name = "factory_reset"};
    if (esp_timer_create(&a, &t) == ESP_OK) {
        esp_timer_start_once(t, 800 * 1000);
    }
}

__attribute__((weak)) void at_matter_get_info(at_matter_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->transport = "";
}

static void restart_cb(void *arg)
{
    esp_restart();
}

esp_err_t at_settings_set_mode_and_restart(uint8_t mode)
{
    at_settings_t cfg;
    at_settings_get(&cfg);
    cfg.mode = mode;
    esp_err_t err = at_settings_set(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    static esp_timer_handle_t t;
    esp_timer_create_args_t a = {.callback = restart_cb, .name = "mode_restart"};
    if (esp_timer_create(&a, &t) == ESP_OK) {
        esp_timer_start_once(t, 800 * 1000); /* let the HTTP reply go out first */
    }
    return ESP_OK;
}
