#include "at_cloudlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "at_access.h"
#include "at_app.h"
#include "at_cloud.h"
#include "at_cloudlink_policy.h"
#include "at_net.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "esp_app_desc.h"

static const char *TAG = "cloud";

#define CMD_QUEUE_DEPTH 4
#define CMD_MSG_MAX 2048          // a command is about 1 KB; anything past the client's buffer is dropped unread
#define CONNECT_TIMEOUT_S 30
#define STATE_EVERY_S 15
#define STATE_OUTBOX_MAX 8192     // do not pile up more than this behind a stalled connection
#define TICK_MS 1000

extern const char root_pem_start[] asm("_binary_hearth_root_ca_pem_start");

static at_cloudlink_exec_t s_exec;
static esp_mqtt_client_handle_t s_client;
static QueueHandle_t s_cmds;
static const char *s_state = "off";

// Owned by the supervisor task; the MQTT task only sets the two flags and queues commands.
static volatile bool s_up;      // CONNACK accepted
static volatile bool s_down;    // disconnected or errored: the supervisor tears the client down
static uint8_t *s_key;
static char *s_pem;
static size_t s_key_len, s_pem_len;
static uint32_t s_gen;
static uint32_t s_failures;
static int s_endpoint;
static int64_t s_started_s, s_up_s, s_next_try_s;
static uint32_t s_connects, s_cmds_ok, s_cmds_refused, s_heap_min, s_heap_at_connect;
static char s_base[48], s_uri[96], s_client_id[32], s_t_status[64], s_t_state[64], s_t_cmd[64], s_t_ack[64], s_t_event[64];
static uint32_t s_last_fault;
static bool s_fault_known;
static int64_t s_state_s;
static uint8_t s_state_key[4];

static int64_t now_s(void) { return esp_timer_get_time() / 1000000; }

static const char *host_of(int i)
{
    return i == 0 ? AT_CLOUDLINK_HOST_1 : i == 1 ? AT_CLOUDLINK_HOST_2 : i == 2 ? AT_CLOUDLINK_HOST_3 : AT_CLOUDLINK_HOST_4;
}

static int endpoint_count(void) { return AT_CLOUDLINK_HOST_4[0] ? 4 : 3; }

static void note_heap(void)
{
    uint32_t m = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);   // lowest since boot, handshake included
    if (m && (!s_heap_min || m < s_heap_min)) s_heap_min = m;
}

void at_cloudlink_status(at_cloudlink_status_t *out)
{
    out->state = s_state;
    out->endpoint = s_endpoint;
    out->failures = s_failures;
    out->connects = s_connects;
    out->commands_ok = s_cmds_ok;
    out->commands_refused = s_cmds_refused;
    out->heap_min_online = s_heap_min;
    out->heap_at_connect = s_heap_at_connect;
}

// ---- publishing (supervisor task only) -------------------------------------------------------------------

static void publish(const char *topic, const char *payload, bool retain)
{
    if (!s_client || !s_up) return;
    if (esp_mqtt_client_get_outbox_size(s_client) > STATE_OUTBOX_MAX) return;
    esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, retain, true);
}

static void publish_ack(const char *id, const char *result, const char *reason)
{
    if (!id || !id[0]) return;   // nothing to name: say nothing
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "result", result);
    if (reason) cJSON_AddStringToObject(o, "reason", reason);
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (js) publish(s_t_ack, js, false);
    free(js);
}

static void publish_event(const char *kind, uint32_t code, const char *name, const char *id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "kind", kind);
    if (name) cJSON_AddStringToObject(o, "name", name);
    if (code) cJSON_AddNumberToObject(o, "code", code);
    if (id) cJSON_AddStringToObject(o, "id", id);
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (js) publish(s_t_event, js, false);
    free(js);
}

// The same JSON as GET /api/status with "firmware" added at the front.
static void publish_state(void)
{
    char *js = at_app_state_json();
    if (!js) return;
    if (js[0] == '{') {
        const char *ver = esp_app_get_description()->version;
        size_t n = strlen(js) + strlen(ver) + 20;
        char *out = malloc(n);
        if (out) {
            snprintf(out, n, "{\"firmware\":\"%s\",%s", ver, js + 1);
            publish(s_t_state, out, true);
            free(out);
        }
    }
    free(js);
    s_state_s = now_s();
}

