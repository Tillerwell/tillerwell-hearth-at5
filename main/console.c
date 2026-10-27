/* USB serial console: quick status, manual commands and network setup without
 * a browser. Type "help". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "at_app.h"
#include "at_matter.h"
#include "at_mqtt.h"
#include "at_net.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

static int cmd_status(int argc, char **argv)
{
    at_view_t v;
    at_app_view(&v);
    const at_snapshot_t *s = &v.snap;
    printf("state: %s%s\n", s->have_status ? (s->state_name ? s->state_name : "unknown") : "no data",
           s->stale ? " (stale)" : "");
    if (s->have_status) {
        printf("fault: %u %s\n", s->status.fault, s->fault_name ? s->fault_name : "");
        printf("coolant: %.1f C, burner %s, run %u min\n", s->status.coolant_temp, s->status.burner_active ? "on" : "off",
               s->status.elapsed_min);
    }
    if (s->have_cabin) printf("controller temperature: %d C\n", s->cabin_temp);
    if (v.have_effective_temp) printf("thermostat uses: %.1f C%s\n", v.effective_temp, v.effective_is_external ? " (external)" : "");
    printf("auto: %s target %.1f | freeze guard: %s floor %.1f | bypass: %s\n", v.auto_enabled ? "on" : "off",
           v.auto_target, v.freeze_enabled ? "on" : "off", v.freeze_target, s->bypass ? "ON" : "off");
    printf("frames: heater %lu (crc %lu), controller %lu (crc %lu); commands ok %lu failed %lu\n",
           (unsigned long)s->frames_heater, (unsigned long)s->bad_crc_heater, (unsigned long)s->frames_panel,
           (unsigned long)s->bad_crc_panel, (unsigned long)s->inject_ok, (unsigned long)s->inject_fail);
    char ip[20];
    at_net_ip_str(ip, sizeof(ip));
    printf("network: %s%s\n", ip[0] ? ip : "not connected", at_net_setup_ap_active() ? " (setup AP up)" : "");
    printf("heap: %u free, %u lowest, %u largest block\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return 0;
}

static int cmd_start(int argc, char **argv) { printf(at_app_start_thermostat() ? "queued\n" : "not sent\n"); return 0; }
static int cmd_stop(int argc, char **argv) { printf(at_app_stop() ? "queued\n" : "not sent\n"); return 0; }
static int cmd_pump(int argc, char **argv) { printf(at_app_start_pump() ? "queued\n" : "not sent\n"); return 0; }
static int cmd_preheat(int argc, char **argv)
{
    printf(at_app_start_preheat(argc > 1 ? atoi(argv[1]) : 0) ? "queued\n" : "not sent\n");
    return 0;
}

static int cmd_auto(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: auto on|off|<target C>\n");
        return 1;
    }
    if (!strcmp(argv[1], "on")) at_app_set_auto(true);
    else if (!strcmp(argv[1], "off")) at_app_set_auto(false);
    else at_app_set_auto_target(atof(argv[1]));
    return 0;
}

static int cmd_freeze(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: freeze on|off|<floor C>\n");
        return 1;
    }
    if (!strcmp(argv[1], "on")) at_app_set_freeze(true);
    else if (!strcmp(argv[1], "off")) at_app_set_freeze(false);
    else at_app_set_freeze_target(atof(argv[1]));
    return 0;
}

static int cmd_bypass(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: bypass on|off\n");
        return 1;
    }
    at_app_set_bypass(!strcmp(argv[1], "on"));
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: wifi <ssid> [password]\n");
        return 1;
    }
    esp_err_t e = at_net_set_credentials(argv[1], argc > 2 ? argv[2] : "");
    printf(e == ESP_OK ? "saved, joining\n" : "failed: %s\n", esp_err_to_name(e));
    return 0;
}

static int cmd_matter(int argc, char **argv)
{
    at_matter_info_t m;
    at_matter_get_info(&m);
    if (!m.transport || !m.transport[0]) {
        printf("this firmware has no Matter\n");
        return 0;
    }
    printf("Matter over %s, %s, manual code %s\n", m.transport, m.commissioned ? "paired" : "not paired", m.manual);
    return 0;
}

static int cmd_mode(int argc, char **argv)
{
    at_settings_t c;
    at_settings_get(&c);
    if (argc < 2 || (strcmp(argv[1], "wifi") && strcmp(argv[1], "matter"))) {
        printf("mode: %s\nusage: mode wifi|matter (restarts)\n", c.mode == AT_MODE_MATTER ? "matter" : "wifi");
        return argc < 2 ? 0 : 1;
    }
    printf("restarting in %s mode\n", argv[1]);
    at_settings_set_mode_and_restart(!strcmp(argv[1], "matter") ? AT_MODE_MATTER : AT_MODE_WIFI);
    return 0;
}

static int cmd_timezone(int argc, char **argv)
{
    at_timer_status_t ts;
    at_app_timer_status(&ts);
    if (argc < 2) {
        printf("time zone: %s\nusage: timezone <IANA name such as Europe/London | POSIX string such as GMT0BST,M3.5.0/1,M10.5.0>\n",
               ts.timezone[0] ? ts.timezone : "not set");
        return 0;
    }
    if (!at_app_set_timezone(argv[1])) {
        printf("not recognised: use a name from the built-in list or a full POSIX TZ string\n");
        return 1;
    }
    printf("saved\n");
    return 0;
}

static int cmd_timers(int argc, char **argv)
{
    at_timer_status_t ts;
    at_app_timer_status(&ts);
    printf("clock: %s (time %s, zone %s)\n", ts.clock_ok ? "ok" : "NOT ok, timers will not fire", ts.time_set ? "set" : "not set",
           ts.tz_set ? ts.timezone : "not set");
    static const char *const DAY[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    at_timer_t list[AT_TIMERS_MAX];
    int n = at_app_timers_get(list, AT_TIMERS_MAX);
    if (!n) printf("no timers\n");
    for (int i = 0; i < n; i++) {
        const at_timer_t *t = &list[i];
        char when[48] = "";
        if (t->one_off) {
            at_format_date(t->date_day, when);
        } else {
            for (int d = 0; d < 7; d++) {
                if (!(t->days & (1u << d))) continue;
                if (when[0]) strlcat(when, " ", sizeof(when));
                strlcat(when, DAY[d], sizeof(when));
            }
        }
        printf("%s %s %02u:%02u for %u min %s%s\n", t->enabled ? "on " : "off", t->id, t->start_minute / 60, t->start_minute % 60,
               t->duration_min, t->mode == AT_TMODE_THERMOSTAT ? "thermostat " : "preheat ", when);
    }
    if (ts.have_next) printf("next: %s at epoch %lld\n", ts.next_id, (long long)ts.next_start_epoch);
    if (ts.active) printf("running: %s\n", ts.active_id);
    if (ts.note[0]) printf("last event: %s\n", ts.note);
    return 0;
}

static int cmd_reboot(int argc, char **argv) { esp_restart(); return 0; }
static int cmd_factory(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "yes")) {
        printf("this erases all settings and the Matter pairing. Run: factory-reset yes\n");
        return 1;
    }
    at_settings_factory_reset();
    return 0;
}

void at_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "hearth> ";
    esp_console_dev_usb_serial_jtag_config_t uc = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    if (esp_console_new_repl_usb_serial_jtag(&uc, &rc, &repl) != ESP_OK) {
        return;
    }
    esp_console_register_help_command();
    static const esp_console_cmd_t cmds[] = {
        {.command = "status", .help = "show heater and link status", .func = cmd_status},
        {.command = "start", .help = "start in thermostat mode", .func = cmd_start},
        {.command = "stop", .help = "stop the heater", .func = cmd_stop},
        {.command = "preheat", .help = "preheat [minutes]", .func = cmd_preheat},
        {.command = "pump", .help = "run the fuel pump / ventilation only", .func = cmd_pump},
        {.command = "auto", .help = "auto on|off|<target C>", .func = cmd_auto},
        {.command = "freeze", .help = "freeze on|off|<floor C>", .func = cmd_freeze},
        {.command = "bypass", .help = "bypass on|off (never send commands)", .func = cmd_bypass},
        {.command = "wifi", .help = "wifi <ssid> [password]", .func = cmd_wifi},
        {.command = "mode", .help = "mode wifi|matter (restarts)", .func = cmd_mode},
        {.command = "matter", .help = "Matter pairing info", .func = cmd_matter},
        {.command = "timezone", .help = "timezone [IANA name | POSIX string]", .func = cmd_timezone},
        {.command = "timers", .help = "show the clock, the timers and the next one", .func = cmd_timers},
        {.command = "reboot", .help = "restart", .func = cmd_reboot},
        {.command = "factory-reset", .help = "factory-reset yes", .func = cmd_factory},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
    esp_console_start_repl(repl);
}
