/* Host tests for at_proto / at_core / at_ctl. Build and run: make -C host_tests */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "at_core.h"
#include "at_ctl.h"
#include "at_fuel.h"
#include "at_proto.h"
#include "at_timers.h"
#include "at_tz.h"
#include "at_version.h"
#include "vectors.h"

static int g_fail, g_checks;
#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_fail++;                                                            \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

static bool bytes_eq(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    return an == bn && memcmp(a, b, an) == 0;
}
#define EQV(buf, n, vec) bytes_eq((buf), (n), (vec), sizeof(vec))

/* ---- protocol ------------------------------------------------------------- */

static void test_frames(void)
{
    printf("frames vs base project\n");
    uint8_t f[AT_MAX_FRAME];
    size_t n;

    static const uint8_t two[2] = {0x00, 0x22};
    n = at_build_frame(3, 1, two, 2, f);
    CHECK(EQV(f, n, V_START_THERMOSTAT));
    static const uint8_t pre[2] = {0x00, 0x1E};
    n = at_build_frame(3, 1, pre, 2, f);
    CHECK(EQV(f, n, V_START_PREHEAT));
    static const uint8_t d90[2] = {0x00, 90};
    n = at_build_frame(3, 2, d90, 2, f);
    CHECK(EQV(f, n, V_PREHEAT_TIME_90));
    n = at_build_frame(3, 3, NULL, 0, f);
    CHECK(EQV(f, n, V_STOP));
    static const uint8_t pump[2] = {0x00, 0x28};
    n = at_build_frame(3, 0x21, pump, 2, f);
    CHECK(EQV(f, n, V_PUMP));
    n = at_build_pubr0(f);
    CHECK(EQV(f, n, V_PUBR0));
    CHECK(at_build_frame(3, 1, f, AT_MAX_PAYLOAD + 1, f) == 0);
}

typedef struct {
    int frames, bad, strays;
    size_t stray_bytes;
    uint8_t last[AT_MAX_FRAME];
    size_t last_len;
} fr_t;

static void fr_cb(at_ev_kind_t k, const uint8_t *d, size_t n, bool ok, void *ctx)
{
    fr_t *r = ctx;
    if (k == AT_EV_STRAY) {
        r->strays++;
        r->stray_bytes += n;
    } else if (ok) {
        r->frames++;
        memcpy(r->last, d, n);
        r->last_len = n;
    } else {
        r->bad++;
    }
}

static void test_framer(void)
{
    printf("framer\n");
    at_framer_t f;
    fr_t r;

    /* one byte at a time */
    at_framer_init(&f);
    memset(&r, 0, sizeof(r));
    for (size_t i = 0; i < sizeof(V_STATUS_REPLY); i++) {
        at_framer_feed(&f, &V_STATUS_REPLY[i], 1, fr_cb, &r);
    }
    CHECK(r.frames == 1 && r.strays == 0 && EQV(r.last, r.last_len, V_STATUS_REPLY));
    CHECK(at_framer_empty(&f));

    /* garbage, frame, garbage, two frames back to back */
    at_framer_init(&f);
    memset(&r, 0, sizeof(r));
    uint8_t buf[256];
    size_t n = 0;
    buf[n++] = 0x11; buf[n++] = 0x22;
    memcpy(buf + n, V_STATUS_REPLY, sizeof(V_STATUS_REPLY)); n += sizeof(V_STATUS_REPLY);
    buf[n++] = 0x33;
    memcpy(buf + n, V_STOP, sizeof(V_STOP)); n += sizeof(V_STOP);
    memcpy(buf + n, V_STOP, sizeof(V_STOP)); n += sizeof(V_STOP);
    at_framer_feed(&f, buf, n, fr_cb, &r);
    CHECK(r.frames == 3);
    CHECK(r.stray_bytes == 3);
    CHECK(at_framer_empty(&f));

    /* corrupted CRC on a known device is reported, then resynced */
    at_framer_init(&f);
    memset(&r, 0, sizeof(r));
    uint8_t bad[sizeof(V_STOP)];
    memcpy(bad, V_STOP, sizeof(bad));
    bad[sizeof(bad) - 1] ^= 0xFF;
    at_framer_feed(&f, bad, sizeof(bad), fr_cb, &r);
    at_framer_feed(&f, V_STOP, sizeof(V_STOP), fr_cb, &r);
    CHECK(r.bad == 1 && r.frames == 1 && f.bad_crc == 1);

    /* the known corruption signature from PROTOCOL.md: dev byte before AA,
     * length-low byte missing; must resync and recover the next frame */
    at_framer_init(&f);
    memset(&r, 0, sizeof(r));
    static const uint8_t garbled[] = {0x04, 0xaa, 0x00, 0x0f, 0x02, 0x03, 0x00, 0x46, 0x46};
    at_framer_feed(&f, garbled, sizeof(garbled), fr_cb, &r);
    at_framer_feed(&f, V_STATUS_REPLY, sizeof(V_STATUS_REPLY), fr_cb, &r);
    at_framer_feed(&f, V_STATUS_REPLY, sizeof(V_STATUS_REPLY), fr_cb, &r);
    CHECK(r.frames >= 1);
    CHECK(EQV(r.last, r.last_len, V_STATUS_REPLY));

    /* absurd length slips instead of stalling */
    at_framer_init(&f);
    memset(&r, 0, sizeof(r));
    static const uint8_t huge[] = {0xaa, 0x04, 0xff, 0xff, 0x0f};
    at_framer_feed(&f, huge, sizeof(huge), fr_cb, &r);
    at_framer_feed(&f, V_STOP, sizeof(V_STOP), fr_cb, &r);
    CHECK(r.frames == 1);
}

static void test_decode(void)
{
    printf("decoding vs base project\n");
    at_status_t s;
    CHECK(at_decode_status(V_STATUS_PAYLOAD, sizeof(V_STATUS_PAYLOAD), &s));
    CHECK(s.state_raw == 2 && s.substate == 3 && s.fault == 78);
    CHECK(fabsf(s.coolant_temp - 51.0f) < 0.01f);
    CHECK(s.elapsed_min == 12 && s.elapsed_sec == 33 && s.burner_active);
    CHECK(strcmp(at_state_name(2), "running") == 0 && at_state_name(1) == NULL);
    CHECK(!at_decode_status(V_STATUS_PAYLOAD, 17, &s));

    at_ext_t e;
    CHECK(at_decode_ext(V_EXT_PAYLOAD, sizeof(V_EXT_PAYLOAD), &e));
    CHECK(e.flame_temp_c == V_EXT_FLAME);
    CHECK(fabsf(e.voltage - V_EXT_VOLT) < 0.001f);
    CHECK(fabsf(e.fuel_pump_hz - V_EXT_PUMP) < 0.001f);
    CHECK(e.output_pct == V_EXT_PCT);
    CHECK(e.mode_code == V_EXT_MODE_CODE);
    CHECK(strcmp(e.mode_name, V_EXT_MODE_NAME) == 0);
    CHECK(e.fault_code == 25);
    CHECK(strcmp(at_fault_name(e.fault_code), V_EXT_FAULT_NAME) == 0);
    CHECK(e.relay_state == 1 && e.fan_current_ma == 300 && e.running_time_s == 1234);
    CHECK(!at_decode_ext(V_EXT_PAYLOAD, 55, &e));

    /* every mode name and every fault name equals the base project's table */
    int bad_modes = 0, bad_faults = 0;
    for (int code = 0; code < 44; code++) {
        uint8_t p[58] = {0};
        p[0] = (uint8_t)(code / 10);
        p[1] = (uint8_t)(code % 10);
        at_decode_ext(p, sizeof(p), &e);
        if (strcmp(e.mode_name, V_MODES[code]) != 0) bad_modes++;
    }
    for (int code = 0; code < 256; code++) {
        const char *mine = at_fault_name((uint8_t)code);
        if ((mine == NULL) != (V_FAULTS[code] == NULL) || (mine && strcmp(mine, V_FAULTS[code]) != 0)) bad_faults++;
    }
    CHECK(bad_modes == 0);
    CHECK(bad_faults == 0);
}

/* ---- bus simulation ------------------------------------------------------- */

typedef struct {
    uint8_t buf[4096];
    size_t head, tail;
} fifo_t;

static void fifo_put(fifo_t *f, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) f->buf[f->tail++ % sizeof(f->buf)] = d[i];
}
static size_t fifo_get(fifo_t *f, uint8_t *d, size_t max)
{
    size_t n = 0;
    while (f->head != f->tail && n < max) d[n++] = f->buf[f->head++ % sizeof(f->buf)];
    return n;
}

/* Two wires per side. "from_X" = bytes X transmitted (core reads these);
 * "to_X" = bytes the core transmitted to X. */
typedef struct {
    fifo_t from_heater, to_heater, from_panel, to_panel;
    /* heater simulator */
    at_framer_t hf;
    uint32_t reply_at;
    uint8_t pending[AT_MAX_FRAME];
    size_t pending_len;
    int got_frames;
    uint8_t got_types[32];
    int heater_cabin; /* payload of the last cabin temperature report the heater received */
    uint8_t last_start[AT_MAX_FRAME]; /* the last start (type 0x01) frame, byte for byte */
    size_t last_start_len;
    int heater_state;
    int ack_delay_ms;
    bool heater_silent;
    /* panel simulator */
    at_framer_t pf;
    int panel_status_replies, panel_acks;
    uint32_t panel_next_poll;
    int panel_polls;
    bool panel_polling;
    int panel_ext_seen;
    int panel_start_echoes, panel_stop_echoes;
    uint8_t panel_last_status[AT_MAX_FRAME];
    size_t panel_last_status_len;
    uint32_t now;
} sim_t;

static size_t port_read_heater(void *ctx, uint8_t *b, size_t max) { return fifo_get(&((sim_t *)ctx)->from_heater, b, max); }
static void port_write_heater(void *ctx, const uint8_t *b, size_t n) { fifo_put(&((sim_t *)ctx)->to_heater, b, n); }
static size_t port_read_panel(void *ctx, uint8_t *b, size_t max) { return fifo_get(&((sim_t *)ctx)->from_panel, b, max); }
static void port_write_panel(void *ctx, const uint8_t *b, size_t n) { fifo_put(&((sim_t *)ctx)->to_panel, b, n); }

static void heater_cb(at_ev_kind_t k, const uint8_t *d, size_t n, bool ok, void *ctx)
{
    sim_t *s = ctx;
    if (k != AT_EV_FRAME || !ok) return;
    if (s->got_frames < 32) s->got_types[s->got_frames] = d[4];
    s->got_frames++;
    if (d[4] == 0x01 && n <= sizeof(s->last_start)) { memcpy(s->last_start, d, n); s->last_start_len = n; }
    if (d[4] == 0x11 && n == 8) s->heater_cabin = d[5];
    if (s->heater_silent) return;
    uint8_t out[AT_MAX_FRAME];
    size_t on = 0;
    switch (d[4]) {
    case 0x0F: {
        uint8_t st[18] = {0};
        st[0] = (uint8_t)s->heater_state;
        st[3] = st[4] = 55;
        st[12] = s->heater_state == 2 ? 0xFF : 0;
        on = at_build_frame(4, 0x0F, st, 18, out);
        break;
    }
    case 0x11: on = at_build_frame(0, 0x11, NULL, 0, out); break;
    case 0x01:
        s->heater_state = 2; /* any run time starts it */
        on = at_build_frame(4, 0x01, d + 5, n - 7, out); /* echoes the payload, as the real heater does */
        break;
    case 0x02: on = at_build_frame(2, 0x04, NULL, 0, out); break;
    case 0x03:
        s->heater_state = 0;
        on = at_build_frame(4, 0x03, NULL, 0, out);
        break;
    case 0x21: on = at_build_frame(2, 0x21, (const uint8_t[]){0, 0x28, 0}, 3, out); break;
    default: break;
    }
    if (on) {
        memcpy(s->pending, out, on);
        s->pending_len = on;
        s->reply_at = s->now + (uint32_t)s->ack_delay_ms;
    }
}

