#include "at_web.h"

#include <stdlib.h>
#include <string.h>

#include "at_access.h"
#include "at_cloud.h"
#include "at_cloudlink.h"
#include "at_app.h"
#include "at_auth.h"
#include "at_version.h"
#include "at_mqtt.h"
#include "at_net.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
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

/* The old password, for the old API (docs/API_SPEC.md §3): HTTP Basic, user "admin", only if a web password is set. */
static bool legacy_authorised(httpd_req_t *req)
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

/* ---- signed requests (docs/API_SPEC.md §5, §6) ------------------------------------------------------------
 * The signed API rides on this same server (the spec's plain-HTTP fallback until the TLS heap test is done on
 * the bench): a request carrying a Hearth-Auth header is checked against the user table and the route's
 * permission level. A request without one is the old API, which stays until an admin switches it off.
 * esp_http_server runs one request at a time, so one set of statics is enough. */

#define BODY_MAX 10240
#define OTA_PATH "/api/ota"

static struct {
    bool signed_req;     /* this request was signed by a user in the table */
    bool pending;        /* signature not yet checked: the body is too big to hold (OTA), the handler finishes it */
    at_user_t user;
    at_auth_header_t hdr;
    char method[8];
    char path[64];
} s_who;
static char *s_body;     /* a signed request's body, already read and hashed by the gate */
static size_t s_body_len;
static char s_next_nonce[AT_NONCE_LEN + 1];

static void drop_stash(void)
{
    free(s_body);
    s_body = NULL;
    s_body_len = 0;
}

static esp_err_t send_auth_error(httpd_req_t *req, const char *status, const char *code, const char *need)
{
    char b[96];
    if (need) snprintf(b, sizeof(b), "{\"error\":\"%s\",\"need\":\"%s\"}", code, need);
    else snprintf(b, sizeof(b), "{\"error\":\"%s\"}", code);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

/* Read the whole request body (NUL-terminated, malloc'd) or NULL. */
static char *recv_all(httpd_req_t *req, size_t max, size_t *out_len)
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
    if (out_len) *out_len = len;
    return b;
}

static void issue_nonce(httpd_req_t *req)
{
    at_access_nonce_new(s_next_nonce);
    httpd_resp_set_hdr(req, "Hearth-Nonce", s_next_nonce);
}

static bool signed_header(httpd_req_t *req, char *hdr, size_t n, bool *present)
{
    esp_err_t e = httpd_req_get_hdr_value_str(req, "Hearth-Auth", hdr, n);
    *present = e != ESP_ERR_NOT_FOUND;
    return e == ESP_OK;
}

/* The command a signed POST /api/cmd asks for needs its own level; the route alone is only control. */
static at_perm_t cmd_perm_of_body(const char *body) { return at_auth_cmd_json_perm(body); }

