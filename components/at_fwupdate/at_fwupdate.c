#include "at_fwupdate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG = "fwupdate";

#define READ_CHUNK 2048

static at_fwupdate_req_t s_req;
static volatile bool s_pending, s_busy;
static char s_err[16];

const char *at_fwupdate_last_error(void) { return s_err; }
bool at_fwupdate_busy(void) { return s_busy; }

const char *at_fwupdate_request(const char *cmd_json)
{
    if (s_busy || s_pending) return "busy";
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return "too_big";
    at_fwupdate_req_t r;
    const char *why = at_fwupdate_parse(cmd_json, esp_app_get_description()->version, part->size, &r);
    if (why) return why;
    s_req = r;
    s_pending = true;
    return NULL;
}

// SHA-256 of the first `len` bytes of the partition the image was just written to.
static bool partition_sha256(const esp_partition_t *part, size_t len, char hex[65])
{
    uint8_t *buf = malloc(READ_CHUNK);
    if (!buf) return false;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    bool ok = true;
    for (size_t off = 0; ok && off < len;) {
        size_t n = len - off < READ_CHUNK ? len - off : READ_CHUNK;
        ok = esp_partition_read(part, off, buf, n) == ESP_OK;
        if (ok) mbedtls_sha256_update(&ctx, buf, n);
        off += n;
    }
    uint8_t d[32];
    mbedtls_sha256_finish(&ctx, d);
    mbedtls_sha256_free(&ctx);
    free(buf);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    return ok;
}

static void finish_failed(const char *why)
{
    snprintf(s_err, sizeof(s_err), "%s", why);
    ESP_LOGE(TAG, "update failed: %s", why);
    s_busy = false;
    vTaskDelete(NULL);
}

static void update_task(void *arg)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_http_client_config_t hc = {
        .url = s_req.url,
        .crt_bundle_attach = esp_crt_bundle_attach,   // github.com and its CDN, checked against the standard roots
        .timeout_ms = 20000,
        .buffer_size = 2048,                          // the CDN redirect carries a long signed URL
        .buffer_size_tx = 1024,
    };
    esp_https_ota_config_t oc = {.http_config = &hc};
    esp_https_ota_handle_t h = NULL;
    ESP_LOGW(TAG, "downloading RoamOS %s", s_req.version);
    if (!part || esp_https_ota_begin(&oc, &h) != ESP_OK) finish_failed("download_failed");

    esp_app_desc_t desc;
    char got[sizeof(desc.version) + 1] = "";
    if (esp_https_ota_get_img_desc(h, &desc) == ESP_OK) memcpy(got, desc.version, sizeof(desc.version));
    if (strcmp(got, s_req.version) != 0) {
        esp_https_ota_abort(h);
        finish_failed("wrong_version");
    }

    esp_err_t e;
    while ((e = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    }
    if (e != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        finish_failed("download_failed");
    }
    if ((uint32_t)esp_https_ota_get_image_len_read(h) != s_req.size) {
        esp_https_ota_abort(h);
        finish_failed("size_mismatch");
    }
    // The signed command named the hash; the new image only becomes the boot image if it matches.
    char hex[65];
    if (!partition_sha256(part, s_req.size, hex) || strcmp(hex, s_req.sha256) != 0) {
        esp_https_ota_abort(h);
        finish_failed("bad_hash");
    }
    if (esp_https_ota_finish(h) != ESP_OK) finish_failed("bad_image");
    ESP_LOGW(TAG, "RoamOS %s installed, restarting", s_req.version);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool at_fwupdate_start(void)
{
    if (!s_pending || s_busy) return false;
    s_pending = false;
    s_err[0] = 0;
    s_busy = true;
    if (xTaskCreate(update_task, "fwupdate", 10240, NULL, 3, NULL) != pdPASS) {
        snprintf(s_err, sizeof(s_err), "no_memory");
        s_busy = false;
        return false;
    }
    return true;
}
