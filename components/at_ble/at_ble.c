#include "at_ble.h"

#include <stdio.h>
#include <string.h>

#include "at_app.h"
#include "at_led.h"
#include "at_net.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

#define OPEN_AFTER_BOOT_US   (10 * 60 * 1000000LL)
#define OPEN_AFTER_WRITE_US  (3 * 60 * 1000000LL)
#define FAILS_BEFORE_REPORT  3
#define REOPEN_OFFLINE_US    (30 * 1000000LL) /* after the window: Wi-Fi down this long brings Bluetooth back */
#define RETRY_INIT_US        (30 * 1000000LL)

/* 6e2f00NN-5d1c-4f3a-9b7e-2a4c8d1e0f01 (little-endian bytes) */
#define UUID(n) BLE_UUID128_INIT(0x01, 0x0f, 0x1e, 0x8d, 0x4c, 0x2a, 0x7e, 0x9b, 0x3a, 0x4f, 0x1c, 0x5d, (n), 0x00, 0x2f, 0x6e)
static const ble_uuid128_t svc_uuid = UUID(0x01);
static const ble_uuid128_t info_uuid = UUID(0x02);
static const ble_uuid128_t scan_uuid = UUID(0x03);
static const ble_uuid128_t prov_uuid = UUID(0x04);
static const ble_uuid128_t status_uuid = UUID(0x05);
static const ble_uuid128_t mode_uuid = UUID(0x06);

static uint16_t s_status_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static char s_ble_name[24];
static volatile bool s_running; /* NimBLE is initialised (it is shut down while setup is closed, to free its memory) */
static volatile bool s_synced;
static bool s_adv_on;
static int64_t s_open_until_us;

/* setup progress */
static bool s_prov_pending;
static uint32_t s_prov_disc_base;
static char s_prov_ssid[33];

/* Wi-Fi scan, run on its own task because it blocks for seconds */
static char s_scan_json[512] = "[]";
static volatile bool s_scanning;
static volatile bool s_scan_done;

static bool provisionable(void)
{
    return !at_net_connected() || esp_timer_get_time() < s_open_until_us;
}

static void extend_window(int64_t us)
{
    int64_t until = esp_timer_get_time() + us;
    if (until > s_open_until_us) s_open_until_us = until;
}

/* ---- status document ------------------------------------------------------ */

static const char *reason_name(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_AUTH_EXPIRE:
        return "auth";
    case WIFI_REASON_NO_AP_FOUND:
        return "not_found";
    default:
        return "other";
    }
}

static char *status_json(void)
{
    cJSON *o = cJSON_CreateObject();
    const char *state = "idle";
    if (s_prov_pending) {
        if (at_net_connected()) state = "connected";
        else if (at_net_disconnect_count() - s_prov_disc_base >= FAILS_BEFORE_REPORT) state = "failed";
        else state = "connecting";
    } else if (at_net_connected()) {
        state = "connected";
    }
    cJSON_AddStringToObject(o, "state", state);
    char ip[16];
    at_net_ip_str(ip, sizeof(ip));
    cJSON_AddStringToObject(o, "ip", ip);
    if (!strcmp(state, "failed")) cJSON_AddStringToObject(o, "reason", reason_name(at_net_last_disconnect_reason()));
    cJSON_AddStringToObject(o, "scan", s_scanning ? "busy" : (s_scan_done ? "done" : "idle"));
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static int append_str(struct ble_gatt_access_ctxt *ctxt, const char *s)
{
    return os_mbuf_append(ctxt->om, s, strlen(s)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ---- GATT ---------------------------------------------------------------- */

static void scan_task(void *arg)
{
    char buf[sizeof(s_scan_json)];
    at_net_scan_json(buf, sizeof(buf));
    memcpy(s_scan_json, buf, sizeof(buf));
    s_scan_done = true;
    s_scanning = false; /* monitor_task notifies the phone: the stack may be on its way down by now */
    vTaskDelete(NULL);
}

static int info_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    at_settings_t cfg;
    at_settings_get(&cfg);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "device_name", cfg.device_name);
    cJSON_AddBoolToObject(o, "configured", cfg.wifi_ssid[0] != 0);
    cJSON_AddBoolToObject(o, "open", provisionable());
    char ip[16];
    at_net_ip_str(ip, sizeof(ip));
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddStringToObject(o, "wifi_ssid", at_net_connected() ? cfg.wifi_ssid : "");
    cJSON_AddNumberToObject(o, "proto", 1);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return BLE_ATT_ERR_INSUFFICIENT_RES;
    int rc = append_str(ctxt, s);
    free(s);
    return rc;
}

static int scan_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return append_str(ctxt, s_scan_json);
    }
    if (!provisionable()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!s_scanning) {
        s_scanning = true;
        s_scan_done = false;
        if (xTaskCreate(scan_task, "ble_scan", 4096, NULL, 4, NULL) != pdPASS) s_scanning = false;
    }
    return 0;
}