/* Every protected route starts here. True: carry on (the reply is the handler's). False: the reply has gone. */
static bool gate(httpd_req_t *req)
{
    drop_stash();
    memset(&s_who, 0, sizeof(s_who));
    char hv[256];
    bool present;
    bool ok = signed_header(req, hv, sizeof(hv), &present);
    if (!present) {
        if (at_access_legacy_enabled()) return legacy_authorised(req);
        send_auth_error(req, "401 Unauthorized", "auth_required", NULL);
        return false;
    }
    if (!ok) {
        send_auth_error(req, "401 Unauthorized", "auth_required", NULL);
        return false;
    }
    const char *method = http_method_str(req->method);
    at_perm_t need = at_auth_route_perm(method, req->uri);
    at_auth_header_t h;
    const char *err = at_access_open(hv, &h);
    if (err) {
        send_auth_error(req, "401 Unauthorized", err, NULL);
        return false;
    }
    at_user_t u;
    if (!at_access_user_find(h.kid, &u)) {
        send_auth_error(req, "401 Unauthorized", "unknown_key", NULL);
        return false;
    }
    char hex[65];
    bool is_ota = req->method == HTTP_POST && strncmp(req->uri, OTA_PATH, strlen(OTA_PATH)) == 0 &&
                  (req->uri[strlen(OTA_PATH)] == 0 || req->uri[strlen(OTA_PATH)] == '?');
    if (is_ota) {
        if (strlen(req->uri) >= sizeof(s_who.path)) {
            send_auth_error(req, "401 Unauthorized", "bad_signature", NULL);
            return false;
        }
        if (u.perm < AT_PERM_ADMIN) {
            send_auth_error(req, "403 Forbidden", "forbidden", "admin");
            return false;
        }
        s_who.pending = true;   /* h_ota hashes the image as it arrives and checks before it boots it */
        strlcpy(s_who.method, method, sizeof(s_who.method));
        strlcpy(s_who.path, req->uri, sizeof(s_who.path)); /* length checked above */
        s_who.hdr = h;
    } else {
        if (req->content_len > BODY_MAX) {
            send_auth_error(req, "413 Payload Too Large", "too_large", NULL);
            return false;
        }
        if (req->content_len > 0) {
            s_body = recv_all(req, BODY_MAX, &s_body_len);
            if (!s_body) {
                send_auth_error(req, "400 Bad Request", "bad_body", NULL);
                return false;
            }
        }
        at_auth_sha256_hex((const uint8_t *)(s_body ? s_body : ""), s_body_len, hex);
        err = at_access_verify(&h, u.pub, method, req->uri, hex);
        if (err) {
            drop_stash();
            send_auth_error(req, "401 Unauthorized", err, NULL);
            return false;
        }
        if (req->method == HTTP_POST && !strcmp(req->uri, "/api/cmd")) need = cmd_perm_of_body(s_body);
        if (u.perm < need) {
            drop_stash();
            send_auth_error(req, "403 Forbidden", "forbidden", at_perm_name(need));
            return false;
        }
    }
    s_who.signed_req = true;
    s_who.user = u;
    issue_nonce(req);
    return true;
}

/* For the /api/auth/ routes that only make sense for a signed-in user. */
static bool gate_signed(httpd_req_t *req)
{
    if (!gate(req)) return false;
    if (!s_who.signed_req) {
        drop_stash();
        send_auth_error(req, "401 Unauthorized", "auth_required", NULL);
        return false;
    }
    return true;
}

