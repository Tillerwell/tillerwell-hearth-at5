#include "at_web.h"

#include <stdlib.h>
#include <string.h>

#include "at_app.h"
#include "at_version.h"
#include "at_mqtt.h"
#include "at_net.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;

/* ---- helpers --------------------------------------------------------------- */

static bool ct_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char d = (unsigned char)(la ^ lb);
    for (size_t i = 0; i < la && i < lb; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* HTTP Basic, user "admin", only if a web password is set. */
static bool authorised(httpd_req_t *req)
{
    at_settings_t cfg;
    at_settings_get(&cfg);
    if (!cfg.web_password[0]) {
        return true;
    }
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK && strncmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[120];
        size_t olen = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &olen, (unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[olen] = 0;
            char want[64];
            snprintf(want, sizeof(want), "admin:%s", cfg.web_password);
            if (ct_equal((char *)dec, want)) {
                return true;
            }
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Autoterm\"");
    httpd_resp_send(req, "Password required", HTTPD_RESP_USE_STRLEN);
    return false;
}

#define REQUIRE_AUTH(req) do { if (!authorised(req)) return ESP_OK; } while (0)

static esp_err_t send_json_str(httpd_req_t *req, const char *s)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return httpd_resp_send_500(req);
    esp_err_t e = send_json_str(req, s);
    free(s);
    return e;
}

static esp_err_t send_ok(httpd_req_t *req, bool ok)
{
    return send_json_str(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* Read a small request body into a malloc'd, NUL-terminated buffer. */
static char *read_body(httpd_req_t *req, size_t max)
{
    size_t len = req->content_len;
    if (len == 0 || len > max) return NULL;
    char *b = malloc(len + 1);
    if (!b) return NULL;
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, b + got, len - got);
        if (r <= 0) {
            free(b);
            return NULL;
        }
        got += (size_t)r;
    }
    b[len] = 0;
    return b;
}

static cJSON *read_json(httpd_req_t *req)
{
    char *b = read_body(req, 2048);
    if (!b) return NULL;
    cJSON *j = cJSON_Parse(b);
    free(b);
    return j;
}

static bool jbool(const cJSON *o, const char *k, bool dflt)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(i)) return cJSON_IsTrue(i);
    if (cJSON_IsNumber(i)) return i->valuedouble != 0;
    return dflt;
}

static double jnum(const cJSON *o, const char *k, double dflt)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : dflt;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : NULL;
}

/* ---- handlers -------------------------------------------------------------- */