// A fault appearing is an event; the state document itself goes out on a timer and whenever the run changes.
static void watch_state(void)
{
    at_view_t v;
    at_app_view(&v);
    const at_snapshot_t *s = &v.snap;
    uint32_t fault = s->have_status ? s->status.fault : 0;
    uint8_t key[4] = {s->have_status ? (uint8_t)s->status.state_raw : 0xff, (uint8_t)fault, (uint8_t)s->run_mode,
                      (uint8_t)((s->have_status && s->status.burner_active) | (v.auto_enabled << 1))};
    bool changed = memcmp(key, s_state_key, sizeof(key)) != 0;
    if (s_fault_known && fault && fault != s_last_fault) {
        publish_event("fault", fault, s->fault_name ? s->fault_name : "unknown", NULL);
    }
    s_last_fault = fault;
    s_fault_known = true;
    if (changed || now_s() - s_state_s >= STATE_EVERY_S) {
        memcpy(s_state_key, key, sizeof(key));
        publish_state();
    }
}

// ---- commands (spec §9.1) --------------------------------------------------------------------------------

static void handle_command(const char *msg)
{
    at_cloud_cmd_t cmd;
    char id[40];
    bool synced = at_app_clock_synced();
    const char *why = at_access_cloud_cmd(msg, synced ? (int64_t)time(NULL) : 0, synced, &cmd, id, sizeof(id));
    if (why) {
        ESP_LOGW(TAG, "command %s refused: %s", id[0] ? id : "?", why);
        s_cmds_refused++;
        publish_ack(id, "rejected", why);
        return;
    }
    // Step 8: the same door the web UI uses, and it can still say no.
    if (s_exec && s_exec(cmd.cmd_json)) {
        ESP_LOGI(TAG, "command %s done", id);
        s_cmds_ok++;
        publish_ack(id, "done", NULL);
        watch_state();
    } else {
        ESP_LOGW(TAG, "command %s refused by the box", id);
        s_cmds_refused++;
        publish_ack(id, "rejected", "refused");
        publish_event("refused", 0, NULL, id);
    }
}

static void drain_commands(void)
{
    char *msg;
    while (xQueueReceive(s_cmds, &msg, 0) == pdTRUE) {
        handle_command(msg);
        free(msg);
    }
}

// ---- MQTT events (the client's own task) --------------------------------------------------------------------

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_up_s = now_s();
        s_connects++;
        s_heap_at_connect = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
        note_heap();
        s_up = true;
        esp_mqtt_client_subscribe(s_client, s_t_cmd, 1);
        esp_mqtt_client_enqueue(s_client, s_t_status, "online", 0, 1, true, true);
        s_state_s = 0;   // the supervisor publishes the state at once
        memset(s_state_key, 0xaa, sizeof(s_state_key));
        ESP_LOGI(TAG, "connected to %s (free heap %u)", host_of(s_endpoint), (unsigned)s_heap_at_connect);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_up = false;
        s_down = true;
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle) {
            ESP_LOGW(TAG, "error: type %d, tls 0x%x, connect refused %d", (int)ev->error_handle->error_type,
                     (unsigned)ev->error_handle->esp_tls_last_esp_err, (int)ev->error_handle->connect_return_code);
        }
        s_down = true;
        break;
    case MQTT_EVENT_DATA:
        // Only whole messages: the client's buffer is bigger than any command, so a fragmented one is not one of ours.
        if (ev->current_data_offset == 0 && ev->data_len == ev->total_data_len && ev->data_len > 0 &&
            ev->data_len < CMD_MSG_MAX) {
            char *copy = malloc((size_t)ev->data_len + 1);
            if (copy) {
                memcpy(copy, ev->data, (size_t)ev->data_len);
                copy[ev->data_len] = 0;
                if (xQueueSend(s_cmds, &copy, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "command dropped: queue full");
                    free(copy);
                }
            }
        }
        break;
    default:
        break;
    }
}

// ---- the client ------------------------------------------------------------------------------------------

static void free_identity(void)
{
    if (s_key) memset(s_key, 0, 160);
    free(s_key);
    free(s_pem);
    s_key = NULL;
    s_pem = NULL;
}

