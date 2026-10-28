#include "at_autoupdate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "at_app.h"
#include "at_cloudlink.h"
#include "at_fwupdate.h"
#include "at_net.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "autoupdate";

#define RELEASE_URL "https://api.github.com/repos/Tillerwell/tillerwell-hearth-at5/releases/latest"
#define RELEASE_MAX 20480           // bytes of release JSON we are willing to hold; more and it is treated as unusable
#define CHECK_EVERY_S (6 * 3600)
#define RETRY_FAILED_S (30 * 60)    // GitHub or the network let us down
#define WAIT_IDLE_S 60              // an update is known but the heater or a timer is in the way
#define FIRST_CHECK_S 300           // after boot: Wi-Fi, network time and the cloud link settle first
#define TLS_HEADROOM 45000          // largest free block wanted for a second TLS session

static int64_t now_s(void) { return esp_timer_get_time() / 1000000; }

// Fetches the latest release JSON into a malloc'd, NUL-terminated buffer; NULL on any failure.
static char *fetch_release(void)
{
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < TLS_HEADROOM + RELEASE_MAX) {
        ESP_LOGW(TAG, "not enough free memory to check for updates");
        return NULL;
    }
    char *buf = malloc(RELEASE_MAX + 1);
    if (!buf) return NULL;
    esp_http_client_config_t hc = {
        .url = RELEASE_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) { free(buf); return NULL; }
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
    esp_http_client_set_header(c, "User-Agent", "Tillerwell-Hearth");
    int len = 0;
    bool ok = esp_http_client_open(c, 0) == ESP_OK;
    if (ok) {
        esp_http_client_fetch_headers(c);
        ok = esp_http_client_get_status_code(c) == 200;
        for (int r; ok && (r = esp_http_client_read(c, buf + len, RELEASE_MAX - len)) > 0;) {
            len += r;
            if (len >= RELEASE_MAX) ok = false;   // truncated JSON would not parse anyway
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!ok || len <= 0) { free(buf); return NULL; }
    buf[len] = 0;
    return buf;
}

static bool idle_now(void)
{
    at_view_t v;
    at_app_view(&v);
    at_timer_status_t t;
    at_app_timer_status(&t);
    // "Off" means the heater is seen, idle, and nothing of ours is running it.
    bool off = v.snap.have_status && !v.snap.stale && v.snap.status.state_raw == 0x00 &&
               v.snap.run_mode == AT_RUN_NONE && !v.snap.hearth_run;
    return at_autoupdate_idle(t.clock_ok && at_app_clock_synced(), off, t.active, t.have_next, t.next_start_epoch,
                              (int64_t)time(NULL));
}

// Looks for a newer release with the cloud link held, so the check has memory for its own TLS session.
// 1 = found (cmd filled), 0 = up to date, -1 = could not tell.
static int check(char *cmd, size_t n)
{
    at_cloudlink_hold(true);
    for (int i = 0; i < 40 && !at_cloudlink_held(); i++) vTaskDelay(pdMS_TO_TICKS(250));
    char *json = fetch_release();
    at_cloudlink_hold(false);
    if (!json) return -1;
    int r = at_autoupdate_pick(json, esp_app_get_description()->version, cmd, n);
    free(json);
    return r;
}

static void task(void *arg)
{
    char cmd[AT_FWUPDATE_URL_MAX + 160];
    bool have = false;
    int64_t next_check = now_s() + FIRST_CHECK_S;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (!at_settings_auto_update()) { have = false; continue; }
        if (at_fwupdate_busy() || !at_net_connected() || !at_app_clock_synced()) continue;

        if (!have && now_s() >= next_check) {
            int r = check(cmd, sizeof(cmd));
            have = r == 1;
            next_check = now_s() + (r < 0 ? RETRY_FAILED_S : CHECK_EVERY_S);
            ESP_LOGI(TAG, r == 1 ? "a newer firmware is available" : r == 0 ? "firmware is up to date" : "update check failed");
        }
        if (!have) continue;
        if (!idle_now()) { next_check = now_s() + WAIT_IDLE_S; continue; }

        // Free the cloud link's TLS session first, then look once more: that wait takes a few seconds.
        at_cloudlink_hold(true);
        for (int i = 0; i < 40 && !at_cloudlink_held(); i++) vTaskDelay(pdMS_TO_TICKS(250));
        const char *no = idle_now() ? at_fwupdate_request(cmd) : "not_idle";
        if (no) {
            ESP_LOGW(TAG, "not installing now: %s", no);
            at_cloudlink_hold(false);
            if (strcmp(no, "not_idle") == 0) next_check = now_s() + WAIT_IDLE_S;
            else { have = false; next_check = now_s() + CHECK_EVERY_S; }
            continue;
        }
        have = false;
        ESP_LOGW(TAG, "heater is off and no timer is due: installing the update");
        if (!at_fwupdate_start()) next_check = now_s() + RETRY_FAILED_S;
        at_cloudlink_hold(false);   // at_fwupdate_busy keeps the link down while the download runs
    }
}

void at_autoupdate_start(void)
{
    xTaskCreate(task, "autoupdate", 6144, NULL, 2, NULL);
}