static esp_err_t h_index(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_status(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *s = at_app_state_json();
    if (!s) return httpd_resp_send_500(req);
    esp_err_t e = send_json_str(req, s);
    free(s);
    return e;
}

static esp_err_t h_info(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    at_settings_t cfg;
    at_settings_get(&cfg);
    cJSON *o = cJSON_CreateObject();
    const esp_app_desc_t *d = esp_app_get_description();
    cJSON_AddStringToObject(o, "version", d->version);
    cJSON_AddStringToObject(o, "idf", d->idf_ver);
    cJSON_AddStringToObject(o, "variant", "all");
    cJSON_AddStringToObject(o, "device_name", cfg.device_name);
    char buf[40];
    at_net_ip_str(buf, sizeof(buf));
    cJSON_AddStringToObject(o, "ip", buf);
    at_net_ap_ssid(buf, sizeof(buf));
    cJSON_AddStringToObject(o, "ap_ssid", buf);
    cJSON_AddNumberToObject(o, "rssi", at_net_rssi());
    at_timer_status_t ts;
    at_app_timer_status(&ts);
    cJSON_AddStringToObject(o, "timezone", ts.timezone);
    cJSON_AddStringToObject(o, "wifi_ssid", cfg.wifi_ssid);
    /* the Wi-Fi setup card applies when this boot manages Wi-Fi itself */
    bool own_wifi = false;
    own_wifi = cfg.mode != AT_MODE_MATTER;
    cJSON_AddBoolToObject(o, "can_switch_mode", true);
    cJSON_AddStringToObject(o, "mode", own_wifi ? "wifi" : "matter");
    cJSON_AddBoolToObject(o, "wifi_setup", own_wifi);
    cJSON_AddBoolToObject(o, "web_password_set", cfg.web_password[0] != 0);
    at_matter_info_t mi;
    at_matter_get_info(&mi);
    if (mi.transport && mi.transport[0]) {
        cJSON *mt = cJSON_AddObjectToObject(o, "matter");
        cJSON_AddStringToObject(mt, "transport", mi.transport);
        cJSON_AddStringToObject(mt, "qr", mi.qr);
        cJSON_AddStringToObject(mt, "manual", mi.manual);
        cJSON_AddBoolToObject(mt, "commissioned", mi.commissioned);
    }
    return send_json(req, o);
}

static esp_err_t h_cmd(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *j = read_json(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    const char *c = jstr(j, "cmd");
    bool ok = true;
    if (!c) {
        ok = false;
    } else if (!strcmp(c, "start_preheat")) {
        ok = at_app_start_preheat((int)jnum(j, "minutes", 0));
    } else if (!strcmp(c, "preheat_duration")) {
        ok = at_app_set_preheat_duration((int)jnum(j, "minutes", 0));
    } else if (!strcmp(c, "start_thermostat")) {
        ok = at_app_start_thermostat();
    } else if (!strcmp(c, "stop")) {
        ok = at_app_stop();
    } else if (!strcmp(c, "start_pump")) {
        ok = at_app_start_pump();
    } else if (!strcmp(c, "send_pubr0")) {
        ok = at_app_send_pubr0();
    } else if (!strcmp(c, "send_pubr0_variant")) {
        ok = at_app_send_pubr0_variant((int)jnum(j, "variant", 0));
    } else if (!strcmp(c, "auto")) {
        at_app_set_auto(jbool(j, "enabled", false));
    } else if (!strcmp(c, "auto_target")) {
        at_app_set_auto_target((float)jnum(j, "value", 20));
    } else if (!strcmp(c, "auto_range")) {
        at_app_set_auto_range((float)jnum(j, "low", 19), (float)jnum(j, "high", 21));
    } else if (!strcmp(c, "hysteresis")) {
        at_app_set_hysteresis((float)jnum(j, "min", 0), (float)jnum(j, "max", 0));
    } else if (!strcmp(c, "preheat_temp")) {
        at_app_set_preheat_temp((float)jnum(j, "value", 88));
    } else if (!strcmp(c, "factory_reset")) {
        at_settings_factory_reset_soon(); /* wipes Wi-Fi and settings, restarts into setup mode */
    } else if (!strcmp(c, "freeze")) {
        at_app_set_freeze(jbool(j, "enabled", false));
    } else if (!strcmp(c, "freeze_target")) {
        at_app_set_freeze_target((float)jnum(j, "value", 5));
    } else if (!strcmp(c, "fuel_price")) {
        ok = at_app_fuel_set_price((float)jnum(j, "value", -1));
    } else if (!strcmp(c, "fuel_factor")) {
        ok = at_app_fuel_set_factor((float)jnum(j, "value", -1));
    } else if (!strcmp(c, "fuel_rates")) {
        ok = at_app_fuel_set_rates((float)jnum(j, "starting", -1), (float)jnum(j, "low", -1),
                                   (float)jnum(j, "middle", -1), (float)jnum(j, "high", -1));
    } else if (!strcmp(c, "pump_count_reset")) {
        ok = at_app_fuel_pump_reset_count();
    } else if (!strcmp(c, "pump_calibrate")) {
        ok = at_app_fuel_pump_calibrate((float)jnum(j, "litres", -1));
    } else if (!strcmp(c, "ml_per_pulse")) {
        ok = at_app_fuel_set_ml_per_pulse((float)jnum(j, "value", -1));
    } else if (!strcmp(c, "tank_capacity")) {
        ok = at_app_fuel_set_tank_capacity((float)jnum(j, "value", -1));
    } else if (!strcmp(c, "tank_level")) {
        ok = at_app_fuel_set_tank_level((float)jnum(j, "value", -1));
    } else if (!strcmp(c, "tank_filled")) {
        ok = at_app_fuel_tank_filled();
    } else if (!strcmp(c, "preheat_minutes")) {
        at_app_set_preheat_minutes((int)jnum(j, "value", 30));
    } else if (!strcmp(c, "bypass")) {
        at_app_set_bypass(jbool(j, "enabled", false));
    } else if (!strcmp(c, "debug")) {
        at_app_set_debug(jbool(j, "enabled", false), (int)jnum(j, "interval", 0));
    } else if (!strcmp(c, "use_external_temp")) {
        at_app_set_use_external_temp(jbool(j, "enabled", true));
    } else if (!strcmp(c, "external_temp")) {
        at_app_set_external_temp((float)jnum(j, "value", 0));
    } else if (!strcmp(c, "timezone")) {
        const char *z = jstr(j, "value");
        ok = z && at_app_set_timezone(z);
    } else if (!strcmp(c, "set_time")) {
        ok = at_app_set_time((int64_t)jnum(j, "epoch", 0));
    } else {
        ok = false;
    }
    cJSON_Delete(j);
    return send_ok(req, ok);
}

static void copy_str(char *dst, size_t n, const char *src)
{
    strlcpy(dst, src, n);
}

/* ---- timers (contract: docs/TIMERS.md, rules in at_core/at_timers.c) ----------------- */

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, o);
}