#define REQUIRE_AUTH(req) do { if (!gate(req)) return ESP_OK; } while (0)
#define REQUIRE_SIGNED(req) do { if (!gate_signed(req)) return ESP_OK; } while (0)

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
    if (s_body) { /* a signed request: the gate has read it */
        char *b = s_body;
        size_t n = s_body_len;
        s_body = NULL;
        s_body_len = 0;
        if (n == 0 || n > max) {
            free(b);
            return NULL;
        }
        return b;
    }
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
    /* the page itself holds no data; with the old API switched off it is served freely and its calls fail */
    if (at_access_legacy_enabled() && !legacy_authorised(req)) return ESP_OK;
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
    at_settings_display_name(buf, sizeof(buf));
    cJSON_AddStringToObject(o, "display_name", buf);
    at_settings_hostname(buf, sizeof(buf));
    cJSON_AddStringToObject(o, "hostname", buf);
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
    /* bytes: free now, lowest since boot, largest single block (falling over days = a leak or fragmentation) */
    cJSON *hp = cJSON_AddObjectToObject(o, "heap");
    cJSON_AddNumberToObject(hp, "free", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(hp, "min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(hp, "largest", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    /* the cloud link (docs/API_SPEC.md §9.5): state, and the heap seen while a TLS session was up */
    at_cloudlink_status_t cl;
    at_cloudlink_status(&cl);
    cJSON *cj = cJSON_AddObjectToObject(o, "cloud");
    cJSON_AddStringToObject(cj, "state", cl.state);
    cJSON_AddNumberToObject(cj, "endpoint", cl.endpoint);
    cJSON_AddNumberToObject(cj, "failures", cl.failures);
    cJSON_AddNumberToObject(cj, "connects", cl.connects);
    cJSON_AddNumberToObject(cj, "commands_ok", cl.commands_ok);
    cJSON_AddNumberToObject(cj, "commands_refused", cl.commands_refused);
    cJSON_AddNumberToObject(cj, "heap_at_connect", cl.heap_at_connect);
    cJSON_AddNumberToObject(cj, "heap_min", cl.heap_min_online);
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

static void schedule_restart(int ms);

/* The app's name for the box becomes its network hostname ("Autoterm 5D" -> Autoterm-5D.local, see
 * at_settings_hostname); "" goes back to hearth-xxxxxx. The hostname is applied at startup (DHCP, mDNS, Matter's
 * delegated host), so an actual change restarts the box. */
static bool set_display_name(const char *name)
{
    char cur[AT_DISPLAY_NAME_MAX];
    at_settings_display_name(cur, sizeof(cur));
    if (strlen(name) >= sizeof(cur)) return false;
    if (!strcmp(cur, name)) return true;
    if (at_settings_display_name_set(name) != ESP_OK) return false;
    schedule_restart(800); /* let the HTTP reply go out first */
    return true;
}

/* The safety layer's front door, shared by POST /api/cmd and the cloud link: the caller has already checked who may
 * ask. False means nothing was done (unknown command, bad value, or the box said no: bypass on, queue full). */
static bool run_cmd(const cJSON *j)
{
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
    } else if (!strcmp(c, "display_name")) {
        const char *v = jstr(j, "value");
        ok = v && set_display_name(v);
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
    return ok;
}

bool at_web_exec_cmd(const char *json)
{
    cJSON *j = json ? cJSON_Parse(json) : NULL;
    bool ok = j && run_cmd(j);
    cJSON_Delete(j);
    return ok;
}

static esp_err_t h_cmd(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *j = read_json(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    bool ok = run_cmd(j);
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
    bool name_ok = true;
    if ((s = jstr(j, "display_name"))) name_ok = set_display_name(s);
    cJSON_Delete(j);
    esp_err_t err = at_settings_set(&cfg);
    if (err == ESP_OK && !name_ok) err = ESP_FAIL;
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
    /* /api/log is polled every few seconds by the capture tool.  Keeping the
     * snapshot on the HTTP worker's already-reserved stack avoids a transient
     * heap allocation (about 3 KiB) for every poll.  Under a busy update this
     * allocation was the only part of the request that could fail, returning
     * 500 while /api/status still worked and making the device look offline. */
    at_log_entry_t e[AT_LOG_ENTRIES];
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
    mbedtls_sha256_context sha; /* a signed update is hashed as it arrives (the image is too big to hold) */
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    size_t have = 0;
    while (have < OTA_DESC_END) {
        int r = httpd_req_recv(req, buf + have, OTA_CHUNK - have);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            mbedtls_sha256_free(&sha);
            return ESP_FAIL;
        }
        have += (size_t)r;
    }
    mbedtls_sha256_update(&sha, (unsigned char *)buf, have);
    esp_app_desc_t desc;
    memcpy(&desc, buf + OTA_DESC_OFFSET, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        free(buf);
        mbedtls_sha256_free(&sha);
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
        mbedtls_sha256_free(&sha);
        return send_error(req, "409 Conflict", msg);
    case AT_UPDATE_NOT_ROAMOS:
        snprintf(msg, sizeof(msg), "Version \"%s\" isn't a RoamOS version, so it can't be checked. Tick \"Allow an older version\" to install it anyway.",
                 incoming);
        free(buf);
        mbedtls_sha256_free(&sha);
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
        mbedtls_sha256_free(&sha);
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
        mbedtls_sha256_update(&sha, (unsigned char *)buf, (size_t)r);
        left -= (size_t)r;
    }
    free(buf);
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (!ok || esp_ota_end(h) != ESP_OK) {
        if (!ok) esp_ota_abort(h);
        ESP_LOGE(TAG, "OTA failed");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "firmware rejected");
    }
    if (s_who.pending) { /* the signature covers the whole image, so it can only be checked now: before booting it */
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        const char *serr = at_access_verify(&s_who.hdr, s_who.user.pub, s_who.method, s_who.path, hex);
        s_who.pending = false;
        if (serr) {
            ESP_LOGE(TAG, "OTA signature check failed (%s), not booting the new image", serr);
            return send_auth_error(req, "401 Unauthorized", serr, NULL);
        }
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "firmware rejected");
    }
    ESP_LOGW(TAG, "OTA written to %s (RoamOS %s), restarting", part->label, incoming);
    send_ok(req, true);
    schedule_restart(1000);
    return ESP_OK;
}