static void panel_cb(at_ev_kind_t k, const uint8_t *d, size_t n, bool ok, void *ctx)
{
    sim_t *s = ctx;
    if (k != AT_EV_FRAME || !ok) return;
    if (d[1] == 0x02 && d[4] == 0x01) s->panel_ext_seen++;
    if (d[1] == 0x04 && d[4] == 0x0F) {
        s->panel_status_replies++;
        memcpy(s->panel_last_status, d, n);
        s->panel_last_status_len = n;
    }
    if (d[1] == 0x00 && d[4] == 0x11) s->panel_acks++;
    if (d[1] == 0x04 && d[4] == 0x03) s->panel_stop_echoes++;
    if (d[1] == 0x04 && d[4] == 0x01) s->panel_start_echoes++;
}

static void sim_init(sim_t *s)
{
    memset(s, 0, sizeof(*s));
    at_framer_init(&s->hf);
    at_framer_init(&s->pf);
    s->ack_delay_ms = 40;
    s->panel_polling = true;
}

/* One millisecond of the world: panel polls, heater replies, bytes in flight. */
static void sim_tick(sim_t *s, at_core_t *c)
{
    s->now++;
    if (s->panel_polling && s->now >= s->panel_next_poll) {
        uint8_t f[AT_MAX_FRAME];
        size_t n;
        if (s->panel_polls % 2 == 0) {
            n = at_build_frame(3, 0x0F, NULL, 0, f);
        } else {
            uint8_t t = 21;
            n = at_build_frame(3, 0x11, &t, 1, f);
        }
        s->panel_polls++;
        fifo_put(&s->from_panel, f, n);
        s->panel_next_poll = s->now + 1000;
    }
    if (s->pending_len && s->now >= s->reply_at) {
        fifo_put(&s->from_heater, s->pending, s->pending_len);
        s->pending_len = 0;
    }
    at_core_step(c, s->now);
    uint8_t b[256];
    size_t n = fifo_get(&s->to_heater, b, sizeof(b));
    if (n) at_framer_feed(&s->hf, b, n, heater_cb, s);
    n = fifo_get(&s->to_panel, b, sizeof(b));
    if (n) at_framer_feed(&s->pf, b, n, panel_cb, s);
}

static void sim_run(sim_t *s, at_core_t *c, uint32_t ms)
{
    while (ms--) sim_tick(s, c);
}

static void bus_setup(sim_t *s, at_core_t *c)
{
    sim_init(s);
    at_port_t h = {port_read_heater, port_write_heater, s};
    at_port_t p = {port_read_panel, port_write_panel, s};
    at_core_init(c, h, p);
}

static void panel_control(sim_t *s, uint8_t type, const uint8_t *payload, size_t plen)
{
    uint8_t f[AT_MAX_FRAME];
    size_t n = at_build_frame(AT_DEV_PANEL, type, payload, plen, f);
    fifo_put(&s->from_panel, f, n);
}

static void test_passthrough(void)
{
    printf("relay passthrough\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 5500);
    /* panel got its status replies and acks untouched */
    CHECK(s.panel_status_replies >= 2);
    CHECK(s.panel_acks >= 2);
    CHECK(s.got_frames >= 5);
    at_snapshot_t sn;
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.have_status && sn.have_cabin && sn.cabin_temp == 21);
    CHECK(!sn.stale);
    CHECK(sn.state_name && strcmp(sn.state_name, "idle") == 0);
    CHECK(sn.bad_crc_heater == 0 && sn.bad_crc_panel == 0);
    CHECK(!sn.standalone && sn.own_polls == 0); /* controller talking: the box never polls */
}

/* The web log endpoint takes a fixed-size snapshot before formatting it.  Keep
 * that copy contract exercised without relying on a heap allocation. */