static esp_err_t h_timers_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *s = at_app_timers_json();
    if (!s) return httpd_resp_send_500(req);
    esp_err_t e = send_json_str(req, s);
    free(s);
    return e;
}

static esp_err_t h_fuel_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *s = at_app_fuel_json();
    if (!s) return httpd_resp_send_500(req);
    esp_err_t e = send_json_str(req, s);
    free(s);
    return e;
}

static esp_err_t h_timers_put(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (req->content_len > 10240) return send_error(req, "400 Bad Request", "body too large");
    char *body = read_body(req, 10240);
    if (!body) return send_error(req, "400 Bad Request", "body must be a JSON array of timers");
    char err[160];
    int code = at_app_timers_put_json(body, err, sizeof(err));
    free(body);
    if (code == 200) return send_ok(req, true);
    return send_error(req, code == 400 ? "400 Bad Request" : "500 Internal Server Error", err);
}

static esp_err_t h_settings(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *j = read_json(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    at_settings_t cfg;
    at_settings_get(&cfg);
    const char *s;
    if ((s = jstr(j, "device_name")) && s[0] && strlen(s) < sizeof(cfg.device_name)) {
        /* hostname characters only */
        bool okname = true;
        for (const char *p = s; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-')) okname = false;
        }
        if (okname) copy_str(cfg.device_name, sizeof(cfg.device_name), s);
    }
    if ((s = jstr(j, "web_password")) && strlen(s) < sizeof(cfg.web_password)) copy_str(cfg.web_password, sizeof(cfg.web_password), s);
    cJSON_Delete(j);
    esp_err_t err = at_settings_set(&cfg);
    if (err == ESP_OK) at_mqtt_reload();
        return send_ok(req, err == ESP_OK);
}

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *buf = malloc(2048);
    if (!buf) return httpd_resp_send_500(req);
    at_net_scan_json(buf, 2048);
    esp_err_t e = send_json_str(req, buf);
    free(buf);
    return e;
}

static void restart_later(void *arg)
{
    esp_restart();
}

static void schedule_restart(int ms)
{
    esp_timer_handle_t t;
    esp_timer_create_args_t a = {.callback = restart_later, .name = "restart"};
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, (int64_t)ms * 1000);
}