static int prov_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (!provisionable()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    char buf[200];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len >= sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &got) != 0) return BLE_ATT_ERR_UNLIKELY;
    buf[got] = 0;

    cJSON *o = cJSON_Parse(buf);
    const cJSON *ssid = o ? cJSON_GetObjectItem(o, "ssid") : NULL;
    const cJSON *pass = o ? cJSON_GetObjectItem(o, "pass") : NULL;
    int rc = BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    if (cJSON_IsString(ssid) && ssid->valuestring[0] && strlen(ssid->valuestring) <= 32
        && (!pass || (cJSON_IsString(pass) && strlen(pass->valuestring) <= 64))) {
        const char *pw = pass ? pass->valuestring : "";
        s_prov_disc_base = at_net_disconnect_count();
        s_prov_pending = true;
        strlcpy(s_prov_ssid, ssid->valuestring, sizeof(s_prov_ssid));
        extend_window(OPEN_AFTER_WRITE_US);
        ESP_LOGI(TAG, "joining \"%s\" (from phone)", s_prov_ssid);
        rc = at_net_set_credentials(s_prov_ssid, pw) == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
        if (rc != 0) s_prov_pending = false;
    }
    cJSON_Delete(o);
    ble_gatts_chr_updated(s_status_handle);
    return rc;
}

/* Write "matter": the user picked Matter in the app, so save Matter mode and restart into it.
 * Matter mode does not run this service, so the link drops a moment after the write is acknowledged. */
static int mode_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (!provisionable()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    char buf[16];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om), got = 0;
    if (len == 0 || len >= sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &got) != 0) return BLE_ATT_ERR_UNLIKELY;
    buf[got] = 0;
    if (strcmp(buf, "matter") != 0) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    ESP_LOGI(TAG, "switching to Matter mode (from phone)");
    return at_settings_set_mode_and_restart(AT_MODE_MATTER) == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
}

static int status_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char *s = status_json();
    if (!s) return BLE_ATT_ERR_INSUFFICIENT_RES;
    int rc = append_str(ctxt, s);
    free(s);
    return rc;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &info_uuid.u, .access_cb = info_access, .flags = BLE_GATT_CHR_F_READ},
            {.uuid = &scan_uuid.u, .access_cb = scan_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE},
            {.uuid = &prov_uuid.u, .access_cb = prov_access, .flags = BLE_GATT_CHR_F_WRITE},
            {.uuid = &status_uuid.u, .access_cb = status_access, .val_handle = &s_status_handle,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY},
            {.uuid = &mode_uuid.u, .access_cb = mode_access, .flags = BLE_GATT_CHR_F_WRITE},
            {0},
        },
    },
    {0},
};

/* ---- advertising --------------------------------------------------------- */

static int gap_event(struct ble_gap_event *ev, void *arg);

