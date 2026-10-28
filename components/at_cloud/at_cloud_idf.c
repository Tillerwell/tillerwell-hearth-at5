// Hooks the cloud identity to NVS, the hardware RNG, the pinned root and the clock.
#include "at_app.h"
#include "at_cloud.h"
#include "esp_random.h"

extern const char root_pem_start[] asm("_binary_hearth_root_ca_pem_start");

static void io_random(void *buf, size_t len) { esp_fill_random(buf, len); }
static int io_save(const char *key, const void *in, size_t len) { return at_settings_blob_save(key, in, len); }
static int io_erase(const char *key) { return at_settings_blob_erase(key); }

static bool io_clock_ok(void)
{
    at_timer_status_t ts;
    at_app_timer_status(&ts);
    return ts.time_set;
}

static const at_cloud_io_t IO = {
    .load = at_settings_blob_load,
    .save = io_save,
    .erase = io_erase,
    .random = io_random,
    .root_pem = root_pem_start,
    .clock_ok = io_clock_ok,
};

void at_cloud_start(void) { at_cloud_init(&IO); }
