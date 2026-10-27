#include "at_net.h"

#include <stdio.h>
#include <string.h>

#include "at_app.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "sdkconfig.h"

#include "mdns.h"
#include "lwip/sockets.h"

static const char *TAG = "net";

static at_net_ip_cb_t s_cb;
static bool s_inited;
static volatile bool s_connected;
static volatile bool s_ap_active;
static char s_ip[16];
static char s_ap_ssid[33];
static bool s_own; /* this boot manages Wi-Fi itself (not Matter) */
static bool s_ha_adv;      /* the Home Assistant service is currently announced */
static bool s_mdns_ready;  /* mDNS is up (own Wi-Fi: after at_net_start; Matter: once the delegated host exists) */

#define HA_SERVICE "_hearth-ha" /* Home Assistant's integration browses this */

/* Announce the service Home Assistant discovers the box by (always on). The _hearth-app._tcp service the Tillerwell app
 * uses stays up either way. */
void at_net_ha_advertise(bool on)
{
    if (!s_mdns_ready || on == s_ha_adv) return;
    at_settings_t cfg;
    at_settings_get(&cfg);
    esp_err_t err;
    if (on) {
        err = s_own ? mdns_service_add(NULL, HA_SERVICE, "_tcp", 80, NULL, 0)
                    : mdns_service_add_for_host("Tillerwell Hearth", HA_SERVICE, "_tcp", cfg.device_name, 80, NULL, 0);
    } else {
        err = s_own ? mdns_service_remove(HA_SERVICE, "_tcp")
                    : mdns_service_remove_for_host("Tillerwell Hearth", HA_SERVICE, "_tcp", cfg.device_name);
    }
    if (err == ESP_OK) s_ha_adv = on;
    else ESP_LOGW(TAG, "mDNS Home Assistant service %s: %s", on ? "add" : "remove", esp_err_to_name(err));
}
static volatile uint32_t s_disc_count;
static volatile uint8_t s_disc_reason;

uint32_t at_net_disconnect_count(void) { return s_disc_count; }
uint8_t at_net_last_disconnect_reason(void) { return s_disc_reason; }

esp_err_t at_net_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_inited = true;
    return ESP_OK;
}

bool at_net_connected(void) { return s_connected; }
bool at_net_setup_ap_active(void) { return s_ap_active; }

void at_net_ip_str(char *out, size_t len)
{
    snprintf(out, len, "%s", s_connected ? s_ip : "");
}

void at_net_ap_ssid(char *out, size_t len)
{
    snprintf(out, len, "%s", s_ap_active ? s_ap_ssid : "");
}

int at_net_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

static const char *auth_name(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    default: return "other";
    }
}

esp_err_t at_net_scan_json(char *out, size_t len)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        snprintf(out, len, "[]");
        return err;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);
    size_t pos = 0;
    pos += snprintf(out + pos, len - pos, "[");
    for (int i = 0; i < n && pos + 100 < len; i++) {
        /* skip empty and duplicate names; escape quotes/backslashes */
        if (!recs[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char *)recs[i].ssid, (char *)recs[j].ssid) == 0) dup = true;
        }
        if (dup) continue;
        char esc[70];
        size_t e = 0;
        for (const char *p = (char *)recs[i].ssid; *p && e < sizeof(esc) - 2; p++) {
            if (*p == '"' || *p == '\\') esc[e++] = '\\';
            if ((unsigned char)*p >= 0x20) esc[e++] = *p;
        }
        esc[e] = 0;
        pos += snprintf(out + pos, len - pos, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}",
                        pos > 1 ? "," : "", esc, recs[i].rssi, auth_name(recs[i].authmode));
    }
    snprintf(out + pos, len - pos, "]");
    return ESP_OK;
}

/* Matter mode: Matter owns mDNS and calls mdns_hostname_set() with its own MAC-derived name every time it
 * publishes, so a plain mdns_hostname_set(hearth-xxxxxx) would be overwritten. Publish our name as a
 * delegated host instead (own A record + our services) so hearth-<id>.local and _hearth-app._tcp keep working for
 * the Tillerwell app alongside Matter. Called on every address change. */
static void matter_mdns_publish(const esp_ip4_addr_t *ip)
{
    at_settings_t cfg;
    at_settings_get(&cfg);
    esp_err_t err = mdns_init(); /* idempotent: Matter normally started it already */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }
    mdns_ip_addr_t addr = {.next = NULL};
    addr.addr.type = ESP_IPADDR_TYPE_V4;
    addr.addr.u_addr.ip4 = *ip;
    /* later addresses (DHCP renewal, reconnect) only update the A record; _set_address reports OK even for an
     * unknown host, so remember whether the first call already ran */
    static bool added;
    if (added) {
        mdns_delegate_hostname_set_address(cfg.device_name, &addr);
        return;
    }
    err = mdns_delegate_hostname_add(cfg.device_name, &addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns delegate host: %s", esp_err_to_name(err));
        return;
    }
    added = true;
    mdns_service_add_for_host("Tillerwell Hearth", "_http", "_tcp", cfg.device_name, 80, NULL, 0);
    mdns_service_add_for_host("Tillerwell Hearth", "_hearth-app", "_tcp", cfg.device_name, 80, NULL, 0);
    s_mdns_ready = true;
    at_net_ha_advertise(true);
    ESP_LOGI(TAG, "mDNS: %s.local (Matter mode)", cfg.device_name);
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "got IP %s", s_ip);
        if (!s_own) matter_mdns_publish(&e->ip_info.ip);
        if (s_cb) s_cb(true);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_connected = false;
        if (s_cb) s_cb(false);
    }
}