static void test_log_snapshot(void)
{
    printf("log snapshot\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 40000); /* more events than the ring retains */

    at_log_entry_t all[AT_LOG_ENTRIES];
    at_log_entry_t latest[3];
    int n = at_core_log_copy(&c, all, AT_LOG_ENTRIES);
    int m = at_core_log_copy(&c, latest, (int)(sizeof(latest) / sizeof(latest[0])));
    CHECK(n == AT_LOG_ENTRIES);
    CHECK(m == (int)(sizeof(latest) / sizeof(latest[0])));
    CHECK(memcmp(latest, all + n - m, sizeof(latest)) == 0);
}

static int count_type(sim_t *s, uint8_t t)
{
    int n = 0;
    for (int i = 0; i < s->got_frames && i < 32; i++) n += s->got_types[i] == t;
    return n;
}

static void test_injection(void)
{
    printf("command injection\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 3000);
    int base_status_replies = s.panel_status_replies;
    int before = s.got_frames;

    CHECK(at_cmd_start_thermostat(&c, s.now));
    sim_run(&s, &c, 3000);
    CHECK(count_type(&s, 0x01) == 1);
    /* The controller's "thermostat, run indefinitely" start, captured off the
     * bus.  The payload is a run time: 00 22 would stop the heater after 34 min. */
    static const uint8_t panel_thermostat[] = {0xaa, 0x03, 0x02, 0x00, 0x01, 0xff, 0xfb, 0x4a, 0x39};
    CHECK(EQV(s.last_start, s.last_start_len, panel_thermostat));
    CHECK(s.heater_state == 2);
    at_snapshot_t sn;
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.have_last_cmd && sn.last_cmd_replied && strcmp(sn.last_cmd, "thermostat") == 0);
    CHECK(sn.inject_ok == 1 && sn.inject_fail == 0 && sn.queue_len == 0);
    CHECK(s.panel_status_replies > base_status_replies); /* panel kept being served */
    (void)before;
    /* never the echo of our start: the controller stops a start it did not send */
    CHECK(s.panel_start_echoes == 0);

    /* stop */
    CHECK(at_cmd_stop(&c, s.now));
    sim_run(&s, &c, 3000);
    CHECK(count_type(&s, 0x03) == 1 && s.heater_state == 0);
    CHECK(s.panel_start_echoes == 0 && s.panel_stop_echoes == 1);

    /* preheat sends marker then duration, 1.5 s apart */
    CHECK(at_cmd_start_preheat(&c, 90, s.now));
    uint32_t t0 = s.now;
    int seen_marker_at = -1, seen_dur_at = -1;
    int markers = count_type(&s, 0x01), durs = count_type(&s, 0x02);
    for (int i = 0; i < 6000; i++) {
        sim_tick(&s, &c);
        if (seen_marker_at < 0 && count_type(&s, 0x01) > markers) seen_marker_at = (int)(s.now - t0);
        if (seen_dur_at < 0 && count_type(&s, 0x02) > durs) seen_dur_at = (int)(s.now - t0);
    }
    CHECK(seen_marker_at >= 0 && seen_dur_at >= 0);
    CHECK(seen_dur_at - seen_marker_at >= 1500);

    /* pump */
    CHECK(at_cmd_start_pump(&c, s.now));
    sim_run(&s, &c, 3000);
    CHECK(count_type(&s, 0x21) == 1);
}

static void test_injection_no_reply(void)
{
    printf("injection with a silent heater\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 3000);
    s.heater_silent = true;
    CHECK(at_cmd_stop(&c, s.now));
    sim_run(&s, &c, 6000);
    at_snapshot_t sn;
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.inject_fail >= 1 && !sn.last_cmd_replied && sn.queue_len == 0);
}

static void test_manual_stop_panel_handoff(void)
{
    printf("manual stop and controller handoff\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 3000);

    static const uint8_t thermostat[] = {0x00, 0x22};
    panel_control(&s, AT_TYPE_START, thermostat, sizeof(thermostat));
    sim_run(&s, &c, 1000);
    CHECK(s.heater_state == 2 && count_type(&s, AT_TYPE_START) == 1);

    CHECK(at_cmd_stop_manual(&c, s.now));
    sim_run(&s, &c, 1000);
    at_snapshot_t sn;
    at_core_snapshot(&c, s.now, &sn);
    CHECK(s.heater_state == 0 && sn.panel_stop_hold);
    /* the controller sees the heater's echo of the stop, as after its own */
    CHECK(s.panel_stop_echoes == 1);

    /* The controller's armed thermostat restarts the heater after an
     * external stop, with any of these (all captured).  The manual stop
     * remains authoritative. */
    int starts = count_type(&s, AT_TYPE_START);
    static const uint8_t retries[][2] = {{0x00, 0x01}, {0x00, 0x03}, {0xff, 0xfd}, {0xff, 0xfc}};
    for (int i = 0; i < 4; i++) {
        panel_control(&s, AT_TYPE_START, retries[i], 2);
        sim_run(&s, &c, 1000);
        at_core_snapshot(&c, s.now, &sn);
        CHECK(s.heater_state == 0 && count_type(&s, AT_TYPE_START) == starts && sn.panel_stop_hold);
    }

    /* Its restart cycle includes a stop of its own, then ff ff ~50 s later:
     * the stop goes to the heater but the hold stays. */
    panel_control(&s, AT_TYPE_STOP, NULL, 0);
    sim_run(&s, &c, 1000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(count_type(&s, AT_TYPE_STOP) == 2 && sn.panel_stop_hold);
    static const uint8_t cycle_restart[] = {0xff, 0xff};
    panel_control(&s, AT_TYPE_START, cycle_restart, 2);
    sim_run(&s, &c, 1000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(s.heater_state == 0 && count_type(&s, AT_TYPE_START) == starts && sn.panel_stop_hold);

    /* Selecting thermostat again on the physical controller takes ownership
     * back and starts the heater normally. */
    panel_control(&s, AT_TYPE_START, thermostat, sizeof(thermostat));
    sim_run(&s, &c, 1000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(s.heater_state == 2 && count_type(&s, AT_TYPE_START) == starts + 1);
    CHECK(!sn.panel_stop_hold && sn.panel_last_control_type == AT_TYPE_START && sn.panel_control_seq == 3);
}

static void test_run_mode(void)
{
    printf("run mode follows the last start, controller's or ours\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 3000);
    at_snapshot_t sn;
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.run_mode == AT_RUN_NONE);

    static const uint8_t thermostat[] = {0x00, 0x22}, preheat[] = {0x00, 0x1e}, restart[] = {0xff, 0xfd};
    panel_control(&s, AT_TYPE_START, thermostat, 2);
    sim_run(&s, &c, 1000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.run_mode == AT_RUN_THERMOSTAT);

    /* its stop is part of its thermostat cycle: the mode stays */
    panel_control(&s, AT_TYPE_STOP, NULL, 0);
    sim_run(&s, &c, 1000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.run_mode == AT_RUN_THERMOSTAT);

    panel_control(&s, AT_TYPE_START, preheat, 2);
    sim_run(&s, &c, 1000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.run_mode == AT_RUN_PREHEAT);

    /* only its armed thermostat restarts the heater */
    panel_control(&s, AT_TYPE_START, restart, 2);
    sim_run(&s, &c, 1000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.run_mode == AT_RUN_THERMOSTAT);

    CHECK(at_cmd_start_preheat(&c, 30, s.now));
    sim_run(&s, &c, 3000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.run_mode == AT_RUN_PREHEAT);

    CHECK(at_cmd_start_thermostat(&c, s.now));
    sim_run(&s, &c, 3000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.run_mode == AT_RUN_THERMOSTAT);
}

static void test_controller_stop_during_hearth_run(void)
{
    printf("controller stop held back during a Hearth run\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 3000);

    /* App temperature mode: the Hearth starts the heater, the controller
     * shows preheat and stops it later.  The heater keeps running and the
     * stop does not count as the controller taking control. */
    CHECK(at_cmd_start_thermostat(&c, s.now));
    sim_run(&s, &c, 3000);
    at_snapshot_t sn;
    at_core_snapshot(&c, s.now, &sn);
    uint32_t seq = sn.panel_control_seq;
    CHECK(s.heater_state == 2 && sn.hearth_run);
    panel_control(&s, AT_TYPE_STOP, NULL, 0);
    sim_run(&s, &c, 1000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(s.heater_state == 2 && count_type(&s, AT_TYPE_STOP) == 0);
    CHECK(sn.hearth_run && sn.panel_control_seq == seq);

    /* a Hearth stop ends the run; the controller's stops pass again */
    CHECK(at_cmd_stop(&c, s.now));
    sim_run(&s, &c, 3000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(s.heater_state == 0 && !sn.hearth_run);
    panel_control(&s, AT_TYPE_STOP, NULL, 0);
    sim_run(&s, &c, 1000);
    CHECK(count_type(&s, AT_TYPE_STOP) == 2);

    /* a Hearth preheat is held the same way */
    CHECK(at_cmd_start_preheat(&c, 60, s.now));
    sim_run(&s, &c, 4000);
    panel_control(&s, AT_TYPE_STOP, NULL, 0);
    sim_run(&s, &c, 1000);
    CHECK(s.heater_state == 2 && count_type(&s, AT_TYPE_STOP) == 2);

    /* a selection on the controller takes the run over, and its stop then works */
    static const uint8_t thermostat[] = {0x00, 0x22};
    panel_control(&s, AT_TYPE_START, thermostat, 2);
    sim_run(&s, &c, 1000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(!sn.hearth_run);
    panel_control(&s, AT_TYPE_STOP, NULL, 0);
    sim_run(&s, &c, 1000);
    CHECK(s.heater_state == 0 && count_type(&s, AT_TYPE_STOP) == 3);

    /* Bypass gives the controller its stop back */
    CHECK(at_cmd_start_thermostat(&c, s.now));
    sim_run(&s, &c, 3000);
    at_core_set_bypass(&c, true);
    panel_control(&s, AT_TYPE_STOP, NULL, 0);
    sim_run(&s, &c, 1000);
    CHECK(s.heater_state == 0 && count_type(&s, AT_TYPE_STOP) == 4);
}

static void test_bypass(void)
{
    printf("bypass\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 3000);
    at_core_set_bypass(&c, true);
    CHECK(!at_cmd_stop(&c, s.now));
    sim_run(&s, &c, 3000);
    CHECK(count_type(&s, 0x03) == 0);
    at_core_set_bypass(&c, false);
    CHECK(at_cmd_stop(&c, s.now));
    sim_run(&s, &c, 3000);
    CHECK(count_type(&s, 0x03) == 1);
}

static void test_standalone(void)
{
    printf("standalone: no controller, the box polls the heater and commands still go\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    s.panel_polling = false; /* controller disconnected */
    CHECK(at_cmd_stop(&c, 0));
    sim_run(&s, &c, 4000);
    CHECK(c.own_polls == 0 && count_type(&s, 0x0F) == 0); /* grace period after boot */
    sim_run(&s, &c, 6000);
    at_snapshot_t sn;
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.standalone && sn.own_polls >= 3 && count_type(&s, 0x0F) >= 3);
    CHECK(sn.have_status && !sn.stale);
    CHECK(count_type(&s, 0x03) == 1 && c.q_count == 0 && sn.inject_ok == 1);
    CHECK(count_type(&s, 0x11) == 0); /* no room temperature known: none reported */

    CHECK(at_cmd_start_thermostat(&c, s.now));
    at_core_set_room_temp(&c, 18.4f, s.now);
    sim_run(&s, &c, 3000);
    CHECK(count_type(&s, 0x01) == 1 && s.heater_state == 2);
    CHECK(count_type(&s, 0x11) == 1);
    sim_run(&s, &c, 10000);
    CHECK(count_type(&s, 0x11) == 2); /* every 10 s */

    /* PUBR0: never sent while the heater may be answering our own poll, and our polls pause after it */
    CHECK(at_cmd_send_pubr0(&c, s.now));
    uint32_t before = c.own_polls;
    while (c.q_count) sim_run(&s, &c, 10);
    CHECK(s.now - c.last_poll_ms >= AT_QUIET_GAP_MS);
    uint32_t sent_at = s.now;
    sim_run(&s, &c, AT_DIRECT_HUSH_MS - 100);
    CHECK(c.own_polls - before <= 1); /* at most the poll the handshake waited behind */
    uint32_t hushed = c.own_polls;
    sim_run(&s, &c, 2000);
    CHECK(c.own_polls > hushed && c.last_poll_ms >= sent_at + AT_DIRECT_HUSH_MS);

    /* PUBR0 experiments only reshape the same 15-byte frame */
    {
        uint8_t plain[AT_PUBR0_LEN];
        at_build_pubr0(plain);
        CHECK(at_cmd_send_pubr0_variant(&c, 1, s.now) && c.q_count == 1);
        const at_queue_item_t *qi = &c.q[c.q_head];
        CHECK(qi->len == AT_PUBR0_LEN && qi->frame[13] == plain[14] && qi->frame[14] == plain[13] && !memcmp(qi->frame, plain, 13));
        while (c.q_count) sim_run(&s, &c, 10);
        CHECK(at_cmd_send_pubr0_variant(&c, 3, s.now) && c.q_count == 5);
        while (c.q_count) sim_run(&s, &c, 10);
        CHECK(at_cmd_send_pubr0_variant(&c, 4, s.now) && c.q[c.q_head].hush_set && c.q[c.q_head].hush_ms == 0);
        while (c.q_count) sim_run(&s, &c, 10);
    }

    /* controller comes back: the box stops polling within a poll interval */
    s.panel_polling = true;
    s.panel_next_poll = s.now;
    sim_run(&s, &c, 1500);
    uint32_t polls = c.own_polls;
    sim_run(&s, &c, 3000);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(!sn.standalone && c.own_polls == polls);
}

static void test_room_temp_to_heater(void)
{
    printf("room temperature replaces the controller's on its way to the heater\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 4500);
    at_snapshot_t sn;
    at_core_snapshot(&c, s.now, &sn);
    CHECK(s.heater_cabin == 21 && !sn.room_to_heater); /* no room temperature: the controller's goes through */

    at_core_set_room_temp(&c, 17.6f, s.now);
    sim_run(&s, &c, 2500);
    at_core_snapshot(&c, s.now, &sn);
    CHECK(s.heater_cabin == 18 && sn.room_to_heater && sn.room_temp == 18);
    CHECK(sn.cabin_temp == 21);                      /* still records what the controller measured */
    CHECK(s.panel_acks >= 2 && sn.bad_crc_panel == 0 && sn.bad_crc_heater == 0);

    at_core_set_room_temp(&c, -3.0f, s.now);         /* one unsigned byte on the wire */
    sim_run(&s, &c, 2500);
    CHECK(s.heater_cabin == 0);

    at_core_set_use_room_temp(&c, false);            /* "Use external temperature" off */
    sim_run(&s, &c, 2500);
    CHECK(s.heater_cabin == 21);
    at_core_set_use_room_temp(&c, true);
    at_core_set_bypass(&c, true);                    /* bypass relays untouched */
    sim_run(&s, &c, 2500);
    CHECK(s.heater_cabin == 21);
    at_core_set_bypass(&c, false);
    sim_run(&s, &c, 2500);
    CHECK(s.heater_cabin == 0);

    sim_run(&s, &c, AT_ROOM_FRESH_MS);               /* stale: back to the controller's */
    at_core_snapshot(&c, s.now, &sn);
    CHECK(s.heater_cabin == 21 && !sn.room_to_heater);
}

static void test_ext_filter_and_pubr0(void)
{
    printf("extended frame filter and PUBR0\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 3000);
    CHECK(at_cmd_send_pubr0(&c, s.now));
    sim_run(&s, &c, 3000);
    /* PUBR0 is not a valid frame to the simulated heater's framer, so it
     * shows up as stray there; what matters is the core did not divert: the
     * panel was still served */
    CHECK(c.q_count == 0 && c.state == AT_ST_RELAY);
    CHECK(s.to_heater.head == s.to_heater.tail);

    /* heater starts streaming extended frames: decoded, never forwarded */
    uint8_t f[AT_MAX_FRAME];
    size_t n = at_build_frame(0x02, 0x01, V_EXT_PAYLOAD, sizeof(V_EXT_PAYLOAD), f);
    fifo_put(&s.from_heater, f, n);
    sim_run(&s, &c, 500);
    CHECK(s.panel_ext_seen == 0);
    at_snapshot_t sn;
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.have_ext && sn.ext_active && sn.ext.flame_temp_c == V_EXT_FLAME);
}

static void test_strays_not_to_panel(void)
{
    printf("stray bytes never reach the controller\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 2500);
    int replies = s.panel_status_replies;
    /* a line glitch on the heater RX reads as a lone 0xff between frames */
    for (int i = 0; i < 5; i++) {
        fifo_put(&s.from_heater, (const uint8_t[]){0xFF}, 1);
        sim_run(&s, &c, 1000);
    }
    CHECK(s.pf.dropped == 0);
    CHECK(s.panel_status_replies > replies);
    /* the other way stays byte-for-byte */
    fifo_put(&s.from_panel, (const uint8_t[]){0xFE}, 1);
    sim_run(&s, &c, 300);
    CHECK(s.hf.dropped == 1);
}

static void test_fault_tracking(void)
{
    printf("fault tracking\n");
    static sim_t s;
    static at_core_t c;
    bus_setup(&s, &c);
    sim_run(&s, &c, 2500);
    uint8_t st[18] = {0};
    st[2] = 78;
    uint8_t f[AT_MAX_FRAME];
    size_t n = at_build_frame(4, 0x0F, st, 18, f);
    fifo_put(&s.from_heater, f, n);
    sim_run(&s, &c, 300);
    at_snapshot_t sn;
    at_core_snapshot(&c, s.now, &sn);
    CHECK(sn.status.fault == 78 && sn.fault_name && strcmp(sn.fault_name, "Flame break during running") == 0);
    CHECK(sn.have_last_fault && sn.last_fault_code == 78);
}

/* ---- thermostat logic ------------------------------------------------------ */

typedef struct {
    int starts, stops;
} ops_log_t;
static void op_start(void *c) { ((ops_log_t *)c)->starts++; }
static void op_stop(void *c) { ((ops_log_t *)c)->stops++; }

static at_snapshot_t mk_snap(int cabin, uint8_t state, uint8_t fault)
{
    at_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.have_status = true;
    s.status.state_raw = state;
    s.status.fault = fault;
    s.fault_name = fault ? "Flame break during running" : NULL;
    s.have_cabin = true;
    s.cabin_temp = cabin;
    s.cabin_age_ms = 1000;
    return s;
}

static void test_ctl(void)
{
    printf("thermostat and frost protection\n");
    ops_log_t l = {0};
    at_ctl_ops_t ops = {op_start, op_stop, &l};
    at_ctl_t t;
    at_ctl_init(&t, 20.0f, 5.0f);
    at_snapshot_t cold = mk_snap(17, 0, 0), hot = mk_snap(22, 2, 0);
    uint32_t now = 100000;

    /* disabled: nothing */
    at_ctl_tick(&t, &cold, now, &ops);
    CHECK(l.starts == 0);

    /* enabling while cold starts immediately */
    at_ctl_set_auto(&t, true, &cold, now, &ops);
    CHECK(l.starts == 1);
    /* rate limit */
    at_ctl_tick(&t, &cold, now + 3000, &ops);
    CHECK(l.starts == 1);
    /* too warm and running: stop once, not repeatedly during cooldown */
    at_ctl_tick(&t, &hot, now + 100000, &ops);
    CHECK(l.stops == 1);
    at_snapshot_t cooling = mk_snap(22, 4, 0);
    at_ctl_tick(&t, &cooling, now + 200000, &ops);
    CHECK(l.stops == 1);
    /* idle and cold again restarts */
    at_ctl_tick(&t, &cold, now + 300000, &ops);
    CHECK(l.starts == 2);
    /* in the dead band nothing happens */
    at_snapshot_t mid = mk_snap(20, 0, 0);
    at_ctl_tick(&t, &mid, now + 400000, &ops);
    CHECK(l.starts == 2 && l.stops == 1);
    /* stale reading is ignored */
    at_snapshot_t stale = mk_snap(10, 0, 0);
    stale.cabin_age_ms = 30000;
    at_ctl_tick(&t, &stale, now + 500000, &ops);
    CHECK(l.starts == 2);

    /* enabling when already warm does not start */
    ops_log_t l2 = {0};
    at_ctl_ops_t ops2 = {op_start, op_stop, &l2};
    at_ctl_t t2;
    at_ctl_init(&t2, 20.0f, 5.0f);
    at_snapshot_t warm = mk_snap(21, 0, 0);
    at_ctl_set_auto(&t2, true, &warm, 1000, &ops2);
    CHECK(l2.starts == 0);
    at_ctl_set_auto(&t2, false, &warm, 2000, &ops2);
    CHECK(l2.stops == 1);

    /* a band of 18-21 acts like the Comfort Controller set to 20: the first run starts at 20, every
     * later one at 18, and each runs to 21 with no stop inside the band */
    {
        ops_log_t lb = {0};
        at_ctl_ops_t opsb = {op_start, op_stop, &lb};
        at_ctl_t tb;
        at_ctl_init(&tb, 20.0f, 5.0f);
        at_ctl_set_auto_range(&tb, 18.0f, 21.0f);
        at_ctl_set_hysteresis(&tb, 2.0f, 1.0f);
        uint32_t tm = 100000;
        at_snapshot_t s209 = mk_snap(21, 0, 0);
        at_ctl_set_external(&tb, 20.9f, tm);
        at_ctl_set_auto(&tb, true, &s209, tm, &opsb);
        CHECK(lb.starts == 0); /* above the set temperature: waits */
        CHECK(at_ctl_auto_start_at(&tb) == 20.0f);
        at_ctl_set_external(&tb, 20.4f, tm += 100000);
        at_ctl_tick(&tb, &s209, tm, &opsb);
        CHECK(lb.starts == 0);
        at_ctl_set_external(&tb, 20.0f, tm += 100000);
        at_ctl_tick(&tb, &s209, tm, &opsb);
        CHECK(lb.starts == 1); /* down to 20: first run */
        at_snapshot_t burning = mk_snap(20, 2, 0);
        at_ctl_set_external(&tb, 20.8f, tm += 100000);
        at_ctl_tick(&tb, &burning, tm, &opsb);
        CHECK(lb.stops == 0);
        at_ctl_set_external(&tb, 21.0f, tm += 100000);
        at_ctl_tick(&tb, &burning, tm, &opsb);
        CHECK(lb.stops == 1); /* top of the band: stop */
        CHECK(at_ctl_auto_start_at(&tb) == 18.0f);
        at_snapshot_t idle = mk_snap(20, 0, 0);
        at_ctl_set_external(&tb, 20.0f, tm += 100000);
        at_ctl_tick(&tb, &idle, tm, &opsb);
        at_ctl_set_external(&tb, 18.1f, tm += 100000);
        at_ctl_tick(&tb, &idle, tm, &opsb);
        CHECK(lb.starts == 1); /* after the first run it waits for the low end */
        at_ctl_set_external(&tb, 18.0f, tm += 100000);
        at_ctl_tick(&tb, &idle, tm, &opsb);
        CHECK(lb.starts == 2);
        /* running: 18.1 and 17.9 change nothing, it runs on to 21 */
        at_ctl_set_external(&tb, 18.1f, tm += 100000);
        at_ctl_tick(&tb, &burning, tm, &opsb);
        at_ctl_set_external(&tb, 17.9f, tm += 100000);
        at_ctl_tick(&tb, &burning, tm, &opsb);
        CHECK(lb.starts == 2 && lb.stops == 1);
        at_ctl_set_external(&tb, 21.0f, tm += 100000);
        at_ctl_tick(&tb, &burning, tm, &opsb);
        CHECK(lb.stops == 2);

        /* armed below the set temperature: starts straight away */
        ops_log_t lc = {0};
        at_ctl_ops_t opsc = {op_start, op_stop, &lc};
        at_ctl_t tc;
        at_ctl_init(&tc, 20.0f, 5.0f);
        at_ctl_set_auto_range(&tc, 18.0f, 21.0f);
        at_ctl_set_external(&tc, 19.5f, 1000);
        at_ctl_set_auto(&tc, true, &idle, 1000, &opsc);
        CHECK(lc.starts == 1);

        /* a Home Assistant reading 20 s old still counts: armed at 20.5 it waits, and the loop
         * starts at 20 on a reading up to 90 s old; an older one is no reading at all */
        ops_log_t le = {0};
        at_ctl_ops_t opse = {op_start, op_stop, &le};
        at_ctl_t te;
        at_ctl_init(&te, 20.0f, 5.0f);
        at_ctl_set_auto_range(&te, 18.0f, 21.0f);
        at_snapshot_t nocab = mk_snap(0, 0, 0);
        nocab.have_cabin = false;
        at_ctl_set_external(&te, 20.5f, 100000);
        at_ctl_set_auto(&te, true, &nocab, 120000, &opse);
        CHECK(le.starts == 0);
        at_ctl_set_external(&te, 20.0f, 200000);
        at_ctl_tick(&te, &nocab, 200000 + 60000, &opse);
        CHECK(le.starts == 1);

        /* armed with no reading at all: never starts blind */
        ops_log_t lf = {0};
        at_ctl_ops_t opsf = {op_start, op_stop, &lf};
        at_ctl_t tf;
        at_ctl_init(&tf, 20.0f, 5.0f);
        at_ctl_set_auto_range(&tf, 18.0f, 21.0f);
        at_ctl_set_external(&tf, 15.0f, 1000);
        at_ctl_set_auto(&tf, true, &nocab, 1000 + AT_CTL_EXT_STALE_MS + 1, &opsf);
        at_ctl_tick(&tf, &nocab, 1000 + AT_CTL_EXT_STALE_MS + 200000, &opsf);
        CHECK(lf.starts == 0);

        /* Auto restored at boot (no arming) waits for the low end */
        ops_log_t ld = {0};
        at_ctl_ops_t opsd = {op_start, op_stop, &ld};
        at_ctl_t td;
        at_ctl_init(&td, 20.0f, 5.0f);
        at_ctl_set_auto_range(&td, 18.0f, 21.0f);
        td.autot.enabled = true;
        at_ctl_set_external(&td, 19.5f, 100000);
        at_ctl_tick(&td, &idle, 100000, &opsd);
        CHECK(ld.starts == 0);
    }

    /* fault back-off: 5 min, 15 min, 20 min, then give up */
    ops_log_t l3 = {0};
    at_ctl_ops_t ops3 = {op_start, op_stop, &l3};
    at_ctl_t t3;
    at_ctl_init(&t3, 20.0f, 5.0f);
    at_snapshot_t warm3 = mk_snap(25, 0, 0);
    at_ctl_set_auto(&t3, true, &warm3, 0, &ops3); /* warm: no start */
    at_snapshot_t faulted = mk_snap(15, 0, 78);
    uint32_t tt = 200000;
    at_ctl_tick(&t3, &faulted, tt, &ops3);
    CHECK(l3.starts == 0); /* armed, waiting */
    at_ctl_tick(&t3, &faulted, tt + 4 * 60000, &ops3);
    CHECK(l3.starts == 0);
    at_ctl_tick(&t3, &faulted, tt + 5 * 60000 + 1000, &ops3);
    CHECK(l3.starts == 1);
    uint32_t t_after1 = tt + 5 * 60000 + 1000;
    at_ctl_tick(&t3, &faulted, t_after1 + 100000, &ops3); /* re-arm 15 min */
    at_ctl_tick(&t3, &faulted, t_after1 + 100000 + 14 * 60000, &ops3);
    CHECK(l3.starts == 1);
    at_ctl_tick(&t3, &faulted, t_after1 + 100000 + 15 * 60000 + 1000, &ops3);
    CHECK(l3.starts == 2);

    /* frost protection runs without Auto and only stops its own runs */
    ops_log_t l4 = {0};
    at_ctl_ops_t ops4 = {op_start, op_stop, &l4};
    at_ctl_t t4;
    at_ctl_init(&t4, 20.0f, 5.0f);
    at_ctl_set_freeze(&t4, true);
    at_snapshot_t frozen = mk_snap(4, 0, 0);
    at_ctl_tick(&t4, &frozen, 100000, &ops4);
    CHECK(l4.starts == 1);
    at_snapshot_t warmed = mk_snap(8, 2, 0);
    at_ctl_tick(&t4, &warmed, 300000, &ops4);
    CHECK(l4.stops == 1);
    /* a run it did not start is left alone */
    ops_log_t l5 = {0};
    at_ctl_ops_t ops5 = {op_start, op_stop, &l5};
    at_ctl_t t5;
    at_ctl_init(&t5, 20.0f, 5.0f);
    at_ctl_set_freeze(&t5, true);
    at_ctl_tick(&t5, &warmed, 100000, &ops5);
    CHECK(l5.stops == 0 && l5.starts == 0);

    /* external temperature preferred while fresh, falls back when stale */
    at_ctl_t t6;
    at_ctl_init(&t6, 20.0f, 5.0f);
    at_snapshot_t cab = mk_snap(25, 0, 0);
    float temp;
    uint32_t age;
    bool ext;
    at_ctl_set_external(&t6, 18.5f, 1000);
    CHECK(at_ctl_effective_temp(&t6, &cab, 5000, &temp, &age, &ext) && ext && temp == 18.5f);
    CHECK(at_ctl_effective_temp(&t6, &cab, 1000 + AT_CTL_EXT_STALE_MS + 1, &temp, &age, &ext) && !ext && temp == 25.0f);
}


/* ---- timers ----------------------------------------------------------------------------------------- */

static at_timer_t rep(const char *id, int start, int dur, uint8_t days)
{
    at_timer_t t;
    memset(&t, 0, sizeof(t));
    strcpy(t.id, id);
    t.enabled = true;
    t.mode = AT_TMODE_PREHEAT;
    t.start_minute = (uint16_t)start;
    t.duration_min = (uint16_t)dur;
    t.days = days;
    t.low_c = 19;
    t.high_c = 22;
    return t;
}

static at_timer_t once(const char *id, int32_t day, int start, int dur)
{
    at_timer_t t = rep(id, start, dur, 0);
    t.one_off = true;
    t.date_day = day;
    return t;
}

#define MON 0x01
#define TUE 0x02
#define SUN 0x40
#define WEEKDAYS 0x1F

static at_localtime_t lt_at(int32_t day, int minute)
{
    at_localtime_t t = {true, (int64_t)day * 86400 + minute * 60, day, (uint16_t)minute};
    return t;
}

/* Local time for a UTC instant given the zone's UTC offset. */
static at_localtime_t lt_utc(int64_t utc_s, int offset_s)
{
    int64_t l = utc_s + offset_s;
    at_localtime_t t = {true, utc_s, (int32_t)(l / 86400), (uint16_t)((l % 86400) / 60)};
    return t;
}

static bool is_ok(at_timer_problem_t p) { return p.err == AT_TERR_NONE; }

static void test_timer_rules(void)
{
    printf("timer rules\n");
    int32_t mon = at_days_from_civil(2026, 10, 26);
    CHECK(at_iso_weekday(mon) == 1 && at_iso_weekday(mon + 6) == 7 && at_iso_weekday(0) == 4);
    at_localtime_t now = lt_at(mon, 12 * 60);
    at_timer_t a[2];

    a[0] = rep("a", 390, 60, WEEKDAYS);
    CHECK(is_ok(at_timers_validate(a, 1, NULL, 0, &now, 3)));
    a[0] = rep("a", 390, 60, 0);
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_NO_DAYS);
    a[0] = rep("a", 390, 60, 0x80 | MON);
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_NO_DAYS);
    a[0] = rep("a", 390, 19, MON);
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_BAD_DURATION);
    a[0] = rep("a", 390, 481, MON);
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_BAD_DURATION);
    a[0] = rep("a", 390, 20, MON);
    CHECK(is_ok(at_timers_validate(a, 1, NULL, 0, &now, 3)));
    a[0] = rep("a", 390, 480, MON);
    CHECK(is_ok(at_timers_validate(a, 1, NULL, 0, &now, 3)));
    a[0] = rep("a", 1440, 60, MON);
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_BAD_START);
    a[0] = rep("", 390, 60, MON);
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_BAD_ID);
    a[0] = rep("a b", 390, 60, MON);
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_BAD_ID);
    a[0] = rep("a", 390, 60, MON);
    a[0].mode = 2;
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_BAD_MODE);

    /* thirty minutes between one ending and the next starting */
    a[0] = rep("a", 360, 60, MON);                          /* Mon 06:00-07:00 */
    a[1] = rep("b", 7 * 60 + 29, 20, MON);
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 3).err == AT_TERR_TOO_CLOSE);
    a[1] = rep("b", 7 * 60 + 30, 20, MON);
    CHECK(is_ok(at_timers_validate(a, 2, NULL, 0, &now, 3)));
    a[1] = rep("b", 360, 60, TUE);
    CHECK(is_ok(at_timers_validate(a, 2, NULL, 0, &now, 3)));
    at_timer_problem_t p;
    a[1] = rep("b", 7 * 60 + 29, 20, MON);
    p = at_timers_validate(a, 2, NULL, 0, &now, 3);
    CHECK(p.index == 1 && p.other == 0);
    /* ... also when the earlier one is the one listed second */
    a[0] = rep("b", 7 * 60 + 29, 20, MON);
    a[1] = rep("a", 360, 60, MON);
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 3).err == AT_TERR_TOO_CLOSE);

    /* across midnight and the end of the week */
    a[0] = rep("late", 22 * 60, 120, SUN);                  /* Sun 22:00 -> Mon 00:00 (+30) */
    a[1] = rep("early", 15, 20, MON);
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 3).err == AT_TERR_TOO_CLOSE);
    a[1] = rep("early", 30, 20, MON);
    CHECK(is_ok(at_timers_validate(a, 2, NULL, 0, &now, 3)));
    a[0] = rep("spill", 20 * 60, 480, SUN);                 /* Sun 20:00 -> Mon 04:00 (+30) */
    a[1] = rep("mon", 4 * 60 + 29, 20, MON);
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 3).err == AT_TERR_TOO_CLOSE);
    a[1] = rep("mon", 4 * 60 + 30, 20, MON);
    CHECK(is_ok(at_timers_validate(a, 2, NULL, 0, &now, 3)));
    /* the reverse: Monday run that ends late, Sunday timer just before the week wraps */
    a[0] = rep("mon", 0, 20, MON);
    a[1] = rep("sun", 23 * 60 + 40, 20, SUN);               /* Sun 23:40 -> 00:00 (+30 = 00:30) clashes with Mon 00:00 */
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 3).err == AT_TERR_TOO_CLOSE);

    /* disabled timers are ignored */
    a[0] = rep("a", 360, 60, MON);
    a[0].enabled = false;
    a[1] = rep("b", 400, 20, MON);
    CHECK(is_ok(at_timers_validate(a, 2, NULL, 0, &now, 3)));

    /* one-offs */
    int32_t day = mon + 3;
    int wd = at_iso_weekday(day);
    a[0] = once("o1", day, 600, 60);
    a[1] = once("o2", day, 11 * 60 + 20, 20);
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 3).err == AT_TERR_TOO_CLOSE);
    a[1] = once("o2", day, 11 * 60 + 30, 20);
    CHECK(is_ok(at_timers_validate(a, 2, NULL, 0, &now, 3)));
    a[1] = once("o2", day + 1, 600, 60);                    /* next day, same time */
    CHECK(is_ok(at_timers_validate(a, 2, NULL, 0, &now, 3)));
    a[1] = rep("r", 11 * 60, 20, (uint8_t)(1u << (wd - 1)));
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 3).err == AT_TERR_TOO_CLOSE);
    a[1] = rep("r", 11 * 60, 20, (uint8_t)(1u << (wd % 7)));
    CHECK(is_ok(at_timers_validate(a, 2, NULL, 0, &now, 3)));
    /* a repeating run spilling past midnight into the one-off's day */
    a[1] = rep("r", 23 * 60, 120, (uint8_t)(1u << ((wd + 5) % 7)));   /* the day before, 23:00 -> 01:00 */
    a[0] = once("o1", day, 60, 30);                                    /* 01:00 */
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 3).err == AT_TERR_TOO_CLOSE);
    a[0] = once("o1", day, 90, 30);                                    /* 01:30 */
    CHECK(is_ok(at_timers_validate(a, 2, NULL, 0, &now, 3)));

    /* past one-offs */
    a[0] = once("y", mon - 1, 600, 60);
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_IN_PAST);
    a[0] = once("t", mon, 11 * 60, 60);                      /* ended at 12:00 = now */
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_IN_PAST);
    a[0] = once("t", mon, 11 * 60 + 1, 60);
    CHECK(is_ok(at_timers_validate(a, 1, NULL, 0, &now, 3)));
    /* ... unless it is already stored: then it is just ignored, and pruned */
    a[0] = once("old", mon - 1, 600, 60);
    a[1] = rep("r", 600, 60, (uint8_t)(1u << (at_iso_weekday(mon - 1) - 1)));
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 3).err == AT_TERR_IN_PAST);
    CHECK(is_ok(at_timers_validate(a, 2, a, 1, &now, 3)));   /* overlap with the finished one-off not counted */
    CHECK(at_timers_prune_expired(a, 2, &now, NULL) == 1 && !strcmp(a[0].id, "r"));
    /* the clock not being ok: nothing is "past" */
    at_localtime_t noclock = {0};
    a[0] = once("old", mon - 1, 600, 60);
    CHECK(is_ok(at_timers_validate(a, 1, NULL, 0, &noclock, 3)));
    /* a run in progress keeps its one-off */
    a[0] = once("run", mon, 600, 60);
    CHECK(at_timers_prune_expired(a, 1, &now, "run") == 1);
    CHECK(at_timers_prune_expired(a, 1, &now, "other") == 0);

    /* bad dates */
    int32_t d;
    CHECK(at_parse_date("2026-10-26", &d) && d == mon);
    CHECK(!at_parse_date("2026-02-30", &d) && !at_parse_date("2026-13-01", &d) && !at_parse_date("2026-10-0", &d));
    CHECK(!at_parse_date("26-10-26", &d) && !at_parse_date("2026/10/26", &d) && !at_parse_date(NULL, &d));
    CHECK(at_parse_date("2028-02-29", &d) && !at_parse_date("2027-02-29", &d));
    char buf[11];
    at_format_date(mon, buf);
    CHECK(!strcmp(buf, "2026-10-26"));

    /* count, duplicate ids */
    at_timer_t many[AT_TIMERS_MAX + 1];
    for (int i = 0; i <= AT_TIMERS_MAX; i++) {
        char id[8];
        snprintf(id, sizeof(id), "t%d", i);
        many[i] = once(id, mon + 10 + i, 600, 60);
    }
    CHECK(is_ok(at_timers_validate(many, AT_TIMERS_MAX, NULL, 0, &now, 3)));
    CHECK(at_timers_validate(many, AT_TIMERS_MAX + 1, NULL, 0, &now, 3).err == AT_TERR_TOO_MANY);
    a[0] = rep("same", 360, 60, MON);
    a[1] = rep("same", 600, 60, TUE);
    CHECK(at_timers_validate(a, 2, NULL, 0, &now, 3).err == AT_TERR_DUPLICATE_ID);
    CHECK(is_ok(at_timers_validate(a, 0, NULL, 0, &now, 3)));

    /* thermostat range honours the hysteresis minimum */
    a[0] = rep("th", 360, 60, MON);
    a[0].mode = AT_TMODE_THERMOSTAT;
    a[0].low_c = 19;
    a[0].high_c = 22;
    CHECK(is_ok(at_timers_validate(a, 1, NULL, 0, &now, 3)));
    a[0].high_c = 21;
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_BAD_RANGE);
    CHECK(is_ok(at_timers_validate(a, 1, NULL, 0, &now, 2)));
    a[0].high_c = 19;
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 0).err == AT_TERR_BAD_RANGE);
    a[0].low_c = -1;
    a[0].high_c = 22;
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_BAD_RANGE);
    a[0].low_c = 19;
    a[0].high_c = 31;
    CHECK(at_timers_validate(a, 1, NULL, 0, &now, 3).err == AT_TERR_BAD_RANGE);
    /* the range is ignored for preheat */
    a[0].mode = AT_TMODE_PREHEAT;
    a[0].low_c = 30;
    a[0].high_c = 0;
    CHECK(is_ok(at_timers_validate(a, 1, NULL, 0, &now, 3)));
}