// A clean disconnect does not fire the Last Will, so a deliberate stop says "offline" itself first.
static void client_stop(bool say_offline)
{
    if (!s_client) return;
    if (say_offline && s_up) {
        esp_mqtt_client_enqueue(s_client, s_t_status, "offline", 0, 1, true, true);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_up = false;
    s_down = false;
    free_identity();
    char *msg;
    while (xQueueReceive(s_cmds, &msg, 0) == pdTRUE) free(msg);
}

static bool client_start(void)
{
    s_key = malloc(160);
    s_pem = malloc(AT_CLOUD_CERT_MAX);
    if (!s_key || !s_pem || !at_cloud_identity(s_key, 160, &s_key_len, s_pem, AT_CLOUD_CERT_MAX, &s_pem_len)) {
        free_identity();
        return false;
    }
    s_gen = at_cloud_generation();
    snprintf(s_uri, sizeof(s_uri), "mqtts://%s:%d", host_of(s_endpoint), AT_CLOUDLINK_PORT);
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = s_uri,
        .broker.verification.certificate = root_pem_start,
        .broker.verification.certificate_len = strlen(root_pem_start) + 1,
        // The name checked is the host dialled, so the broker certificate must carry every entry in the list.
        .broker.verification.skip_cert_common_name_check = false,
        .credentials.client_id = s_client_id,
        .credentials.authentication.certificate = s_pem,
        .credentials.authentication.certificate_len = s_pem_len,
        .credentials.authentication.key = (const char *)s_key,
        .credentials.authentication.key_len = s_key_len,
        .session.keepalive = 60,
        .session.last_will = {.topic = s_t_status, .msg = "offline", .msg_len = 7, .qos = 1, .retain = true},
        .network.disable_auto_reconnect = true,   // the supervisor decides when and where to dial next
        .network.timeout_ms = 15000,
        .task.priority = 3,                       // below the thermostat and the heater bridge
        .task.stack_size = 7168,
        .buffer.size = 2048,
        .buffer.out_size = 2048,
    };
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        free_identity();
        return false;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    s_up = false;
    s_down = false;
    s_started_s = now_s();
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        client_stop(false);
        return false;
    }
    ESP_LOGI(TAG, "dialling %s (endpoint %d, %u failures so far)", s_uri, s_endpoint, (unsigned)s_failures);
    return true;
}

static void fail_and_wait(void)
{
    bool stable = s_up_s && now_s() - s_up_s >= AT_CLOUDLINK_STABLE_S;
    client_stop(false);
    s_up_s = 0;
    s_failures = stable ? 1 : s_failures + 1;
    s_endpoint = at_cloudlink_endpoint_for(s_failures, endpoint_count());
    uint32_t d = at_cloudlink_retry_delay_s(s_failures);
    d += esp_random() % (d / 4 + 1);   // a little spread so a fleet does not come back in step
    s_next_try_s = now_s() + d;
    s_state = "backoff";
    ESP_LOGW(TAG, "link down: retrying in %u s (failures %u, next endpoint %d)", (unsigned)d, (unsigned)s_failures, s_endpoint);
}

static void supervisor(void *arg)
{
    uint32_t seen = UINT32_MAX;   // at_cloud_generation() last looked at, so the certificate store is not read every tick
    bool has_cert = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_client ? 250 : TICK_MS));
        uint32_t gen = at_cloud_generation();
        if (gen != seen) {
            seen = gen;
            has_cert = at_cloud_has_cert();
        }
        if (s_client && (!has_cert || gen != s_gen)) {
            client_stop(true);   // the owner removed the link, or replaced the certificate
            s_failures = 0;
            s_endpoint = 0;
            s_next_try_s = 0;
        }
        if (!has_cert) {
            s_state = "off";
            vTaskDelay(pdMS_TO_TICKS(4000));   // nothing to do until an enrolment: look rarely
            continue;
        }
        if (!s_client) {
            // Network time first: the TLS dates cannot be checked without it, and commands are refused without it.
            if (!at_net_connected() || !at_app_clock_synced()) { s_state = "waiting"; continue; }
            if (now_s() < s_next_try_s) continue;
            s_state = "connecting";
            if (!client_start()) fail_and_wait();
            continue;
        }
        note_heap();
        if (s_down || (!s_up && now_s() - s_started_s > CONNECT_TIMEOUT_S)) {
            fail_and_wait();
            continue;
        }
        if (s_up) {
            s_state = "online";
            drain_commands();
            watch_state();
        }
    }
}

esp_err_t at_cloudlink_start(at_cloudlink_exec_t exec)
{
    s_exec = exec;
    snprintf(s_client_id, sizeof(s_client_id), "%s", at_access_device_id());
    snprintf(s_base, sizeof(s_base), "hearth/%s", at_access_device_id());
    snprintf(s_t_status, sizeof(s_t_status), "%s/status", s_base);
    snprintf(s_t_state, sizeof(s_t_state), "%s/state", s_base);
    snprintf(s_t_cmd, sizeof(s_t_cmd), "%s/cmd", s_base);
    snprintf(s_t_ack, sizeof(s_t_ack), "%s/ack", s_base);
    snprintf(s_t_event, sizeof(s_t_event), "%s/event", s_base);
    s_cmds = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(char *));
    if (!s_cmds) return ESP_ERR_NO_MEM;
    // Priority 2, below every heater task: when the cloud is slow or the radio is busy, only the cloud waits.
    return xTaskCreate(supervisor, "cloudlink", 6144, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