/* ------------------------------------------------------------------------- *
 * Wi-Fi manager
 * ------------------------------------------------------------------------- */

#define FALLBACK_AP_AFTER_MS 120000

static int s_retry;
static int64_t s_first_fail_us;
static bool s_have_creds;

bool at_net_has_saved_network(void) { return s_own && s_have_creds; }
static esp_timer_handle_t s_ap_off_timer;
static esp_timer_handle_t s_reconnect_timer;

static void reconnect_cb(void *arg)
{
    esp_wifi_connect();
}

static void ap_start(void)
{
    if (s_ap_active) {
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA); /* same id as the hostname: last 3 MAC bytes */
    wifi_config_t ap = {0};
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Hearth-%02x%02x%02x", mac[3], mac[4], mac[5]);
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    if (strlen(CONFIG_AUTOTERM_SETUP_AP_PASSWORD) >= 8) {
        strlcpy((char *)ap.ap.password, CONFIG_AUTOTERM_SETUP_AP_PASSWORD, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_ap_active = true;
    ESP_LOGW(TAG, "setup access point \"%s\" up (%s), open http://192.168.4.1/",
             s_ap_ssid, ap.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
}

static void ap_stop_cb(void *arg)
{
    if (s_connected && s_ap_active) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_ap_active = false;
        ESP_LOGI(TAG, "setup access point stopped");
    }
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (s_have_creds) esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        s_disc_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        s_disc_count++;
        s_connected = false;
        if (s_cb) s_cb(false);
        if (!s_have_creds) break;
        int64_t now = esp_timer_get_time();
        if (!s_first_fail_us) s_first_fail_us = now;
        if (now - s_first_fail_us > (int64_t)FALLBACK_AP_AFTER_MS * 1000) {
            ap_start();
        }
        int delay = s_retry < 5 ? 1000 << s_retry : 30000; /* 1,2,4,8,16 then 30 s */
        s_retry++;
        esp_timer_start_once(s_reconnect_timer, (int64_t)delay * 1000);
        break;
    }
    case WIFI_EVENT_STA_CONNECTED:
        s_retry = 0;
        s_first_fail_us = 0;
        break;
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "setup client joined");
        break;
    default:
        break;
    }
}

static void on_ip_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    on_ip(arg, base, id, data);
    if (id == IP_EVENT_STA_GOT_IP && s_ap_active && s_ap_off_timer) {
        /* leave the AP up for 60 s so the browser that just configured us can see the result */
        esp_timer_stop(s_ap_off_timer);
        esp_timer_start_once(s_ap_off_timer, 60 * 1000000LL);
    }
}

/* ---- captive portal DNS: answer every A query with 192.168.4.1 -------------- */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        vTaskDelete(NULL);
    }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(sock);
        vTaskDelete(NULL);
    }
    uint8_t buf[300];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || !s_ap_active) {
            continue;
        }
        /* walk the question name */
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5; /* root label + QTYPE + QCLASS */
        if (p > n) continue;
        uint16_t qtype = (uint16_t)(buf[p - 4] << 8 | buf[p - 3]);
        buf[2] = 0x81; buf[3] = 0x80;           /* response, recursion available */
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0; /* answers only for A */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int out = p;
        if (qtype == 1) {
            static const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
            memcpy(buf + out, ans, sizeof(ans));
            out += sizeof(ans);
        }
        sendto(sock, buf, out, 0, (struct sockaddr *)&from, fl);
    }
}

esp_err_t at_net_set_credentials(const char *ssid, const char *pass)
{
    if (!s_own) return ESP_ERR_NOT_SUPPORTED;
    at_settings_t cfg;
    at_settings_get(&cfg);
    strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, pass ? pass : "", sizeof(cfg.wifi_pass));
    esp_err_t err = at_settings_set(&cfg);
    if (err != ESP_OK) return err;

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, cfg.wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg.wifi_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = cfg.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    s_have_creds = cfg.wifi_ssid[0] != 0;
    s_retry = 0;
    s_first_fail_us = 0;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_wifi_disconnect();
    esp_wifi_connect();
    return ESP_OK;
}

static esp_err_t start_own_wifi(void)
{
    at_settings_t cfg;
    at_settings_get(&cfg);

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t wi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wi));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_wifi, NULL));
    esp_timer_create_args_t ta = {.callback = ap_stop_cb, .name = "ap_off"};
    esp_timer_create(&ta, &s_ap_off_timer);
    esp_timer_create_args_t tr = {.callback = reconnect_cb, .name = "wifi_retry"};
    esp_timer_create(&tr, &s_reconnect_timer);
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* credentials live in our own settings */

    esp_netif_t *sta_if = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_if) esp_netif_set_hostname(sta_if, cfg.device_name);

    s_have_creds = cfg.wifi_ssid[0] != 0;
    if (s_have_creds) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, cfg.wifi_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, cfg.wifi_pass, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = cfg.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (!s_have_creds) {
        ap_start();
    }

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(cfg.device_name);
    mdns_instance_name_set("Tillerwell Hearth");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_hearth-app", "_tcp", 80, NULL, 0);
    s_mdns_ready = true;
    at_net_ha_advertise(true);

    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 3, NULL);
    return ESP_OK;
}

esp_err_t at_net_start(at_net_ip_cb_t cb, bool own_wifi)
{
    s_cb = cb;
    s_own = own_wifi;
    if (own_wifi) {
        return start_own_wifi();
    }
    /* Matter owns Wi-Fi (or Thread): only watch for the address */
    return esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip, NULL);
}