typedef struct {
    int preheats, last_minutes, begins, ends, last_disarm, low, high;
    bool fail;
} fake_t;

static bool f_preheat(void *c, int m)
{
    fake_t *f = c;
    if (f->fail) return false;
    f->preheats++;
    f->last_minutes = m;
    return true;
}
static bool f_begin(void *c, int lo, int hi)
{
    fake_t *f = c;
    if (f->fail) return false;
    f->begins++;
    f->low = lo;
    f->high = hi;
    return true;
}
static void f_end(void *c, bool disarm)
{
    fake_t *f = c;
    f->ends++;
    f->last_disarm = disarm;
}

static void test_timer_schedule(void)
{
    printf("timer firing\n");
    int32_t mon = at_days_from_civil(2026, 10, 26);
    fake_t f = {0};
    at_timer_ops_t ops = {f_preheat, f_begin, f_end, &f};
    at_snapshot_t idle = mk_snap(15, 0, 0);
    at_timer_t tl[4];
    at_runner_t r;
    uint32_t ms = 1000000;

    /* weekday 06:30 timer: fires once on Mon-Fri, never Sat/Sun; the first poll and a late clock never fire it */
    tl[0] = rep("wk", 390, 60, WEEKDAYS);
    at_runner_init(&r);
    for (int d = 0; d < 7; d++) {
        for (int m = 6 * 60; m <= 8 * 60; m++) {
            at_localtime_t t = lt_at(mon + d, m);
            if (d == 0 && m == 6 * 60 + 31) { /* never mind: handled in the next block */ }
            at_runner_tick(&r, tl, 1, &t, ms += 60000, &idle, false, &ops);
        }
    }
    /* the run itself leaves the heater busy in reality; here idle with no grace expiry, so it ends at once per day */
    CHECK(f.preheats == 5 && f.last_minutes == 60);

    /* clock not ok, then ok after the start time: missed, not run late */
    memset(&f, 0, sizeof(f));
    at_runner_init(&r);
    at_localtime_t bad = {0};
    at_runner_tick(&r, tl, 1, &bad, ms += 1000, &idle, false, &ops);
    at_localtime_t t1 = lt_at(mon, 6 * 60 + 31);
    at_runner_tick(&r, tl, 1, &t1, ms += 1000, &idle, false, &ops);
    at_localtime_t t2 = lt_at(mon, 6 * 60 + 32);
    at_runner_tick(&r, tl, 1, &t2, ms += 1000, &idle, false, &ops);
    CHECK(f.preheats == 0);
    /* booting on the very minute it is due: also not fired */
    at_runner_init(&r);
    at_localtime_t t3 = lt_at(mon + 1, 6 * 60 + 30);
    at_runner_tick(&r, tl, 1, &t3, ms += 1000, &idle, false, &ops);
    CHECK(f.preheats == 0);
    /* a big clock step (SNTP arriving) never fires either */
    at_runner_init(&r);
    at_localtime_t u1 = lt_at(mon, 5 * 60), u2 = lt_at(mon, 9 * 60);
    at_runner_tick(&r, tl, 1, &u1, ms += 1000, &idle, false, &ops);
    at_runner_tick(&r, tl, 1, &u2, ms += 1000, &idle, false, &ops);
    CHECK(f.preheats == 0);

    /* polling every second still fires once */
    memset(&f, 0, sizeof(f));
    at_runner_init(&r);
    for (int s = 0; s < 3 * 3600; s++) {
        at_localtime_t t = lt_utc((int64_t)mon * 86400 + 5 * 3600 + s, 0);
        at_runner_tick(&r, tl, 1, &t, ms += 1000, &idle, false, &ops);
    }
    CHECK(f.preheats == 1);

    /* busy heater, fault, stale data, bypass, no data: that firing is skipped, not queued for later */
    const char *why[5] = {"running", "fault", "stale", "bypass", "no data"};
    for (int k = 0; k < 5; k++) {
        memset(&f, 0, sizeof(f));
        at_runner_init(&r);
        at_snapshot_t s = idle;
        if (k == 0) s.status.state_raw = 2;
        if (k == 1) s.status.fault = 4;
        if (k == 2) s.stale = true;
        if (k == 3) s.bypass = true;
        if (k == 4) s.have_status = false;
        for (int m = 6 * 60 + 25; m <= 6 * 60 + 40; m++) {
            at_localtime_t t = lt_at(mon, m);
            at_runner_tick(&r, tl, 1, &t, ms += 60000, k == 0 ? &s : (m < 6 * 60 + 31 ? &s : &idle), false, &ops);
        }
        CHECK(f.preheats == 0);
        CHECK(strstr(r.note, "skipped") != NULL);
        (void)why;
    }
    /* cooling down also counts as busy */
    memset(&f, 0, sizeof(f));
    at_runner_init(&r);
    at_snapshot_t cool0 = mk_snap(15, 4, 0);
    for (int m = 6 * 60 + 25; m <= 6 * 60 + 35; m++) {
        at_localtime_t t = lt_at(mon, m);
        at_runner_tick(&r, tl, 1, &t, ms += 60000, &cool0, false, &ops);
    }
    CHECK(f.preheats == 0);
    /* a refused command (queue full) is not a run */
    memset(&f, 0, sizeof(f));
    f.fail = true;
    at_runner_init(&r);
    for (int m = 6 * 60 + 25; m <= 6 * 60 + 35; m++) {
        at_localtime_t t = lt_at(mon, m);
        at_runner_tick(&r, tl, 1, &t, ms += 60000, &idle, false, &ops);
    }
    CHECK(!r.active);

    /* preheat run: active while the heater works, over when the window ends */
    memset(&f, 0, sizeof(f));
    at_runner_init(&r);
    uint32_t t0 = 5000000;
    at_snapshot_t going = mk_snap(15, 2, 0);
    at_snapshot_t cool = mk_snap(15, 4, 0);
    at_localtime_t tt = lt_at(mon, 6 * 60 + 29);
    at_runner_tick(&r, tl, 1, &tt, t0, &idle, false, &ops);
    tt = lt_at(mon, 6 * 60 + 30);
    at_runner_tick(&r, tl, 1, &tt, t0 + 60000, &idle, false, &ops);
    CHECK(f.preheats == 1 && r.active && !strcmp(r.id, "wk"));
    at_runner_tick(&r, tl, 1, &tt, t0 + 60000 + 30 * 60000, &going, false, &ops);
    CHECK(r.active);
    at_runner_tick(&r, tl, 1, &tt, t0 + 60000 + 59 * 60000, &cool, false, &ops);
    CHECK(r.active); /* cooling down inside the window */
    at_runner_tick(&r, tl, 1, &tt, t0 + 60000 + 60 * 60000, &cool, false, &ops);
    CHECK(!r.active && f.ends == 0);

    /* stopped by hand (or at the coolant limit): the run ends; the timer does not restart in that window */
    memset(&f, 0, sizeof(f));
    at_runner_init(&r);
    tt = lt_at(mon, 6 * 60 + 29);
    at_runner_tick(&r, tl, 1, &tt, t0, &idle, false, &ops);
    tt = lt_at(mon, 6 * 60 + 30);
    at_runner_tick(&r, tl, 1, &tt, t0 + 60000, &idle, false, &ops);
    CHECK(r.active);
    at_runner_tick(&r, tl, 1, &tt, t0 + 60000 + 60000, &idle, false, &ops); /* still within start grace */
    CHECK(r.active);
    at_runner_tick(&r, tl, 1, &tt, t0 + 60000 + 10 * 60000, &idle, false, &ops);
    CHECK(!r.active && f.preheats == 1);
    for (int m = 6 * 60 + 31; m < 7 * 60 + 30; m++) {
        tt = lt_at(mon, m);
        at_runner_tick(&r, tl, 1, &tt, t0 + 11 * 60000 + (uint32_t)(m - 391) * 60000, &idle, false, &ops);
    }
    CHECK(f.preheats == 1);

    /* a manual start inside the window: the timer would find the heater busy */
    memset(&f, 0, sizeof(f));
    at_runner_init(&r);
    for (int m = 6 * 60 + 20; m <= 6 * 60 + 40; m++) {
        tt = lt_at(mon, m);
        at_runner_tick(&r, tl, 1, &tt, ms += 60000, &going, false, &ops);
    }
    CHECK(f.preheats == 0);

    /* thermostat timer: arms Auto with its range, disarms at the end of the window */
    memset(&f, 0, sizeof(f));
    tl[1] = rep("th", 7 * 60 + 30, 60, WEEKDAYS);
    tl[1].mode = AT_TMODE_THERMOSTAT;
    tl[1].low_c = 18;
    tl[1].high_c = 21;
    at_runner_init(&r);
    tt = lt_at(mon, 7 * 60 + 29);
    at_runner_tick(&r, tl + 1, 1, &tt, t0, &idle, false, &ops);
    tt = lt_at(mon, 7 * 60 + 30);
    at_runner_tick(&r, tl + 1, 1, &tt, t0 + 60000, &idle, false, &ops);
    CHECK(f.begins == 1 && f.low == 18 && f.high == 21 && r.active);
    at_runner_tick(&r, tl + 1, 1, &tt, t0 + 20 * 60000, &idle, true, &ops);
    CHECK(r.active && f.ends == 0);
    at_runner_tick(&r, tl + 1, 1, &tt, t0 + 60000 + 60 * 60000, &idle, true, &ops);
    CHECK(!r.active && f.ends == 1 && f.last_disarm == 1);
    /* the user switched Auto off by hand part-way: the run ends, nothing is disarmed, nothing restarts */
    memset(&f, 0, sizeof(f));
    at_runner_init(&r);
    tt = lt_at(mon, 7 * 60 + 29);
    at_runner_tick(&r, tl + 1, 1, &tt, t0, &idle, false, &ops);
    tt = lt_at(mon, 7 * 60 + 30);
    at_runner_tick(&r, tl + 1, 1, &tt, t0 + 60000, &idle, false, &ops);
    at_runner_tick(&r, tl + 1, 1, &tt, t0 + 20 * 60000, &idle, false, &ops);
    CHECK(!r.active && f.ends == 1 && f.last_disarm == 0);
    /* Auto already on when the timer is due: the user's Auto wins */
    memset(&f, 0, sizeof(f));
    at_runner_init(&r);
    tt = lt_at(mon, 7 * 60 + 29);
    at_runner_tick(&r, tl + 1, 1, &tt, t0, &idle, true, &ops);
    tt = lt_at(mon, 7 * 60 + 30);
    at_runner_tick(&r, tl + 1, 1, &tt, t0 + 60000, &idle, true, &ops);
    CHECK(f.begins == 0 && !r.active);

    /* a one-off fires on its date only, and is pruned once its window is over (not while running) */
    memset(&f, 0, sizeof(f));
    tl[2] = once("one", mon + 2, 9 * 60, 30);
    at_runner_init(&r);
    for (int d = 0; d < 5; d++) {
        for (int m = 8 * 60 + 55; m < 9 * 60 + 5; m++) {
            tt = lt_at(mon + d, m);
            at_runner_tick(&r, tl + 2, 1, &tt, ms += 60000, &idle, false, &ops);
        }
    }
    CHECK(f.preheats == 1 && f.last_minutes == 30);
    at_localtime_t during = lt_at(mon + 2, 9 * 60 + 29), after = lt_at(mon + 2, 9 * 60 + 30);
    CHECK(at_timers_prune_expired(tl + 2, 1, &during, NULL) == 1);
    CHECK(at_timers_prune_expired(tl + 2, 1, &after, "one") == 1);
    CHECK(at_timers_prune_expired(tl + 2, 1, &after, NULL) == 0);

    /* two timers: the second one fires after the first has finished and its gap has passed */
    memset(&f, 0, sizeof(f));
    tl[0] = rep("a", 360, 60, MON);
    tl[1] = rep("b", 450, 20, MON);
    at_runner_init(&r);
    uint32_t base = 9000000;
    for (int m = 5 * 60 + 50; m <= 8 * 60; m++) {
        tt = lt_at(mon, m);
        uint32_t now_ms = base + (uint32_t)(m - 350) * 60000;
        const at_snapshot_t *sn = (m >= 361 && m < 420 + 5) ? &going : &idle;
        at_runner_tick(&r, tl, 2, &tt, now_ms, sn, false, &ops);
    }
    CHECK(f.preheats == 2 && f.last_minutes == 20);
}

