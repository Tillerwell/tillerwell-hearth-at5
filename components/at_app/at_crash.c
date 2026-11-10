/* Where the box last crashed, kept across the restart (1.0046). The box has no core-dump partition, and an update
 * over the air cannot add one, so the panic handler is wrapped (-Wl,--wrap=esp_panic_handler, CMakeLists.txt) to
 * copy the crash point into RTC memory first, which a software restart leaves alone. GET /api/info shows it; the
 * addresses decode against the release's hearth-<ver>.elf (riscv32-esp-elf-addr2line -pfiaC -e hearth.elf ...). */
#include <string.h>

#include "at_app.h"
#include "esp_attr.h"
#include "esp_private/panic_internal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"

#define CRASH_MAGIC 0x48435253u /* "HCRS" */

typedef struct {
    uint32_t magic;
    at_crash_t c;
} kept_t;

static RTC_NOINIT_ATTR kept_t s_kept;
static at_crash_t s_last;
static bool s_have;

extern char *g_panic_abort_details;  /* esp_system: the abort() or assert message, NULL for an exception */
extern void __real_esp_panic_handler(panic_info_t *info);

static bool in_ram(uint32_t a) { return a >= 0x40800000u && a + 4 * AT_CRASH_STACK <= 0x40880000u; }
static bool is_code(uint32_t a) { return (a >= 0x42000000u && a < 0x42800000u) || (a >= 0x40800000u && a < 0x40880000u); }

static void copy_str(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < n; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* Runs in the panic handler: plain memory writes only. No heap, no locks, no logging (the crash may be inside one). */
void __wrap_esp_panic_handler(panic_info_t *info)
{
    at_crash_t *c = &s_kept.c;
    uint32_t before = s_kept.magic == CRASH_MAGIC ? c->crashes : 0;
    memset(c, 0, sizeof(*c));
    c->crashes = before + 1;
    c->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    const RvExcFrame *f = info ? (const RvExcFrame *)info->frame : NULL;
    if (f) {
        c->pc = (uint32_t)f->mepc;
        c->ra = (uint32_t)f->ra;
        c->sp = (uint32_t)f->sp;
        c->cause = (uint32_t)f->mcause;
        c->mtval = (uint32_t)f->mtval;
        if (in_ram(c->sp)) {
            const uint32_t *w = (const uint32_t *)c->sp;
            for (int i = 0; i < 64 && c->stack_n < AT_CRASH_STACK && in_ram(c->sp + 4u * (uint32_t)i); i++) {
                if (is_code(w[i])) c->stack[c->stack_n++] = w[i];
            }
        }
    }
    TaskHandle_t t = xTaskGetCurrentTaskHandle();
    copy_str(c->task, sizeof(c->task), t ? pcTaskGetName(t) : "");
    copy_str(c->why, sizeof(c->why), g_panic_abort_details ? g_panic_abort_details : info ? info->reason : "");
    s_kept.magic = CRASH_MAGIC;
    __real_esp_panic_handler(info);
}

void at_crash_init(void)
{
    if (s_kept.magic != CRASH_MAGIC) return;   /* power-on: RTC memory is noise, nothing was kept */
    s_kept.c.boots_since++;
    s_last = s_kept.c;
    s_last.boots_since--;   /* 0: the restart just now was this crash */
    s_have = true;
}

bool at_crash_last(at_crash_t *out)
{
    if (s_have) *out = s_last;
    return s_have;
}