/* ---- /api/auth/... (docs/API_SPEC.md §5.1, §6, §7) -------------------------------------------------------- */

static esp_err_t h_hello(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    char name[40];
    at_settings_display_name(name, sizeof(name));
    if (!name[0]) at_settings_hostname(name, sizeof(name));
    cJSON_AddStringToObject(o, "device_id", at_access_device_id());
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    cJSON_AddBoolToObject(o, "claimed", at_access_has_admin());
    cJSON_AddStringToObject(o, "fingerprint", ""); /* no TLS yet: the signed API is on plain HTTP for now */
    cJSON_AddStringToObject(o, "transport", "http");
    cJSON_AddBoolToObject(o, "legacy", at_access_legacy_enabled());
    return send_json(req, o);
}

static esp_err_t h_nonce(httpd_req_t *req)
{
    char n[AT_NONCE_LEN + 1];
    at_access_nonce_new(n);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "nonce", n);
    cJSON_AddNumberToObject(o, "ttl", AT_NONCE_TTL_S);
    return send_json(req, o);
}

/* A request that is signed but not by a user in the table yet (claim, redeem): read its body, and check the
 * signature with `pub_from` (which gets the parsed body and fills in the key). 0 = ok, else the status was sent. */
typedef const char *(*key_from_body_t)(cJSON *body, uint8_t pub[AT_AUTH_PUBKEY_LEN]);

static cJSON *open_unregistered(httpd_req_t *req, key_from_body_t pub_from, uint8_t pub[AT_AUTH_PUBKEY_LEN])
{
    char hv[256];
    bool present;
    if (!signed_header(req, hv, sizeof(hv), &present)) {
        send_auth_error(req, "401 Unauthorized", "auth_required", NULL);
        return NULL;
    }
    at_auth_header_t h;
    const char *err = at_access_open(hv, &h);
    if (err) {
        send_auth_error(req, "401 Unauthorized", err, NULL);
        return NULL;
    }
    size_t n = 0;
    char *body = recv_all(req, 2048, &n);
    if (!body) {
        send_auth_error(req, "400 Bad Request", "bad_body", NULL);
        return NULL;
    }
    char hex[65];
    at_auth_sha256_hex((const uint8_t *)body, n, hex);
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j || (err = pub_from(j, pub)) != NULL) {
        cJSON_Delete(j);
        send_auth_error(req, "400 Bad Request", err ? err : "bad_body", NULL);
        return NULL;
    }
    char kid[AT_AUTH_KID_LEN + 1];
    at_auth_key_id(pub, kid);
    if (strcmp(kid, h.kid) != 0 || (err = at_access_verify(&h, pub, http_method_str(req->method), req->uri, hex)) != NULL) {
        cJSON_Delete(j);
        send_auth_error(req, "401 Unauthorized", err ? err : "bad_signature", NULL);
        return NULL;
    }
    return j;
}

static const char *key_from_pubkey_field(cJSON *body, uint8_t pub[AT_AUTH_PUBKEY_LEN])
{
    const char *k = jstr(body, "pubkey");
    if (!k || at_b64url_decode(k, strlen(k), pub, AT_AUTH_PUBKEY_LEN) != AT_AUTH_PUBKEY_LEN || pub[0] != 0x04) return "bad_body";
    return NULL;
}

static esp_err_t h_claim_post(httpd_req_t *req)
{
    uint8_t pub[AT_AUTH_PUBKEY_LEN];
    cJSON *j = open_unregistered(req, key_from_pubkey_field, pub);
    if (!j) return ESP_OK;
    const char *label = jstr(j, "label");
    char lab[AT_LABEL_MAX + 1];
    snprintf(lab, sizeof(lab), "%s", label ? label : "");
    cJSON_Delete(j);
    int code = at_access_claim_start(pub, lab);
    if (code == 409) return send_auth_error(req, "409 Conflict", "already_claimed", NULL);
    if (code != 202) return send_auth_error(req, "500 Internal Server Error", "claim_failed", NULL);
    httpd_resp_set_status(req, "202 Accepted");
    ESP_LOGW(TAG, "claimed by the first phone to ask");
    return send_json_str(req, "{\"state\":\"claimed\"}");
}

