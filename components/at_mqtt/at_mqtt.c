#include "at_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "at_app.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosq_broker.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static TaskHandle_t s_pub_task;
static char s_base[80];      /* hearth/<device> */
static char s_node[48];      /* object id prefix / device identifier */
static char s_uri[96];
static char s_prefix[24];

#define MQTT_PORT   1883
#define MQTT_PREFIX "homeassistant"

bool at_mqtt_connected(void) { return s_connected; }

/* ---- built-in broker ----------------------------------------------------------- */

/* The Mosquitto port blocks inside mosq_broker_run(), so it gets its own task. Our own client connects to it over
 * loopback, so the rest of this file does not care whether the broker is built in or external. Home Assistant's MQTT
 * integration is pointed at this box and discovery works as it does with any broker. */
static struct mosq_broker_config s_bk_cfg;
static volatile bool s_bk_alive;     /* the task exists (listening or on its way up or down) */

static void broker_task(void *arg)
{
    int rc = mosq_broker_run(&s_bk_cfg);
    ESP_LOGW(TAG, "broker stopped (%d)", rc);
    s_bk_alive = false;
    vTaskDelete(NULL);
}

static void broker_stop(void)
{
    if (!s_bk_alive) return;
    mosq_broker_stop();
    for (int i = 0; i < 50 && s_bk_alive; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_bk_alive) ESP_LOGE(TAG, "broker did not stop");
}

/* Start the broker if it is not already up. Open: it only listens on the local network, like the web UI. */
static esp_err_t broker_ensure(void)
{
    if (s_bk_alive) return ESP_OK;
    s_bk_cfg = (struct mosq_broker_config){.host = "0.0.0.0", .port = MQTT_PORT};
    s_bk_alive = true;
    if (xTaskCreate(broker_task, "mqtt_broker", 8192, NULL, 4, NULL) != pdPASS) {
        s_bk_alive = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "built-in broker on port %d", MQTT_PORT);
    vTaskDelay(pdMS_TO_TICKS(300)); /* let it start listening before our own client dials in */
    return ESP_OK;
}

/* ---- discovery cleanup ------------------------------------------------------- */

/* Home Assistant uses the Tillerwell integration (HTTP), not MQTT entities. Older firmware published ~45 retained
 * discovery configs; an empty retained payload on each of their topics removes any that are left over. Only the
 * topics are needed, so nothing else of the old entity table is kept. */
static const struct {
    const char *comp;
    const char *id;
} OLD_ENTS[] = {
    {"sensor", "state"}, {"sensor", "state_code"}, {"sensor", "fault_code"}, {"sensor", "fault_name"},
    {"binary_sensor", "fault_active"}, {"sensor", "last_fault_name"}, {"sensor", "cabin_temp"},
    {"sensor", "coolant_temp"}, {"sensor", "effective_temp"}, {"sensor", "elapsed_min"},
    {"binary_sensor", "burner_active"}, {"binary_sensor", "stale"}, {"binary_sensor", "external_temp_active"},
    {"switch", "use_external_temp"}, {"sensor", "auto_note"}, {"switch", "prevent_freezing"},
    {"number", "prevent_freezing_target"}, {"sensor", "prevent_freezing_note"}, {"number", "preheat_minutes"},
    {"binary_sensor", "clock_ok"}, {"sensor", "timer_note"}, {"button", "start_preheat"},
    {"button", "start_thermostat"}, {"button", "stop"}, {"button", "start_pump"}, {"switch", "bypass_mode"},
    {"switch", "debug_mode"}, {"button", "send_debug_handshake"}, {"binary_sensor", "extended_active"},
    {"sensor", "supply_voltage"}, {"sensor", "ext_mode_name"}, {"sensor", "ext_flame_temp"},
    {"sensor", "ext_liquid_temp"}, {"sensor", "ext_overheat_temp"}, {"sensor", "ext_board_temp"},
    {"sensor", "ext_fuel_pump_hz"}, {"sensor", "ext_output_pct"}, {"binary_sensor", "ext_glow_plug"},
    {"sensor", "ext_fault_name"}, {"climate", "thermostat"},
};