/* The UK on 2026-03-29 (clocks forward at 01:00 UTC) and 2026-10-25 (clocks back at 01:00 UTC). */
static void test_timer_dst(void)
{
    printf("timer firing across daylight saving\n");
    fake_t f = {0};
    at_timer_ops_t ops = {f_preheat, f_begin, f_end, &f};
    at_snapshot_t idle = mk_snap(15, 0, 0);
    uint32_t ms = 1;
    at_runner_t r;
    at_timer_t tl[2];

    int64_t fwd = (int64_t)at_days_from_civil(2026, 3, 29) * 86400 + 3600;   /* 01:00 UTC */
    /* 01:30 local does not exist that day; 02:30 does */
    tl[0] = rep("skipped", 90, 30, SUN);
    tl[1] = rep("after", 150, 30, SUN);
    at_runner_init(&r);
    for (int64_t u = fwd - 4 * 3600; u < fwd + 4 * 3600; u += 10) {
        at_localtime_t t = lt_utc(u, u < fwd ? 0 : 3600);
        at_runner_tick(&r, tl, 2, &t, ms += 10000, &idle, false, &ops);
    }
    /* the one in the missing hour fires at 02:00 (the new time), the other at 02:30: both run, once each */
    CHECK(f.preheats == 2);

    int64_t back = (int64_t)at_days_from_civil(2026, 10, 25) * 86400 + 3600; /* 01:00 UTC = 02:00 BST -> 01:00 GMT */
    memset(&f, 0, sizeof(f));
    tl[0] = rep("repeated", 90, 30, SUN);   /* 01:30 happens twice */
    tl[1] = rep("later", 180, 30, SUN);
    at_runner_init(&r);
    for (int64_t u = back - 4 * 3600; u < back + 4 * 3600; u += 10) {
        at_localtime_t t = lt_utc(u, u < back ? 3600 : 0);
        at_runner_tick(&r, tl, 2, &t, ms += 10000, &idle, false, &ops);
    }
    CHECK(f.preheats == 2);   /* 01:30 once, 03:00 once */

    /* a run over the autumn change lasts its real length */
    memset(&f, 0, sizeof(f));
    tl[0] = rep("night", 60, 120, SUN);     /* 01:00 BST, 2 hours */
    at_runner_init(&r);
    bool was_active_after_3h = false;
    at_snapshot_t going = mk_snap(15, 2, 0);
    for (int64_t u = back - 3600 - 3600; u < back + 5 * 3600; u += 10) {
        at_localtime_t t = lt_utc(u, u < back ? 3600 : 0);
        at_runner_tick(&r, tl, 1, &t, ms += 10000, t.minute == 60 ? &idle : &going, false, &ops);
        if (u == back + 3 * 3600) was_active_after_3h = r.active;
    }
    CHECK(f.preheats == 1 && !was_active_after_3h && !r.active);

    /* a run across midnight and across Sunday into Monday */
    memset(&f, 0, sizeof(f));
    tl[0] = rep("wrap", 23 * 60, 180, SUN);                                      /* Sun 23:00 -> Mon 02:00 */
    tl[1] = rep("mon", 2 * 60 + 30, 20, MON);                                    /* Mon 02:30 */
    int32_t sun = at_days_from_civil(2026, 11, 1);
    CHECK(at_iso_weekday(sun) == 7);
    at_runner_init(&r);
    bool active_at_0100 = false, active_at_0230 = true;
    for (int m = 22 * 60; m < 24 * 60 + 4 * 60; m++) {
        int32_t d = sun + m / 1440;
        at_localtime_t t = lt_at(d, m % 1440);
        bool fire_minute = t.minute == 23 * 60 || t.minute == 150;
        at_runner_tick(&r, tl, 2, &t, ms += 60000, fire_minute ? &idle : &going, false, &ops);
        if (m == 24 * 60 + 60) active_at_0100 = r.active;
        if (m == 24 * 60 + 2 * 60 + 29) active_at_0230 = r.active;
    }
    CHECK(f.preheats == 2 && active_at_0100 && !active_at_0230);
}