static esp_err_t h_wifi_set(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *j = read_json(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    const char *ssid = jstr(j, "ssid");
    const char *pass = jstr(j, "pass");
    bool ok = ssid && ssid[0] && strlen(ssid) <= 32 && (!pass || strlen(pass) <= 64) &&
              at_net_set_credentials(ssid, pass ? pass : "") == ESP_OK;
    cJSON_Delete(j);
    return send_ok(req, ok);
}

static esp_err_t h_mode(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *j = read_json(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    const char *m = jstr(j, "mode");
    bool ok = false;
    if (m && (!strcmp(m, "wifi") || !strcmp(m, "matter"))) {
        ok = at_settings_set_mode_and_restart(!strcmp(m, "matter") ? AT_MODE_MATTER : AT_MODE_WIFI) == ESP_OK;
    }
    cJSON_Delete(j);
    return send_ok(req, ok);
}

static esp_err_t h_log(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    at_log_entry_t *e = malloc(sizeof(*e) * AT_LOG_ENTRIES);
    if (!e) return httpd_resp_send_500(req);
    int n = at_app_log_copy(e, AT_LOG_ENTRIES);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char line[AT_LOG_BYTES * 3 + 48];
    for (int i = n - 1; i >= 0; i--) { /* newest first */
        int p = snprintf(line, sizeof(line), "%8lu.%03lu %c%s ", (unsigned long)(e[i].ms / 1000),
                         (unsigned long)(e[i].ms % 1000), e[i].dir, e[i].dir == 'S' || e[i].crc_ok ? " " : "!");
        for (int b = 0; b < e[i].len; b++) p += snprintf(line + p, sizeof(line) - p, "%02x ", e[i].data[b]);
        if (e[i].full_len > e[i].len) p += snprintf(line + p, sizeof(line) - p, "... (%u bytes)", e[i].full_len);
        line[p++] = '\n';
        httpd_resp_send_chunk(req, line, p);
    }
    free(e);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    schedule_restart(500);
    return send_ok(req, true);
}

static esp_err_t h_factory(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_ok(req, true);
    vTaskDelay(pdMS_TO_TICKS(300));
    at_settings_factory_reset();
    return ESP_OK;
}

/* An app image starts with the image header and the first segment's header; the app description
 * (with the version) comes straight after, so these many bytes are enough to read it. */
#define OTA_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define OTA_DESC_END (OTA_DESC_OFFSET + sizeof(esp_app_desc_t))
#define OTA_CHUNK 2048

static bool query_flag(httpd_req_t *req, const char *key)
{
    char q[64], v[8];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0;
}

/* POST /api/ota[?allow_older=1]. Refuses an older RoamOS version (or one not in RoamOS form) with
 * 409 unless allow_older=1, the page's rollback tick box. The check runs before anything is erased. */
static esp_err_t h_ota(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len < 1024 || req->content_len > part->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    }
    char *buf = malloc(OTA_CHUNK);
    if (!buf) return httpd_resp_send_500(req);

    size_t have = 0;
    while (have < OTA_DESC_END) {
        int r = httpd_req_recv(req, buf + have, OTA_CHUNK - have);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        have += (size_t)r;
    }
    esp_app_desc_t desc;
    memcpy(&desc, buf + OTA_DESC_OFFSET, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        free(buf);
        return send_error(req, "400 Bad Request", "That file isn't a Hearth firmware image.");
    }
    char incoming[sizeof(desc.version) + 1];
    memcpy(incoming, desc.version, sizeof(desc.version));
    incoming[sizeof(desc.version)] = '\0';
    const char *running = esp_app_get_description()->version;
    bool allow_older = query_flag(req, "allow_older");
    char msg[160];
    switch (at_update_check(running, incoming, allow_older)) {
    case AT_UPDATE_OLDER:
        snprintf(msg, sizeof(msg), "RoamOS %s is older than the %s running now. Tick \"Allow an older version\" to roll back.",
                 incoming, running);
        free(buf);
        return send_error(req, "409 Conflict", msg);
    case AT_UPDATE_NOT_ROAMOS:
        snprintf(msg, sizeof(msg), "Version \"%s\" isn't a RoamOS version, so it can't be checked. Tick \"Allow an older version\" to install it anyway.",
                 incoming);
        free(buf);
        return send_error(req, "409 Conflict", msg);
    case AT_UPDATE_OK:
        break;
    }
    if (at_update_check(running, incoming, false) != AT_UPDATE_OK) {
        ESP_LOGW(TAG, "rollback allowed: %s replaces %s", incoming, running);
    }

    esp_ota_handle_t h;
    if (esp_ota_begin(part, req->content_len, &h) != ESP_OK) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
    }
    bool ok = esp_ota_write(h, buf, have) == ESP_OK;
    size_t left = req->content_len - have;
    while (ok && left) {
        int r = httpd_req_recv(req, buf, left < OTA_CHUNK ? left : OTA_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || esp_ota_write(h, buf, r) != ESP_OK) {
            ok = false;
            break;
        }
        left -= (size_t)r;
    }
    free(buf);
    if (!ok || esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        if (!ok) esp_ota_abort(h);
        ESP_LOGE(TAG, "OTA failed");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "firmware rejected");
    }
    ESP_LOGW(TAG, "OTA written to %s (RoamOS %s), restarting", part->label, incoming);
    send_ok(req, true);
    schedule_restart(1000);
    return ESP_OK;
}

/* Captive portal: while the setup AP is up, send unknown URLs to the UI. */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, "", 0);
}

esp_err_t at_web_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 20;
    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));

    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/info", .method = HTTP_GET, .handler = h_info},
        {.uri = "/api/cmd", .method = HTTP_POST, .handler = h_cmd},
        {.uri = "/api/timers", .method = HTTP_GET, .handler = h_timers_get},
        {.uri = "/api/timers", .method = HTTP_PUT, .handler = h_timers_put},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = h_settings},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = h_wifi_scan},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi_set},
        {.uri = "/api/log", .method = HTTP_GET, .handler = h_log},
        {.uri = "/api/fuel", .method = HTTP_GET, .handler = h_fuel_get},
        {.uri = "/api/mode", .method = HTTP_POST, .handler = h_mode},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/api/factory_reset", .method = HTTP_POST, .handler = h_factory},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = h_ota},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, h_404);
    ESP_LOGI(TAG, "web UI up");
    return ESP_OK;
}