static void pub(const char *topic, const char *payload, int qos, bool retain)
{
    esp_mqtt_client_enqueue(s_client, topic, payload, 0, qos, retain, true);
}

static void publish_discovery(void)
{
    char topic[160];
    for (size_t i = 0; i < sizeof(OLD_ENTS) / sizeof(OLD_ENTS[0]); i++) {
        snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", s_prefix, OLD_ENTS[i].comp, s_node, OLD_ENTS[i].id);
        pub(topic, "", 0, true);
    }
}

/* ---- commands ------------------------------------------------------------------ */

static bool is_on(const char *p) { return !strcasecmp(p, "ON") || !strcasecmp(p, "true") || !strcmp(p, "1"); }

static void handle_cmd(const char *suffix, const char *payload)
{
    if (!strcmp(suffix, "start_preheat")) at_app_start_preheat(0);
    else if (!strcmp(suffix, "start_thermostat")) at_app_start_thermostat();
    else if (!strcmp(suffix, "stop")) at_app_stop();
    else if (!strcmp(suffix, "start_pump")) at_app_start_pump();
    else if (!strcmp(suffix, "send_debug_handshake")) at_app_send_pubr0();
    else if (!strcmp(suffix, "preheat_minutes/set")) at_app_set_preheat_minutes((int)atof(payload));
    else if (!strcmp(suffix, "auto_mode/set")) at_app_set_auto(!strcasecmp(payload, "heat"));
    else if (!strcmp(suffix, "auto_target/set")) at_app_set_auto_target(atof(payload));
    else if (!strcmp(suffix, "prevent_freezing/set")) at_app_set_freeze(is_on(payload));
    else if (!strcmp(suffix, "prevent_freezing_target/set")) at_app_set_freeze_target(atof(payload));
    else if (!strcmp(suffix, "debug_mode/set")) at_app_set_debug(is_on(payload), 0);
    else if (!strcmp(suffix, "bypass_mode/set")) at_app_set_bypass(is_on(payload));
    else if (!strcmp(suffix, "use_external_temp/set")) at_app_set_use_external_temp(is_on(payload));
    else if (!strcmp(suffix, "external_temp/set")) {
        char *end;
        double v = strtod(payload, &end);
        if (end != payload && v > -60 && v < 80) at_app_set_external_temp((float)v);
    } else {
        ESP_LOGW(TAG, "unknown command %s", suffix);
    }
}

static void publish_state(void)
{
    char *js = at_app_state_json();
    if (js) {
        char topic[96];
        snprintf(topic, sizeof(topic), "%s/state", s_base);
        pub(topic, js, 0, true);
        free(js);
    }
}

/* The timer list, same JSON as GET /api/timers, retained. Edit it by publishing the whole array to
 * <base>/cmd/timers/set; the outcome appears on <base>/timers/result. */
static uint32_t s_timers_version = UINT32_MAX;

static void publish_timers(void)
{
    char *js = at_app_timers_json();
    if (js) {
        char topic[96];
        snprintf(topic, sizeof(topic), "%s/timers", s_base);
        pub(topic, js, 0, true);
        free(js);
    }
    s_timers_version = at_app_timers_version();
}

static void publish_timers_result(int code, const char *err)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", code == 200);
    if (code != 200) cJSON_AddStringToObject(o, "error", err);
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (js) {
        char topic[96];
        snprintf(topic, sizeof(topic), "%s/timers/result", s_base);
        pub(topic, js, 0, false);
        free(js);
    }
}

#define TIMERS_MAX_BODY 10240
static char *s_tbuf;          /* a timers/set message being reassembled from fragments */
static bool s_tcollect;

