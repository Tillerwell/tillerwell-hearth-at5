/*
 * Inline bridge core: transparent relay between the heater and the comfort
 * controller ("panel"), with command injection impersonating the panel.
 *
 * Port of the Relay / Injector / StatusModel classes from
 * https://github.com/sinise/autoterm-heater-control (MIT), reworked as one
 * non-blocking state machine so a single task owns both UARTs. No ESP-IDF
 * dependency: UART access and time are supplied by the caller, so the whole
 * thing is exercised on a host by host_tests/.
 *
 * Injection procedure (same as the base project's "divert and replay"):
 *   1. a frame is queued;
 *   2. wait until a heater status reply has been seen (cached) and the bus is
 *      between frames;
 *   3. stop relaying; answer the panel's queries from the reply cache so it
 *      keeps seeing a healthy heater;
 *   4. write the frame to the heater and wait for its reply, nothing else can
 *      be talking to the heater so the reply is unambiguous;
 *   5. resume relaying.
 * PUBR0 (extended-telemetry handshake) is the exception: it is written
 * directly without diverting, after a short quiet gap on the bus.
 *
 * Standalone: while the controller is silent (unplugged, or its line broken)
 * the core polls the heater itself, as the controller would: a status query
 * once a second and, when a room temperature is known, a cabin temperature
 * report every 10 s. Commands then go through the same divert path, which
 * has nothing to divert.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "at_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the last start seen on the bus asked for, from the controller or
 * from us: the controller's label lives only in the controller. */
typedef enum { AT_RUN_NONE = 0, AT_RUN_PREHEAT, AT_RUN_THERMOSTAT } at_run_mode_t;

typedef struct {
    /* Non-blocking: return the number of bytes copied (0 if none waiting). */
    size_t (*read)(void *ctx, uint8_t *buf, size_t max);
    void (*write)(void *ctx, const uint8_t *buf, size_t n);
    void *ctx;
} at_port_t;

/* Bus log ring (diagnostics; shown on the web UI). */
#define AT_LOG_ENTRIES 64
#define AT_LOG_BYTES   40
typedef struct {
    uint32_t ms;
    char dir;           /* P panel->heater, H heater->panel, I injected, R injection reply,
                           C cached replay to panel, F filtered (not forwarded), S stray
                           (forwarded to the heater only),
                           O own poll sent while the controller is silent,
                           E heater's echo of our stop, passed on to the
                           controller, B controller restart blocked after our stop */
    bool crc_ok;
    uint8_t len;        /* bytes stored in data (truncated to AT_LOG_BYTES) */
    uint16_t full_len;
    uint8_t data[AT_LOG_BYTES];
} at_log_entry_t;

#define AT_QUEUE_LEN 6
#define AT_CACHE_N   4
#define AT_CACHE_MAX 64

typedef struct {
    uint8_t frame[AT_MAX_FRAME];
    size_t len;
    uint32_t delay_ms;      /* wait this long after the previous item finished */
    uint32_t not_before_ms;
    uint32_t queued_ms;
    bool direct;            /* PUBR0-style: no divert */
    bool hush_set;          /* direct only: hush_ms replaces AT_DIRECT_HUSH_MS (PUBR0 experiments) */
    uint32_t hush_ms;
    bool armed;             /* not_before_ms computed */
    char note[16];
} at_queue_item_t;

typedef enum { AT_ST_RELAY = 0, AT_ST_AWAIT_REPLY } at_state_t;

typedef struct {
    at_port_t heater;       /* UART wired to the heater */
    at_port_t panel;        /* UART wired to the comfort controller */

    at_framer_t fh, fp;     /* relay framers: from heater, from panel */
    at_framer_t ih, ip;     /* framers used while diverted */

    /* status model */
    bool have_status;  at_status_t status;  uint32_t status_ms;
    bool have_cabin;   int cabin_temp;      uint32_t cabin_ms;
    bool have_ext;     at_ext_t ext;        uint32_t ext_ms;
    uint32_t ext_frames, ext_first_ms;   /* extended frames decoded since boot, and when the first came */
    bool have_last_fault; uint8_t last_fault_code; uint32_t last_fault_ms;
    bool seen_panel, seen_heater;
    uint32_t last_panel_ms, last_heater_ms;
    uint32_t frames_panel, frames_heater;

    /* standalone polling */
    bool have_room;    int room_temp;       uint32_t room_ms;
    bool polled;       uint32_t last_poll_ms;
    uint32_t hush_until_ms; /* no own polls before this: a PUBR0 handshake was just sent */
    bool cabin_sent;   uint32_t last_cabin_sent_ms;
    uint32_t own_polls;

    /* heater->panel reply cache for known query types */
    struct { bool used; uint8_t type; uint8_t len; uint8_t data[AT_CACHE_MAX]; } cache[AT_CACHE_N];

    /* injection */
    at_queue_item_t q[AT_QUEUE_LEN];
    int q_head, q_count;
    uint32_t last_done_ms;
    at_state_t state;
    at_queue_item_t cur;
    uint32_t cur_deadline_ms;
    uint8_t cached_status[AT_MAX_FRAME];
    size_t cached_status_len;
    bool bypass;
    bool have_boundary_wait;
    uint32_t boundary_wait_start;

    bool have_last_cmd;
    char last_cmd[24];
    uint32_t last_cmd_ms;
    bool last_cmd_replied;
    uint32_t inject_fail_count, inject_ok_count;

    /* A manual Hearth stop owns the heater until the physical controller is
     * deliberately started again. */
    bool panel_stop_hold;
    uint32_t panel_control_seq;
    uint8_t panel_last_control_type;
    at_run_mode_t run_mode;

    /* relay scratch */
    uint8_t out_h[AT_MAX_FRAME * 2];
    size_t out_h_len;
    uint8_t out_p[AT_MAX_FRAME * 2];
    size_t out_p_len;

    at_log_entry_t log[AT_LOG_ENTRIES];
    uint32_t log_seq;       /* total entries ever written */
} at_core_t;