static esp_err_t h_claim_get(httpd_req_t *req)
{
    static const char *const names[] = {"idle", "waiting", "claimed", "timeout"};
    char b[40];
    snprintf(b, sizeof(b), "{\"state\":\"%s\"}", names[at_access_claim_state()]);
    return send_json_str(req, b);
}

static const char *key_from_grant(cJSON *body, uint8_t pub[AT_AUTH_PUBKEY_LEN])
{
    const char *tok = jstr(body, "grant");
    at_grant_t g;
    if (!tok || at_auth_grant_peek(tok, &g) != AT_AUTH_OK) return "malformed";
    memcpy(pub, g.key, AT_AUTH_PUBKEY_LEN);
    return NULL;
}

static esp_err_t h_redeem(httpd_req_t *req)
{
    uint8_t pub[AT_AUTH_PUBKEY_LEN];
    cJSON *j = open_unregistered(req, key_from_grant, pub);
    if (!j) return ESP_OK;
    char tok[640];
    const char *t = jstr(j, "grant");
    snprintf(tok, sizeof(tok), "%s", t ? t : "");
    cJSON_Delete(j);
    const char *err = at_access_redeem(tok, pub);
    if (err) {
        const char *st = "400 Bad Request";
        if (!strcmp(err, "unknown_signer") || !strcmp(err, "bad_signature")) st = "401 Unauthorized";
        else if (!strcmp(err, "wrong_device") || !strcmp(err, "wrong_key")) st = "403 Forbidden";
        else if (!strcmp(err, "already_used") || !strcmp(err, "table_full")) st = "409 Conflict";
        else if (!strcmp(err, "expired")) st = "410 Gone";
        return send_auth_error(req, st, err, NULL);
    }
    at_user_t u;
    char kid[AT_AUTH_KID_LEN + 1];
    at_auth_key_id(pub, kid);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "kid", kid);
    if (at_access_user_find(kid, &u)) cJSON_AddStringToObject(o, "perm", at_perm_name((at_perm_t)u.perm));
    return send_json(req, o);
}

static esp_err_t h_me(httpd_req_t *req)
{
    REQUIRE_SIGNED(req);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "kid", s_who.user.kid);
    cJSON_AddStringToObject(o, "label", s_who.user.label);
    cJSON_AddStringToObject(o, "perm", at_perm_name((at_perm_t)s_who.user.perm));
    cJSON_AddNumberToObject(o, "exp", (double)s_who.user.exp);
    return send_json(req, o);
}

static esp_err_t h_users_list(httpd_req_t *req)
{
    REQUIRE_SIGNED(req);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < at_access_user_count(); i++) {
        at_user_t u;
        if (!at_access_user_at(i, &u)) continue;
        char pub[100];
        at_b64url_encode(u.pub, AT_AUTH_PUBKEY_LEN, pub, sizeof(pub));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "kid", u.kid);
        cJSON_AddStringToObject(o, "pub", pub);
        cJSON_AddStringToObject(o, "perm", at_perm_name((at_perm_t)u.perm));
        cJSON_AddStringToObject(o, "label", u.label);
        cJSON_AddNumberToObject(o, "exp", (double)u.exp);
        cJSON_AddStringToObject(o, "by", u.by);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "users", arr);
    return send_json(req, o);
}

static esp_err_t h_user_delete(httpd_req_t *req)
{
    REQUIRE_SIGNED(req);
    const char *kid = req->uri + strlen("/api/auth/users/");
    if (strlen(kid) != AT_AUTH_KID_LEN) return send_auth_error(req, "400 Bad Request", "bad_kid", NULL);
    switch (at_access_user_remove(kid)) {
    case 0: return send_ok(req, true);
    case 1: return send_auth_error(req, "404 Not Found", "unknown_key", NULL);
    default: return send_auth_error(req, "409 Conflict", "last_admin", NULL);
    }
}

static esp_err_t h_grant_revoke(httpd_req_t *req)
{
    REQUIRE_SIGNED(req);
    cJSON *j = read_json(req);
    const char *gid = j ? jstr(j, "gid") : NULL;
    bool ok = gid && gid[0] && strlen(gid) < 24;
    if (ok) at_access_gid_burn(gid);
    cJSON_Delete(j);
    return ok ? send_ok(req, true) : send_auth_error(req, "400 Bad Request", "bad_body", NULL);
}