static void test_timer_next(void)
{
    printf("next timer\n");
    int32_t mon = at_days_from_civil(2026, 10, 26);
    at_timer_t tl[3];
    int idx;
    int64_t lm;
    at_localtime_t now = lt_at(mon, 7 * 60);

    tl[0] = rep("wk", 390, 60, WEEKDAYS);
    CHECK(at_sched_next(tl, 1, &now, &idx, &lm) && idx == 0 && lm == (int64_t)(mon + 1) * 1440 + 390);
    now = lt_at(mon, 6 * 60);
    CHECK(at_sched_next(tl, 1, &now, &idx, &lm) && lm == (int64_t)mon * 1440 + 390);
    now = lt_at(mon, 390);   /* due right now: that one is not "next" */
    CHECK(at_sched_next(tl, 1, &now, &idx, &lm) && lm == (int64_t)(mon + 1) * 1440 + 390);
    now = lt_at(mon + 4, 8 * 60);   /* Friday: next is Monday */
    CHECK(at_sched_next(tl, 1, &now, &idx, &lm) && lm == (int64_t)(mon + 7) * 1440 + 390);
    tl[1] = once("one", mon + 6, 5 * 60, 30);
    CHECK(at_sched_next(tl, 2, &now, &idx, &lm) && idx == 1 && lm == (int64_t)(mon + 6) * 1440 + 300);
    tl[1].enabled = false;
    CHECK(at_sched_next(tl, 2, &now, &idx, &lm) && idx == 0);
    tl[0].enabled = false;
    CHECK(!at_sched_next(tl, 2, &now, &idx, &lm));
    tl[0] = rep("sun", 600, 60, SUN);
    now = lt_at(mon + 6, 11 * 60);   /* Sunday, after it ran: a week away */
    CHECK(at_sched_next(tl, 1, &now, &idx, &lm) && lm == (int64_t)(mon + 13) * 1440 + 600);
    at_localtime_t none = {0};
    CHECK(!at_sched_next(tl, 1, &none, &idx, &lm));
}