#define AT_STALE_AFTER_MS       25000u
#define AT_EXT_STALE_AFTER_MS   5000u
#define AT_INJECT_REPLY_TIMEOUT_MS 2000u
#define AT_INJECT_START_TIMEOUT_MS 20000u
#define AT_BOUNDARY_WAIT_MS     2000u
#define AT_QUIET_GAP_MS         250u
#define AT_QUIET_MAX_WAIT_MS    2000u
#define AT_PANEL_SILENT_MS      5000u   /* no controller frame for this long: poll the heater ourselves */
#define AT_OWN_POLL_MS          1000u
#define AT_DIRECT_HUSH_MS       5000u   /* own polls pause this long after a direct send, as the vendor tool never polls */
#define AT_OWN_CABIN_MS         10000u
#define AT_ROOM_FRESH_MS        90000u

void at_core_init(at_core_t *c, at_port_t heater, at_port_t panel);

/* Run one iteration: move bytes both ways, advance the injection state. Call
 * every few ms. now_ms is any monotonic millisecond clock. */
void at_core_step(at_core_t *c, uint32_t now_ms);

/* Queue a raw frame for the heater. delay_ms is measured from the previous
 * queued frame finishing. Returns false if bypass is on or the queue is full. */
bool at_core_enqueue(at_core_t *c, const uint8_t *frame, size_t len, uint32_t delay_ms,
                     bool direct, const char *note, uint32_t now_ms);
void at_core_set_bypass(at_core_t *c, bool on);

/* Room temperature from another sensor, reported to the heater as the cabin
 * temperature while the controller is silent. Used while fresher than
 * AT_ROOM_FRESH_MS; otherwise the controller's last reading is used, if any. */
void at_core_set_room_temp(at_core_t *c, float celsius, uint32_t now_ms);

/* Commands (panel impersonation, sender byte 0x03). All return false if
 * nothing was queued. */
bool at_cmd_start_preheat(at_core_t *c, int minutes, uint32_t now_ms);
/* Change the total run time of a preheat that is already running (the controller's own "preheat time" frame). */
bool at_cmd_set_preheat_duration(at_core_t *c, int minutes, uint32_t now_ms);
bool at_cmd_start_thermostat(at_core_t *c, uint32_t now_ms);
bool at_cmd_stop(at_core_t *c, uint32_t now_ms);
bool at_cmd_stop_manual(at_core_t *c, uint32_t now_ms);
bool at_cmd_start_pump(at_core_t *c, uint32_t now_ms);
bool at_cmd_send_pubr0(at_core_t *c, uint32_t now_ms);
/* Experiments for a heater that does not start streaming after the plain handshake. Each only varies the PUBR0
 * frame or the bus around it, nothing else is ever sent. 1: CRC most-significant byte first. 2: bus silent for
 * 10 s before, 20 s after. 3: five sends 150 ms apart. 4: no pause in our own polls after. Else: plain. */
bool at_cmd_send_pubr0_variant(at_core_t *c, int variant, uint32_t now_ms);

/* Plain-data snapshot for the UI / MQTT / Matter layers. */
typedef struct {
    bool have_status;
    at_status_t status;
    const char *state_name;     /* NULL if unknown */
    const char *fault_name;     /* NULL if fault==0 or unknown code */
    uint32_t status_age_ms;

    bool have_cabin;
    int cabin_temp;
    uint32_t cabin_age_ms;

    bool stale;                 /* status or cabin temp older than AT_STALE_AFTER_MS */

    bool have_ext;
    at_ext_t ext;
    bool ext_active;
    uint32_t ext_frames, ext_first_ms;

    bool have_last_fault;
    uint8_t last_fault_code;
    const char *last_fault_name;
    uint32_t last_fault_ago_ms;

    bool have_last_cmd;
    char last_cmd[24];
    uint32_t last_cmd_ago_ms;
    bool last_cmd_replied;

    bool bypass;
    bool panel_stop_hold;
    uint32_t panel_control_seq;
    uint8_t panel_last_control_type;
    at_run_mode_t run_mode;
    int queue_len;
    uint32_t frames_panel, frames_heater;
    uint32_t bad_crc_heater, bad_crc_panel, resyncs_heater, resyncs_panel;
    uint32_t inject_ok, inject_fail;
    bool seen_panel, seen_heater;
    bool standalone;            /* controller silent: the box is polling the heater */
    uint32_t own_polls;
} at_snapshot_t;

void at_core_snapshot(const at_core_t *c, uint32_t now_ms, at_snapshot_t *s);

/* Copy log entries oldest-first; returns the count copied. */
int at_core_log_copy(const at_core_t *c, at_log_entry_t *out, int max);

#ifdef __cplusplus
}
#endif
