// Matter front end. Written against the esp-matter C++ API (see docs/MATTER.md
// for the esp-matter / ESP-IDF versions this was written for). This file is
// always compiled in.

#include "at_matter.h"

#include <cstring>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_endpoint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ConnectivityManager.h>
#include <platform/DeviceInstanceInfoProvider.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include "at_app.h"
#include "sdkconfig.h"

static const char *TAG = "matter";

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t s_ep_thermostat, s_ep_coolant, s_ep_preheat, s_ep_fault;
static volatile bool s_commissioned;
static volatile bool s_wifi_up, s_thread_up; // for the LED: paired but offline blinks
static volatile bool s_started; // the unified image only starts Matter in Matter mode

// esp-matter's built-in test credentials (passcode 20202021, discriminator
// 3840). Fine for personal use; see docs/MATTER.md before giving a unit to
// anyone else.
static const char *kManualCode = "3497-011-2332";

// What Home apps call the accessory. Everything else comes from the stock provider (test credentials, IDs).
static const char *kProductName = "Autoterm Heater";
static const char *kVendorName = "Tillerwell";

class NamedInfoProvider : public chip::DeviceLayer::DeviceInstanceInfoProvider {
public:
    explicit NamedInfoProvider(chip::DeviceLayer::DeviceInstanceInfoProvider *base) : mBase(base) {}
    CHIP_ERROR GetVendorName(char *buf, size_t size) override { return copy(kVendorName, buf, size); }
    CHIP_ERROR GetProductName(char *buf, size_t size) override { return copy(kProductName, buf, size); }
    CHIP_ERROR GetProductLabel(char *buf, size_t size) override { return copy(kProductName, buf, size); }
    CHIP_ERROR GetVendorId(uint16_t &v) override { return mBase->GetVendorId(v); }
    CHIP_ERROR GetProductId(uint16_t &v) override { return mBase->GetProductId(v); }
    CHIP_ERROR GetPartNumber(char *b, size_t n) override { return mBase->GetPartNumber(b, n); }
    CHIP_ERROR GetProductURL(char *b, size_t n) override { return mBase->GetProductURL(b, n); }
    CHIP_ERROR GetSerialNumber(char *b, size_t n) override { return mBase->GetSerialNumber(b, n); }
    CHIP_ERROR GetManufacturingDate(uint16_t &y, uint8_t &m, uint8_t &d) override { return mBase->GetManufacturingDate(y, m, d); }
    CHIP_ERROR GetHardwareVersion(uint16_t &v) override { return mBase->GetHardwareVersion(v); }
    CHIP_ERROR GetHardwareVersionString(char *b, size_t n) override { return mBase->GetHardwareVersionString(b, n); }
    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &s) override { return mBase->GetRotatingDeviceIdUniqueId(s); }

private:
    static CHIP_ERROR copy(const char *src, char *buf, size_t size)
    {
        if (strlen(src) >= size) return CHIP_ERROR_BUFFER_TOO_SMALL;
        strcpy(buf, src);
        return CHIP_NO_ERROR;
    }
    chip::DeviceLayer::DeviceInstanceInfoProvider *mBase;
};

static NamedInfoProvider *s_named_info;

static void install_names()
{
    auto *base = chip::DeviceLayer::GetDeviceInstanceInfoProvider();
    if (!base || s_named_info) return;
    s_named_info = new NamedInfoProvider(base);
    chip::DeviceLayer::SetDeviceInstanceInfoProvider(s_named_info);
}

extern "C" void at_matter_get_info(at_matter_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_started) {
        out->transport = "";
        return;
    }
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    out->transport = "wifi+thread";
#elif CHIP_DEVICE_CONFIG_ENABLE_THREAD
    out->transport = "thread";
#else
    out->transport = "wifi";
#endif
    strlcpy(out->manual, kManualCode, sizeof(out->manual));
    out->commissioned = s_commissioned;
    out->online = s_wifi_up || s_thread_up;
}

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
// Matter-over-Thread (the Home app chose Thread) leaves the Wi-Fi station unconfigured, but the Tillerwell app needs
// the box on the home Wi-Fi to reach its HTTP API. So give esp-wifi the network saved in Wi-Fi mode and let CHIP's own
// station logic connect and reconnect (we do not run a second Wi-Fi manager here, so nothing fights CHIP).
// Matter-over-Wi-Fi: commissioning writes its own credentials, which we leave alone once the box is paired.
// Runs on the CHIP task (from app_event_cb). Wi-Fi + Thread + Bluetooth share the C6 radio via coexistence.
static void keep_saved_wifi()
{
    at_settings_t cfg;
    at_settings_get(&cfg);
    if (!cfg.wifi_ssid[0]) {
        ESP_LOGI(TAG, "no Wi-Fi network saved; Wi-Fi comes only from a Wi-Fi pairing");
        return; // box was never set up over Wi-Fi by the app
    }

    wifi_config_t cur = {};
    const bool have = esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK && cur.sta.ssid[0];
    const bool same = have && strncmp((const char *)cur.sta.ssid, cfg.wifi_ssid, sizeof(cur.sta.ssid)) == 0;
    const bool paired = chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
    if (same || (have && paired)) {
        // already ours, or Matter's credentials from a Wi-Fi pairing: just make sure the station is enabled
        chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_Enabled);
        return;
    }
    // STA mode must be on before esp_wifi_set_config(WIFI_IF_STA) is accepted
    chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_Enabled);
    wifi_config_t sta = {};
    strlcpy((char *)sta.sta.ssid, cfg.wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg.wifi_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = cfg.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    if (esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) {
        ESP_LOGW(TAG, "could not load the saved Wi-Fi network");
        return;
    }
    ESP_LOGI(TAG, "keeping the saved Wi-Fi network \"%s\" up alongside Matter", cfg.wifi_ssid);
    // re-trigger CHIP's station state machine so it connects with the new configuration
    chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_Enabled);
}
#endif