static void test_tz(void)
{
    printf("time zones\n");
    char out[AT_TZ_MAX];
    CHECK(at_tz_resolve("Europe/London", out, sizeof(out)) && !strcmp(out, "GMT0BST,M3.5.0/1,M10.5.0"));
    CHECK(at_tz_resolve("europe/london", out, sizeof(out)));
    CHECK(at_tz_resolve("GMT0BST,M3.5.0/1,M10.5.0", out, sizeof(out)));
    CHECK(at_tz_resolve("CET-1CEST,M3.5.0,M10.5.0/3", out, sizeof(out)));
    CHECK(at_tz_resolve("<+0530>-5:30", out, sizeof(out)));
    CHECK(at_tz_resolve("EST5", out, sizeof(out)));
    CHECK(at_tz_resolve("PST8PDT,M3.2.0,M11.1.0", out, sizeof(out)));
    CHECK(at_tz_resolve("NZST-12NZDT,M9.5.0,M4.1.0/3", out, sizeof(out)));
    CHECK(!at_tz_resolve("Mars/Olympus", out, sizeof(out)));
    CHECK(!at_tz_resolve("foo", out, sizeof(out)));
    CHECK(!at_tz_resolve("GMT", out, sizeof(out)));
    CHECK(!at_tz_resolve("GMT0BST", out, sizeof(out)));              /* daylight name needs its rules */
    CHECK(!at_tz_resolve("GMT0BST,M3.5.0/1", out, sizeof(out)));
    CHECK(!at_tz_resolve("GMT0BST,M13.5.0,M10.5.0", out, sizeof(out)));
    CHECK(!at_tz_resolve("GMT0 BST", out, sizeof(out)));
    CHECK(!at_tz_resolve("GMT0\n", out, sizeof(out)));
    CHECK(!at_tz_resolve("", out, sizeof(out)) && !at_tz_resolve(NULL, out, sizeof(out)));
    CHECK(!at_tz_resolve("Europe/London", out, 8));                    /* does not fit */
    /* every table entry is itself valid and fits */
    const char *names[] = {"UTC", "Europe/Dublin", "Europe/Lisbon", "Atlantic/Azores", "Europe/Athens", "Europe/Istanbul",
                           "America/New_York", "America/Phoenix", "Asia/Kolkata", "Australia/Adelaide", "Pacific/Auckland",
                           "Atlantic/Reykjavik", "Africa/Johannesburg", "Australia/Perth"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        CHECK(at_tz_resolve(names[i], out, sizeof(out)) && at_tz_valid_posix(out));
    }
}