/* POST /api/auth/legacy {"enabled":false} switches the old unauthenticated API off (§3). Needs an admin to exist. */
static esp_err_t h_legacy(httpd_req_t *req)
{
    REQUIRE_SIGNED(req);
    cJSON *j = read_json(req);
    bool have = j && cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(j, "enabled"));
    bool en = have && jbool(j, "enabled", true);
    cJSON_Delete(j);
    if (!have) return send_auth_error(req, "400 Bad Request", "bad_body", NULL);
    if (at_access_legacy_set(en) != 0) return send_auth_error(req, "409 Conflict", "no_admin", NULL);
    return send_ok(req, true);
}

/* ---- /api/cloud (docs/API_SPEC.md §8), admin only ------------------------------------------------------- */

static esp_err_t h_cloud_csr(httpd_req_t *req)
{
    REQUIRE_SIGNED(req);
    char *pem = malloc(1024);
    if (!pem) return httpd_resp_send_500(req);
    if (at_cloud_csr(at_access_device_id(), pem, 1024) != 0) {
        free(pem);
        return send_auth_error(req, "500 Internal Server Error", "csr_failed", NULL);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "device_id", at_access_device_id());
    cJSON_AddStringToObject(o, "csr_pem", pem);
    free(pem);
    return send_json(req, o);
}

static esp_err_t h_cloud_cert(httpd_req_t *req)
{
    REQUIRE_SIGNED(req);
    char *raw = read_body(req, 4096); /* two PEM certificates, JSON-escaped: more than read_json's 2 KB */
    cJSON *j = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    const char *chain = j ? jstr(j, "cert_chain_pem") : NULL;
    const char *err = chain ? at_cloud_install(chain, at_access_device_id()) : "bad_body";
    cJSON_Delete(j);
    if (!err) return send_ok(req, true);
    const char *status = !strcmp(err, "storage") ? "500 Internal Server Error" : "400 Bad Request";
    return send_auth_error(req, status, err, NULL);
}

static esp_err_t h_cloud_delete(httpd_req_t *req)
{
    REQUIRE_SIGNED(req);
    at_cloud_forget();
    return send_ok(req, true);
}

/* Captive portal: while the setup AP is up, send unknown URLs to the UI. */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    /* Only browsers on the setup hotspot want the redirect; an app asking for an API route that isn't there (an older
     * firmware, say) must get a plain 404 rather than a redirect to an address it can't reach. */
    if (!strncmp(req->uri, "/api/", 5)) return send_auth_error(req, "404 Not Found", "not_found", NULL);
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
    cfg.max_uri_handlers = 32;
    cfg.uri_match_fn = httpd_uri_match_wildcard; /* one route (DELETE users/<kid>) uses a wildcard */
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
        {.uri = "/api/hello", .method = HTTP_GET, .handler = h_hello},
        {.uri = "/api/auth/nonce", .method = HTTP_GET, .handler = h_nonce},
        {.uri = "/api/auth/claim", .method = HTTP_POST, .handler = h_claim_post},
        {.uri = "/api/auth/claim", .method = HTTP_GET, .handler = h_claim_get},
        {.uri = "/api/auth/redeem", .method = HTTP_POST, .handler = h_redeem},
        {.uri = "/api/auth/me", .method = HTTP_GET, .handler = h_me},
        {.uri = "/api/auth/users", .method = HTTP_GET, .handler = h_users_list},
        {.uri = "/api/auth/users/*", .method = HTTP_DELETE, .handler = h_user_delete},
        {.uri = "/api/auth/grant-revoke", .method = HTTP_POST, .handler = h_grant_revoke},
        {.uri = "/api/auth/legacy", .method = HTTP_POST, .handler = h_legacy},
        {.uri = "/api/cloud/csr", .method = HTTP_POST, .handler = h_cloud_csr},
        {.uri = "/api/cloud/cert", .method = HTTP_POST, .handler = h_cloud_cert},
        {.uri = "/api/cloud", .method = HTTP_DELETE, .handler = h_cloud_delete},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, h_404);
    ESP_LOGI(TAG, "web UI up");
    return ESP_OK;
}