static void app_event_cb(const ChipDeviceEvent *event, intptr_t)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kServerReady:
        install_names();
        s_commissioned = chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
        keep_saved_wifi();
#endif
        ESP_LOGI(TAG, "Matter server ready, %s", s_commissioned ? "paired" : "not paired yet");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        s_commissioned = true;
        ESP_LOGI(TAG, "commissioning complete");
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
        keep_saved_wifi(); // a Thread pairing leaves Wi-Fi as it was; a Wi-Fi pairing brought its own
#endif
        break;
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        keep_saved_wifi(); // a failed pairing attempt may have cleared the Wi-Fi network
        break;
#endif
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        s_commissioned = chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
        ESP_LOGI(TAG, "fabric removed, %s", s_commissioned ? "still paired elsewhere" : "unpaired");
        break;
    case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
        s_wifi_up = event->WiFiConnectivityChange.Result == chip::DeviceLayer::kConnectivity_Established;
        break;
    case chip::DeviceLayer::DeviceEventType::kThreadConnectivityChange:
        s_thread_up = event->ThreadConnectivityChange.Result == chip::DeviceLayer::kConnectivity_Established;
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        // Also closes when pairing succeeds; only a window that ran out with no pairing matters here
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) break;
        ESP_LOGW(TAG, "nobody paired before the pairing window closed: back to Wi-Fi mode");
        at_settings_set_mode_and_restart(AT_MODE_WIFI);
        break;
    default:
        break;
    }
}

static esp_err_t identification_cb(identification::callback_type_t, uint16_t, uint8_t, uint8_t, void *)
{
    return ESP_OK;
}

// A controller changed something: translate to the heater.
static esp_err_t attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                     uint32_t attribute_id, esp_matter_attr_val_t *val, void *)
{
    if (type != attribute::PRE_UPDATE) {
        return ESP_OK;
    }
    if (endpoint_id == s_ep_thermostat && cluster_id == Thermostat::Id) {
        if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
            // Off, Auto and Heat make sense for a heater; Cool, Dry, Fan and the rest are refused
            const uint8_t mode = val->val.u8;
            if (mode != 0 && mode != 1 && mode != 4) return ESP_ERR_INVALID_ARG;
            at_app_set_auto(mode != 0);
        } else if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
            // "heat to" is the bottom of the app's range; the top moves up if the gap would get too small
            at_view_t v;
            at_app_view(&v);
            const float low = val->val.i16 / 100.0f, gap = at_app_band_min_gap();
            at_app_set_auto_range(low, v.auto_high - low < gap ? low + gap : v.auto_high);
        } else if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
            // "cool to" is the top of the range; the bottom moves down if needed
            at_view_t v;
            at_app_view(&v);
            const float high = val->val.i16 / 100.0f, gap = at_app_band_min_gap();
            at_app_set_auto_range(high - v.auto_low < gap ? high - gap : v.auto_low, high);
        }
    } else if (endpoint_id == s_ep_preheat && cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        if (val->val.b) {
            at_app_start_preheat(0);
        } else {
            at_app_stop();
        }
    }
    return ESP_OK;
}

static void push(uint16_t ep, uint32_t cluster, uint32_t attr, esp_matter_attr_val_t v)
{
    attribute::update(ep, cluster, attr, &v);
}

// Heater -> Matter: keep the attributes current.
static void sync_task(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        at_view_t v;
        at_app_view(&v);
        const at_snapshot_t &s = v.snap;

        if (v.have_effective_temp) {
            push(s_ep_thermostat, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id,
                 esp_matter_nullable_int16((int16_t)(v.effective_temp * 100)));
        }
        push(s_ep_thermostat, Thermostat::Id, Thermostat::Attributes::SystemMode::Id,
             esp_matter_enum8(v.auto_enabled ? 1 /* Auto: shows the range */ : 0 /* Off */));
        push(s_ep_thermostat, Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id,
             esp_matter_int16((int16_t)(v.auto_low * 100)));
        push(s_ep_thermostat, Thermostat::Id, Thermostat::Attributes::OccupiedCoolingSetpoint::Id,
             esp_matter_int16((int16_t)(v.auto_high * 100)));
        push(s_ep_thermostat, Thermostat::Id, Thermostat::Attributes::MinSetpointDeadBand::Id,
             esp_matter_int8((int8_t)(at_app_band_min_gap() * 10 + 0.5f)));
        if (s.have_status) {
            push(s_ep_coolant, TemperatureMeasurement::Id, TemperatureMeasurement::Attributes::MeasuredValue::Id,
                 esp_matter_nullable_int16((int16_t)(s.status.coolant_temp * 100)));
            bool running = s.status.state_raw == 0x02 || s.status.state_raw == 0x03;
            push(s_ep_preheat, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(running));
            push(s_ep_fault, BooleanState::Id, BooleanState::Attributes::StateValue::Id,
                 esp_matter_bool(s.status.fault == 0)); // closed = healthy
        }
    }
}