static void test_version(void)
{
    printf("RoamOS versions and the update check\n");
    CHECK(at_roamos_version_number("0.0001") == 1);
    CHECK(at_roamos_version_number("0.0120") == 120);
    CHECK(at_roamos_version_number("0.1234") == 1234);
    CHECK(at_roamos_version_number("0.9999") == 9999);
    CHECK(at_roamos_version_number("1.0002") == 10002);
    CHECK(at_roamos_version_number("9.9999") == 99999);
    const char *bad[] = {"0.0000", "0.1.0", "0.12", "0.12345", "v0.0001", "10.0001", "0.12a4", " 0.0001", "0.0001 ", "", ".1234", "0,1234"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(at_roamos_version_number(bad[i]) == -1);
    CHECK(at_roamos_version_number(NULL) == -1);

    CHECK(at_update_check("0.0005", "0.0006", false) == AT_UPDATE_OK);
    CHECK(at_update_check("0.0005", "0.0005", false) == AT_UPDATE_OK);      /* reinstalling the same version */
    CHECK(at_update_check("0.9999", "1.0001", false) == AT_UPDATE_OK);
    CHECK(at_update_check("0.0006", "0.0005", false) == AT_UPDATE_OLDER);
    CHECK(at_update_check("1.0001", "0.9999", false) == AT_UPDATE_OLDER);
    CHECK(at_update_check("0.0006", "0.1.0", false) == AT_UPDATE_NOT_ROAMOS);
    CHECK(at_update_check("0.0006", "", false) == AT_UPDATE_NOT_ROAMOS);
    CHECK(at_update_check("0.1.0", "0.0001", false) == AT_UPDATE_OK);       /* pre-RoamOS box: nothing to compare */
    /* the rollback override lets both refusals through */
    CHECK(at_update_check("0.0006", "0.0005", true) == AT_UPDATE_OK);
    CHECK(at_update_check("0.0006", "0.1.0", true) == AT_UPDATE_OK);
}

/* ---- fuel estimate ---------------------------------------------------------- */

static void test_fuel(void)
{
    printf("fuel estimate\n");
    /* levels from the status frame's state/substate */
    CHECK(at_fuel_level(0, 1, false) == AT_FUEL_OFF);
    CHECK(at_fuel_level(2, 1, false) == AT_FUEL_OFF);      /* glow plug warming up: no fuel yet */
    CHECK(at_fuel_level(2, 3, false) == AT_FUEL_STARTING);
    CHECK(at_fuel_level(2, 6, true) == AT_FUEL_STARTING);
    CHECK(at_fuel_level(3, 0, true) == AT_FUEL_LOW);
    CHECK(at_fuel_level(3, 8, true) == AT_FUEL_MIDDLE);
    CHECK(at_fuel_level(3, 2, true) == AT_FUEL_HIGH);
    CHECK(at_fuel_level(3, 7, false) == AT_FUEL_OFF);      /* pump only */
    CHECK(at_fuel_level(3, 5, true) == AT_FUEL_MIDDLE);    /* burning, level unnamed */
    CHECK(at_fuel_level(4, 0, false) == AT_FUEL_OFF);      /* cooldown */
    CHECK(strcmp(at_fuel_level_name(AT_FUEL_HIGH), "high") == 0);

    at_fuel_cfg_t cfg;
    at_fuel_cfg_defaults(&cfg);
    CHECK(at_fuel_cfg_valid(&cfg));
    cfg.price = 1.50f;
    at_fuel_totals_t t;
    at_fuel_totals_init(&t);
    static at_fuel_log_t log;
    at_fuel_log_init(&log);
    at_fuel_run_t run = {0};
    const int32_t day = at_days_from_civil(2026, 10, 27);

    /* idle, then a start: counted once */
    at_fuel_tick(&t, &log, &run, &cfg, 0, 1, false, 3, day);
    CHECK(t.litres == 0 && log.count == 0);
    for (int i = 0; i < 60; i++) at_fuel_tick(&t, &log, &run, &cfg, 2, 3, true, 3, day); /* 3 min starting */
    CHECK(t.starts == 1);
    for (int i = 0; i < 1200; i++) at_fuel_tick(&t, &log, &run, &cfg, 3, 2, true, 3, day); /* 1 h high */
    double want = 0.15 * 3 / 60.0 + 0.62;
    CHECK(fabs(t.litres - want) < 1e-6);
    CHECK(fabs(t.cost - want * 1.50) < 1e-6);
    CHECK(t.level_s[AT_FUEL_HIGH] == 3600 && t.level_s[AT_FUEL_STARTING] == 180);
    const at_fuel_day_t *e = at_fuel_log_find(&log, day);
    CHECK(e && e->starts == 1 && e->burn_min == 63);
    CHECK(e && abs((int)e->ml - (int)(want * 1000)) <= 1);
    CHECK(e && abs((int)e->cost_m - (int)(want * 1500)) <= 1);

    /* cooldown back to a start is a new start; middle via calibration factor */
    at_fuel_tick(&t, &log, &run, &cfg, 4, 0, false, 3, day);
    at_fuel_tick(&t, &log, &run, &cfg, 2, 3, true, 3, day);
    CHECK(t.starts == 2);
    cfg.factor = 2.0f;
    CHECK(fabs(at_fuel_rate_now(&cfg, AT_FUEL_MIDDLE) - 0.70f) < 1e-6);

    /* next day: a new log entry, newest first */
    double before = t.litres;
    for (int i = 0; i < 1200; i++) at_fuel_tick(&t, &log, &run, &cfg, 3, 8, true, 3, day + 1);
    CHECK(fabs(t.litres - before - 0.70) < 1e-6);
    CHECK(log.count == 2 && at_fuel_log_at(&log, 0)->day == day + 1 && at_fuel_log_at(&log, 1)->day == day);
    CHECK(abs((int)at_fuel_log_at(&log, 0)->ml - 700) <= 1);

    /* no clock: totals count, the log does not; a clock that went backwards leaves the log alone */
    at_fuel_tick(&t, &log, &run, &cfg, 3, 2, true, 3, -1);
    at_fuel_tick(&t, &log, &run, &cfg, 3, 2, true, 3, day - 5);
    CHECK(log.count == 2 && at_fuel_log_find(&log, day - 5) == NULL);

    /* the ring keeps the newest AT_FUEL_DAYS days */
    at_fuel_log_init(&log);
    for (int d = 0; d < AT_FUEL_DAYS + 10; d++) at_fuel_tick(&t, &log, &run, &cfg, 3, 2, true, 60, day + d);
    CHECK(log.count == AT_FUEL_DAYS);
    CHECK(at_fuel_log_at(&log, 0)->day == day + AT_FUEL_DAYS + 9);
    CHECK(at_fuel_log_at(&log, AT_FUEL_DAYS - 1)->day == day + 10);
    CHECK(at_fuel_log_at(&log, AT_FUEL_DAYS) == NULL);

    /* the pump's own pulses: counted always, litres only once calibrated */
    {
        at_fuel_totals_t pt;
        at_fuel_totals_init(&pt);
        at_fuel_log_t plog;
        at_fuel_log_init(&plog);
        at_fuel_run_t prun = {0};
        at_fuel_cfg_t pcfg;
        at_fuel_cfg_defaults(&pcfg);
        at_fuel_pump_t pump;
        at_fuel_pump_init(&pump);
        CHECK(at_fuel_pump_valid(&pump));
        for (int i = 0; i < 100; i++) at_fuel_tick_pump(&pt, &plog, &prun, &pcfg, &pump, 3.8f, 3, 2, true, 3, day);
        CHECK(fabs(pump.pulses - 1140.0) < 1e-3);
        CHECK(fabs(pt.litres - 0.62 * 300 / 3600.0) < 1e-6); /* uncalibrated: the table rate */
        CHECK(!at_fuel_pump_calibrate(&pump, 0.0f));

        /* a refill says the 1140 pulses burned 0.0342 l -> 0.03 ml a pulse */
        CHECK(at_fuel_pump_calibrate(&pump, 0.0342f) && fabs(pump.ml_per_pulse - 0.03f) < 1e-6);
        CHECK(fabs(at_fuel_pump_rate_lph(&pump, 3.8f) - 0.4104f) < 1e-4);
        double l0 = pt.litres;
        at_fuel_tick_pump(&pt, &plog, &prun, &pcfg, &pump, 3.8f, 3, 2, true, 10, day);
        CHECK(fabs(pt.litres - l0 - 3.8 * 10 * 0.03 / 1000.0) < 1e-9);

        /* pump stopped while a level is named: nothing; ignition with the pump still: the starting rate;
           priming outside a burn: the pulses */
        l0 = pt.litres;
        at_fuel_tick_pump(&pt, &plog, &prun, &pcfg, &pump, 0.0f, 3, 2, true, 10, day);
        CHECK(pt.litres == l0);
        at_fuel_tick_pump(&pt, &plog, &prun, &pcfg, &pump, 0.0f, 2, 3, true, 3600, day);
        CHECK(fabs(pt.litres - l0 - 0.15) < 1e-6);
        l0 = pt.litres;
        at_fuel_tick_pump(&pt, &plog, &prun, &pcfg, &pump, 5.0f, 0, 4, false, 10, day);
        CHECK(fabs(pt.litres - l0 - 50 * 0.03 / 1000.0) < 1e-9);

        /* extended frames not streaming (< 0): back to the estimate */
        l0 = pt.litres;
        double p0 = pump.pulses;
        at_fuel_tick_pump(&pt, &plog, &prun, &pcfg, &pump, -1.0f, 3, 2, true, 3600, day);
        CHECK(fabs(pt.litres - l0 - 0.62) < 1e-6 && pump.pulses == p0);

        CHECK(!at_fuel_pump_set_ml(&pump, 0.5f) && !at_fuel_pump_set_ml(&pump, -1.0f));
        at_fuel_pump_reset_count(&pump);
        CHECK(pump.pulses == 0 && !at_fuel_pump_calibrate(&pump, 1.0f)); /* too few pulses to say */
    }

    /* the tank: level set by the owner, less the estimate since */
    at_fuel_tank_t tank;
    at_fuel_tank_init(&tank);
    float left;
    CHECK(!at_fuel_tank_remaining(&tank, 10.0, &left));          /* never set */
    CHECK(!at_fuel_tank_set_capacity(&tank, 0) && !at_fuel_tank_set_capacity(&tank, 5000));
    CHECK(at_fuel_tank_set_level(&tank, 150, 10.0, 1800000000));  /* no capacity yet: up to the maximum */
    CHECK(at_fuel_tank_set_capacity(&tank, 80));                  /* level brought down to the new size */
    CHECK(at_fuel_tank_remaining(&tank, 10.0, &left) && fabsf(left - 80) < 1e-4f);
    CHECK(!at_fuel_tank_set_level(&tank, 81, 10.0, 0) && !at_fuel_tank_set_level(&tank, -1, 10.0, 0));
    CHECK(at_fuel_tank_set_level(&tank, 60, 10.0, 1800000000));
    CHECK(at_fuel_tank_remaining(&tank, 12.5, &left) && fabsf(left - 57.5f) < 1e-4f);
    CHECK(at_fuel_tank_remaining(&tank, 100.0, &left) && left == 0); /* never below empty */
    CHECK(tank.set_epoch == 1800000000);

    /* settings limits */
    at_fuel_cfg_defaults(&cfg);
    cfg.factor = 0.1f;
    CHECK(!at_fuel_cfg_valid(&cfg));
    at_fuel_cfg_defaults(&cfg);
    cfg.rate_lph[AT_FUEL_HIGH] = -1;
    CHECK(!at_fuel_cfg_valid(&cfg));
}

int main(void)
{
    test_frames();
    test_framer();
    test_decode();
    test_passthrough();
    test_log_snapshot();
    test_injection();
    test_injection_no_reply();
    test_manual_stop_panel_handoff();
    test_run_mode();
    test_controller_stop_during_hearth_run();
    test_bypass();
    test_standalone();
    test_room_temp_to_heater();
    test_ext_filter_and_pubr0();
    test_strays_not_to_panel();
    test_fault_tracking();
    test_ctl();
    test_timer_rules();
    test_timer_schedule();
    test_timer_dst();
    test_timer_next();
    test_tz();
    test_version();
    test_fuel();
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