/* Returns true if the event belonged to the timers/set topic (and was handled). */
static bool handle_timers_data(esp_mqtt_event_handle_t ev)
{
    if (ev->current_data_offset == 0) {
        free(s_tbuf);
        s_tbuf = NULL;
        char want[100];
        snprintf(want, sizeof(want), "%s/cmd/timers/set", s_base);
        s_tcollect = (size_t)ev->topic_len == strlen(want) && !memcmp(ev->topic, want, (size_t)ev->topic_len);
        if (s_tcollect && ev->total_data_len > 0 && ev->total_data_len <= TIMERS_MAX_BODY) {
            s_tbuf = malloc((size_t)ev->total_data_len + 1);
        }
    }
    if (!s_tcollect) return false;
    if (s_tbuf) memcpy(s_tbuf + ev->current_data_offset, ev->data, (size_t)ev->data_len);
    if (ev->current_data_offset + ev->data_len >= ev->total_data_len) {
        char err[160] = "payload too large or out of memory";
        int code = 400;
        if (s_tbuf) {
            s_tbuf[ev->total_data_len] = 0;
            code = at_app_timers_put_json(s_tbuf, err, sizeof(err));
        }
        free(s_tbuf);
        s_tbuf = NULL;
        s_tcollect = false;
        publish_timers_result(code, err);
        publish_timers();
    }
    return true;
}

static void pub_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!s_connected) continue;
        publish_state();
        if (at_app_timers_version() != s_timers_version) publish_timers();
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        ESP_LOGI(TAG, "connected");
        char t[96];
        snprintf(t, sizeof(t), "%s/availability", s_base);
        pub(t, "online", 0, true);
        publish_discovery();
        snprintf(t, sizeof(t), "%s/cmd/#", s_base);
        esp_mqtt_client_subscribe(s_client, t, 0);
        publish_state();
        publish_timers();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        free(s_tbuf); /* a timers/set cut off mid-message */
        s_tbuf = NULL;
        s_tcollect = false;
        break;
    case MQTT_EVENT_DATA: {
        if (handle_timers_data(ev)) break;
        char topic[128], payload[64];
        if (ev->topic_len <= 0 || (size_t)ev->topic_len >= sizeof(topic) || ev->data_len >= (int)sizeof(payload) ||
            ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) {
            break;
        }
        memcpy(topic, ev->topic, ev->topic_len);
        topic[ev->topic_len] = 0;
        memcpy(payload, ev->data, ev->data_len);
        payload[ev->data_len] = 0;
        char pre[100];
        snprintf(pre, sizeof(pre), "%s/cmd/", s_base);
        if (!strncmp(topic, pre, strlen(pre))) handle_cmd(topic + strlen(pre), payload);
        break;
    }
    default:
        break;
    }
}

void at_mqtt_reload(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }
    at_mqtt_start();
}

esp_err_t at_mqtt_start(void)
{
    if (s_client) {
        return ESP_OK;
    }
    at_settings_t cfg;
    at_settings_get(&cfg);
    esp_err_t err = broker_ensure();
    if (err != ESP_OK) return err;
    snprintf(s_base, sizeof(s_base), "hearth/%s", cfg.device_name);
    snprintf(s_node, sizeof(s_node), "hearth_%s", cfg.device_name);
    for (char *p = s_node; *p; p++) {
        if (*p == '-') *p = '_';
    }
    strlcpy(s_prefix, MQTT_PREFIX, sizeof(s_prefix));
    snprintf(s_uri, sizeof(s_uri), "mqtt://127.0.0.1:%d", MQTT_PORT);
    static char lwt_topic[96], client_id[48];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/availability", s_base);
    snprintf(client_id, sizeof(client_id), "%s", s_node);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = s_uri,
        .credentials.client_id = client_id,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 2000,
        .session.last_will = {.topic = lwt_topic, .msg = "offline", .msg_len = 7, .qos = 0, .retain = true},
        .buffer.size = 2048,
        .buffer.out_size = 2048,
    };
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    if (!s_pub_task) {
        xTaskCreate(pub_task, "mqtt_pub", 4096, NULL, 4, &s_pub_task);
    }
    ESP_LOGI(TAG, "connecting to %s as %s", s_uri, s_node);
    return esp_mqtt_client_start(s_client);
}