extern "C" esp_err_t at_matter_start(void)
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_openthread_platform_config_t ot = {
        // same values as esp-matter's examples/light ESP_OPENTHREAD_DEFAULT_*_CONFIG macros
        .radio_config = {.radio_mode = RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
        .port_config = {.storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10},
    };
    set_openthread_platform_config(&ot);
#endif

    node::config_t node_config;
    strlcpy(node_config.root_node.basic_information.node_label, kProductName,
            sizeof(node_config.root_node.basic_information.node_label));
    node_t *node = node::create(&node_config, attribute_update_cb, identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "node create failed");
        return ESP_FAIL;
    }

    // Heat and cool setpoints with Auto mode: Home shows them as a range. The heater has no cooling; the two
    // setpoints are the bottom and top of the app's thermostat range (start the heater low, stop it high).
    at_view_t v0;
    at_app_view(&v0);
    thermostat::config_t th;
    // heating only, so Home leaves Cool out of its mode list; the cooling setpoint stays as the range's top
    th.thermostat.control_sequence_of_operation = 2;
    th.thermostat.system_mode = v0.auto_enabled ? 1 : 0;
    th.thermostat.feature_flags = cluster::thermostat::feature::heating::get_id() |
                                  cluster::thermostat::feature::cooling::get_id() |
                                  cluster::thermostat::feature::auto_mode::get_id();
    th.thermostat.features.heating.occupied_heating_setpoint = (int16_t)(v0.auto_low * 100);
    th.thermostat.features.cooling.occupied_cooling_setpoint = (int16_t)(v0.auto_high * 100);
    th.thermostat.features.auto_mode.min_setpoint_dead_band = (int8_t)(at_app_band_min_gap() * 10 + 0.5f);
    endpoint_t *ep = thermostat::create(node, &th, ENDPOINT_FLAG_NONE, nullptr);
    s_ep_thermostat = endpoint::get_id(ep);
    {
        // the app's range is 0..30 C; the cluster's stock limits (heat from 7, cool from 16) would cut that short
        cluster_t *tc = cluster::get(ep, Thermostat::Id);
        cluster::thermostat::attribute::create_abs_min_heat_setpoint_limit(tc, 0);
        cluster::thermostat::attribute::create_abs_max_heat_setpoint_limit(tc, 3000);
        cluster::thermostat::attribute::create_abs_min_cool_setpoint_limit(tc, 0);
        cluster::thermostat::attribute::create_abs_max_cool_setpoint_limit(tc, 3000);
        cluster::thermostat::attribute::create_min_heat_setpoint_limit(tc, 0);
        cluster::thermostat::attribute::create_max_heat_setpoint_limit(tc, 3000);
        cluster::thermostat::attribute::create_min_cool_setpoint_limit(tc, 0);
        cluster::thermostat::attribute::create_max_cool_setpoint_limit(tc, 3000);
    }

    temperature_sensor::config_t ts;
    ep = temperature_sensor::create(node, &ts, ENDPOINT_FLAG_NONE, nullptr);
    s_ep_coolant = endpoint::get_id(ep);

    on_off_plug_in_unit::config_t plug;
    ep = on_off_plug_in_unit::create(node, &plug, ENDPOINT_FLAG_NONE, nullptr);
    s_ep_preheat = endpoint::get_id(ep);

    contact_sensor::config_t cs;
    ep = contact_sensor::create(node, &cs, ENDPOINT_FLAG_NONE, nullptr);
    s_ep_fault = endpoint::get_id(ep);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    // Both transports in one image: the controller picks Wi-Fi or Thread while commissioning
    secondary_network_interface::config_t sec;
    ep = secondary_network_interface::create(node, &sec, ENDPOINT_FLAG_NONE, nullptr);
    if (!ep) {
        ESP_LOGE(TAG, "secondary network interface endpoint failed");
        return ESP_FAIL;
    }
#endif

    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    // Do not wait for kServerReady: it only fires once DNS-SD is up, which may need a network we have not given yet
    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) { keep_saved_wifi(); }, 0);
#endif
    xTaskCreate(sync_task, "matter_sync", 4096, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "Matter started; manual pairing code %s (see the log above for the QR code)", kManualCode);
    return ESP_OK;
}
