// Hooks the access logic to NVS, the hardware RNG and the clock.
#include <stdio.h>
#include <time.h>

#include "at_access.h"
#include "at_app.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"

static bool io_wall_time(int64_t *now)
{
    at_timer_status_t ts;
    at_app_timer_status(&ts);
    if (!ts.time_set) return false;
    *now = (int64_t)time(NULL);
    return true;
}

static void io_random(void *buf, size_t len) { esp_fill_random(buf, len); }
static uint32_t io_uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }
static int io_save(const char *key, const void *in, size_t len) { return at_settings_blob_save(key, in, len); }

static const at_access_io_t IO = {
    .load = at_settings_blob_load,
    .save = io_save,
    .random = io_random,
    .uptime_s = io_uptime_s,
    .wall_time = io_wall_time,
};

void at_access_start(void)
{
    uint8_t mac[6];
    char id[24];
    esp_efuse_mac_get_default(mac);
    snprintf(id, sizeof(id), "hearth-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    at_access_init(&IO, id);
}