static void adv_start(void)
{
    if (!s_synced || s_adv_on || !provisionable()) return;
    at_settings_t cfg;
    at_settings_get(&cfg);

    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) return;

    /* scan response: name + manufacturer data {company 0xFFFF, bit0 = already has a saved Wi-Fi network} */
    uint8_t mfg[3] = {0xFF, 0xFF, (uint8_t)(cfg.wifi_ssid[0] ? 1 : 0)};
    struct ble_hs_adv_fields r = {0};
    r.name = (uint8_t *)s_ble_name;
    r.name_len = strlen(s_ble_name);
    r.name_is_complete = 1;
    r.mfg_data = mfg;
    r.mfg_data_len = sizeof(mfg);
    if (ble_gap_adv_rsp_set_fields(&r) != 0) return;

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
    p.itvl_max = BLE_GAP_ADV_ITVL_MS(150);
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc == 0) {
        s_adv_on = true;
        ESP_LOGI(TAG, "advertising as %s", s_ble_name);
    } else {
        ESP_LOGW(TAG, "advertising failed: %d", rc);
    }
}

static void adv_stop(void)
{
    if (!s_adv_on) return;
    ble_gap_adv_stop();
    s_adv_on = false;
    ESP_LOGI(TAG, "setup closed, advertising stopped");
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_adv_on = false; /* connectable advertising stops on connect */
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            at_led_setup_link(true);
        } else adv_start();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        at_led_setup_link(false);
        adv_start();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv_on = false;
        adv_start();
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no Bluetooth address");
        return;
    }
    s_synced = true;
    adv_start();
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_up(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_ble_name);
    if (ble_gatts_count_cfg(gatt_svcs) != 0 || ble_gatts_add_svcs(gatt_svcs) != 0) {
        nimble_port_deinit();
        return ESP_FAIL;
    }
    s_running = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

/* Setup is closed and nobody is connected: release the controller and host (tens of KB of RAM, and the radio
 * time Wi-Fi shares with Bluetooth on the C6). */
static void ble_down(void)
{
    s_synced = false;
    if (nimble_port_stop() != 0) {
        ESP_LOGW(TAG, "Bluetooth did not stop");
        s_synced = true;
        return;
    }
    nimble_port_deinit();
    s_running = false;
    s_synced = false; /* in case the host synced while it was being stopped */
    s_adv_on = false;
    ESP_LOGI(TAG, "setup closed, Bluetooth off");
}

/* Opens/closes the window, and tells the phone when setup progress changes. */
static void monitor_task(void *arg)
{
    char last[96] = "";
    int64_t offline_since = 0, next_try = 0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (at_net_connected()) offline_since = 0;
        else if (!offline_since) offline_since = now;
        /* in the window: always; after it: once Wi-Fi has been gone for a while, not on every short drop */
        const bool want = now < s_open_until_us || (offline_since && now - offline_since >= REOPEN_OFFLINE_US);
        if (want && !s_running && now >= next_try) {
            esp_err_t err = ble_up();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Bluetooth setup failed to start: %s", esp_err_to_name(err));
                next_try = now + RETRY_INIT_US;
            }
            last[0] = 0;
        } else if (!want && s_running && s_conn == BLE_HS_CONN_HANDLE_NONE && !s_scanning) {
            ble_down();
        }
        if (s_synced) {
            if (provisionable()) adv_start();
            else if (s_conn == BLE_HS_CONN_HANDLE_NONE) adv_stop();
            char *s = status_json();
            if (s) {
                if (strcmp(s, last) != 0) {
                    strlcpy(last, s, sizeof(last));
                    ble_gatts_chr_updated(s_status_handle);
                }
                free(s);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t at_ble_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ble_name, sizeof(s_ble_name), "Hearth-%02x%02x%02x", mac[3], mac[4], mac[5]);
    s_open_until_us = esp_timer_get_time() + OPEN_AFTER_BOOT_US;

    esp_err_t err = ble_up();
    if (err != ESP_OK) return err;
    return xTaskCreate(monitor_task, "ble_mon", 4096, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
