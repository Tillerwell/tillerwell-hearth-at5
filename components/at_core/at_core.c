#include "at_core.h"

#include <string.h>

#define READ_CHUNK 128

/* ---- helpers -------------------------------------------------------------- */

static void log_add(at_core_t *c, uint32_t now, char dir, const uint8_t *data, size_t len, bool crc_ok)
{
    at_log_entry_t *e = &c->log[c->log_seq % AT_LOG_ENTRIES];
    c->log_seq++;
    e->ms = now;
    e->dir = dir;
    e->crc_ok = crc_ok;
    e->full_len = (uint16_t)len;
    e->len = (uint8_t)(len < AT_LOG_BYTES ? len : AT_LOG_BYTES);
    memcpy(e->data, data, e->len);
}

static bool is_known_query_type(uint8_t t)
{
    return t == 0x0F || t == 0x11 || t == 0x06 || t == 0x04;
}

static void cache_reply(at_core_t *c, const uint8_t *raw, size_t len)
{
    if (len <= 4 || len > AT_CACHE_MAX || !is_known_query_type(raw[4])) {
        return;
    }
    int slot = -1;
    for (int i = 0; i < AT_CACHE_N; i++) {
        if (c->cache[i].used && c->cache[i].type == raw[4]) {
            slot = i;
            break;
        }
        if (!c->cache[i].used && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        return;
    }
    c->cache[slot].used = true;
    c->cache[slot].type = raw[4];
    c->cache[slot].len = (uint8_t)len;
    memcpy(c->cache[slot].data, raw, len);
}

static const uint8_t *cache_get(const at_core_t *c, uint8_t type, size_t *len)
{
    for (int i = 0; i < AT_CACHE_N; i++) {
        if (c->cache[i].used && c->cache[i].type == type) {
            *len = c->cache[i].len;
            return c->cache[i].data;
        }
    }
    return NULL;
}

/* Update the status model from a CRC-valid frame. */
static void note_frame(at_core_t *c, uint32_t now, bool from_heater, const uint8_t *raw, size_t len)
{
    if (len < AT_HEADER_LEN + AT_CRC_LEN) {
        return;
    }
    uint8_t dev = raw[1], type = raw[4];
    const uint8_t *p = raw + AT_HEADER_LEN;
    size_t plen = len - AT_HEADER_LEN - AT_CRC_LEN;

    if (from_heater) {
        c->seen_heater = true;
        c->last_heater_ms = now;
        c->frames_heater++;
    } else {
        c->seen_panel = true;
        c->last_panel_ms = now;
        c->frames_panel++;
    }

    if (dev == AT_DEV_HEATER && type == AT_TYPE_STATUS && plen == 18) {
        if (at_decode_status(p, plen, &c->status)) {
            c->have_status = true;
            c->status_ms = now;
            uint8_t f = c->status.fault;
            if (f && (!c->have_last_fault || c->last_fault_code != f)) {
                c->have_last_fault = true;
                c->last_fault_code = f;
                c->last_fault_ms = now;
            }
        }
    } else if (dev == AT_DEV_PANEL && type == AT_TYPE_CABIN_TEMP && plen == 1) {
        c->cabin_temp = p[0];
        c->have_cabin = true;
        c->cabin_ms = now;
    } else if (dev == 0x02 && type == 0x01 && plen >= 2) {
        if (at_decode_ext(p, plen, &c->ext)) {
            c->have_ext = true;
            c->ext_ms = now;
        }
    }
}

static bool panel_silent(const at_core_t *c, uint32_t now)
{
    /* never heard: give a controller that is plugged in the first few seconds after boot to speak */
    return c->seen_panel ? now - c->last_panel_ms >= AT_PANEL_SILENT_MS : now >= AT_PANEL_SILENT_MS;
}

/* ---- relay ---------------------------------------------------------------- */

typedef struct {
    at_core_t *c;
    uint32_t now;
    bool from_heater;
} relay_ctx_t;

static void relay_cb(at_ev_kind_t kind, const uint8_t *data, size_t len, bool crc_ok, void *vctx)
{
    relay_ctx_t *r = vctx;
    at_core_t *c = r->c;
    uint8_t *out = r->from_heater ? c->out_h : c->out_p;
    size_t *outlen = r->from_heater ? &c->out_h_len : &c->out_p_len;

    if (kind == AT_EV_STRAY) {
        if (*outlen + len <= sizeof(c->out_h)) {
            memcpy(out + *outlen, data, len);
            *outlen += len;
        }
        log_add(c, r->now, 'S', data, len, false);
        return;
    }
    if (r->from_heater && crc_ok && at_is_extended_frame(data, len)) {
        log_add(c, r->now, 'F', data, len, true); /* never forwarded: confuses the panel */
    } else {
        if (*outlen + len <= sizeof(c->out_h)) {
            memcpy(out + *outlen, data, len);
            *outlen += len;
        }
        log_add(c, r->now, r->from_heater ? 'H' : 'P', data, len, crc_ok);
    }
    if (crc_ok) {
        note_frame(c, r->now, r->from_heater, data, len);
        if (r->from_heater) {
            cache_reply(c, data, len);
        }
    }
}

static void relay_pass(at_core_t *c, uint32_t now, bool from_heater)
{
    uint8_t buf[READ_CHUNK];
    at_port_t *src = from_heater ? &c->heater : &c->panel;
    at_port_t *dst = from_heater ? &c->panel : &c->heater;
    at_framer_t *fr = from_heater ? &c->fh : &c->fp;
    size_t n = src->read(src->ctx, buf, sizeof(buf));
    if (!n) {
        return;
    }
    relay_ctx_t r = {.c = c, .now = now, .from_heater = from_heater};
    uint8_t *out = from_heater ? c->out_h : c->out_p;
    size_t *outlen = from_heater ? &c->out_h_len : &c->out_p_len;
    *outlen = 0;
    at_framer_feed(fr, buf, n, relay_cb, &r);
    if (*outlen) {
        dst->write(dst->ctx, out, *outlen);
    }
}

/* ---- queue / injection ---------------------------------------------------- */

static void set_last_cmd(at_core_t *c, uint32_t now, const char *note)
{
    c->have_last_cmd = true;
    strncpy(c->last_cmd, note && note[0] ? note : "frame", sizeof(c->last_cmd) - 1);
    c->last_cmd[sizeof(c->last_cmd) - 1] = 0;
    c->last_cmd_ms = now;
    c->last_cmd_replied = false;
}

static void q_pop(at_core_t *c, uint32_t now)
{
    c->q_head = (c->q_head + 1) % AT_QUEUE_LEN;
    c->q_count--;
    c->last_done_ms = now;
    c->have_boundary_wait = false;
}

bool at_core_enqueue(at_core_t *c, const uint8_t *frame, size_t len, uint32_t delay_ms,
                     bool direct, const char *note, uint32_t now_ms)
{
    if (c->bypass || c->q_count >= AT_QUEUE_LEN || len == 0 || len > AT_MAX_FRAME) {
        return false;
    }
    at_queue_item_t *it = &c->q[(c->q_head + c->q_count) % AT_QUEUE_LEN];
    memset(it, 0, sizeof(*it));
    memcpy(it->frame, frame, len);
    it->len = len;
    it->delay_ms = delay_ms;
    it->queued_ms = now_ms;
    it->direct = direct;
    if (note) {
        strncpy(it->note, note, sizeof(it->note) - 1);
    }
    c->q_count++;
    return true;
}

void at_core_set_bypass(at_core_t *c, bool on)
{
    c->bypass = on;
    if (on) {
        /* Drop anything waiting; an injection already in flight (the head
         * item) finishes. */
        int keep = c->state == AT_ST_AWAIT_REPLY ? 1 : 0;
        if (c->q_count > keep) {
            c->q_count = keep;
        }
    }
}

typedef struct {
    at_core_t *c;
    uint32_t now;
    bool got_reply;
} inj_ctx_t;

/* Panel frames seen while diverted: answer from the reply cache. */
static void panel_query_cb(at_ev_kind_t kind, const uint8_t *data, size_t len, bool crc_ok, void *vctx)
{
    inj_ctx_t *x = vctx;
    at_core_t *c = x->c;
    if (kind != AT_EV_FRAME || !crc_ok || len <= 4) {
        return;
    }
    note_frame(c, x->now, false, data, len);
    log_add(c, x->now, 'P', data, len, true);
    size_t clen = 0;
    const uint8_t *reply;
    if (data[4] == AT_TYPE_STATUS) {
        reply = c->cached_status;
        clen = c->cached_status_len;
    } else {
        reply = cache_get(c, data[4], &clen);
    }
    if (!reply || !clen) {
        return; /* unknown query type: left unanswered for this brief window */
    }
    c->panel.write(c->panel.ctx, reply, clen);
    log_add(c, x->now, 'C', reply, clen, true);
}

static void service_panel(at_core_t *c, uint32_t now)
{
    uint8_t buf[READ_CHUNK];
    size_t n = c->panel.read(c->panel.ctx, buf, sizeof(buf));
    if (!n) {
        return;
    }
    inj_ctx_t x = {.c = c, .now = now};
    at_framer_feed(&c->ip, buf, n, panel_query_cb, &x);
}

static void heater_reply_cb(at_ev_kind_t kind, const uint8_t *data, size_t len, bool crc_ok, void *vctx)
{
    inj_ctx_t *x = vctx;
    at_core_t *c = x->c;
    if (kind == AT_EV_STRAY) {
        log_add(c, x->now, 'S', data, len, false);
        return;
    }
    if (crc_ok) {
        note_frame(c, x->now, true, data, len);
    }
    size_t plen = len >= 7 ? len - 7 : 0;
    /* A non-empty frame of a display query type can only be the reply to a
     * query the panel itself sent just before the divert: not ours. */
    if (crc_ok && plen > 0 && len > 4 && is_known_query_type(data[4])) {
        log_add(c, x->now, 'F', data, len, true);
        return;
    }
    log_add(c, x->now, 'R', data, len, crc_ok);
    if (crc_ok && !x->got_reply) {
        x->got_reply = true;
    }
}

static void service_heater(at_core_t *c, uint32_t now, inj_ctx_t *x)
{
    uint8_t buf[READ_CHUNK];
    size_t n = c->heater.read(c->heater.ctx, buf, sizeof(buf));
    if (!n) {
        return;
    }
    x->now = now;
    at_framer_feed(&c->ih, buf, n, heater_reply_cb, x);
}

static void finish_injection(at_core_t *c, uint32_t now, bool replied)
{
    c->last_cmd_replied = replied;
    if (replied) {
        c->inject_ok_count++;
    } else {
        c->inject_fail_count++;
    }
    c->state = AT_ST_RELAY;
    q_pop(c, now);
}

static void try_start_injection(at_core_t *c, uint32_t now)
{
    if (c->q_count == 0) {
        return;
    }
    at_queue_item_t *it = &c->q[c->q_head];
    if (!it->armed) {
        uint32_t base = it->queued_ms > c->last_done_ms ? it->queued_ms : c->last_done_ms;
        if (it->delay_ms == 0) {
            base = it->queued_ms;
        }
        it->not_before_ms = base + it->delay_ms;
        it->armed = true;
    }
    if ((int32_t)(now - it->not_before_ms) < 0) {
        return;
    }
    if (c->bypass) {
        q_pop(c, now);
        return;
    }

    if (it->direct) {
        bool quiet = (!c->seen_panel || now - c->last_panel_ms >= AT_QUIET_GAP_MS) &&
                     (!c->seen_heater || now - c->last_heater_ms >= AT_QUIET_GAP_MS) &&
                     at_framer_empty(&c->fh) && at_framer_empty(&c->fp);
        if (!quiet && now - it->not_before_ms < AT_QUIET_MAX_WAIT_MS) {
            return;
        }
        c->heater.write(c->heater.ctx, it->frame, it->len);
        log_add(c, now, 'I', it->frame, it->len, true);
        set_last_cmd(c, now, it->note);
        c->last_cmd_replied = true; /* fire-and-forget: nothing to wait for */
        q_pop(c, now);
        return;
    }

    size_t slen = 0;
    const uint8_t *st = cache_get(c, AT_TYPE_STATUS, &slen);
    if (!st) {
        if (now - it->not_before_ms > AT_INJECT_START_TIMEOUT_MS) {
            log_add(c, now, 'F', it->frame, it->len, true);
            c->inject_fail_count++;
            q_pop(c, now);
        }
        return;
    }
    if (!at_framer_empty(&c->fh) || !at_framer_empty(&c->fp)) {
        if (!c->have_boundary_wait) {
            c->have_boundary_wait = true;
            c->boundary_wait_start = now;
        }
        if (now - c->boundary_wait_start < AT_BOUNDARY_WAIT_MS) {
            return;
        }
    }

    /* Divert. */
    memcpy(c->cached_status, st, slen);
    c->cached_status_len = slen;
    at_framer_init(&c->ih);
    at_framer_init(&c->ip);
    uint8_t scratch[READ_CHUNK];
    while (c->heater.read(c->heater.ctx, scratch, sizeof(scratch))) {
        /* anything already buffered predates this injection: discard */
    }
    service_panel(c, now);
    c->heater.write(c->heater.ctx, it->frame, it->len);
    log_add(c, now, 'I', it->frame, it->len, true);
    set_last_cmd(c, now, it->note);
    c->cur_deadline_ms = now + AT_INJECT_REPLY_TIMEOUT_MS;
    c->state = AT_ST_AWAIT_REPLY;
}

/* ---- standalone ----------------------------------------------------------- */

static bool cabin_source(const at_core_t *c, uint32_t now, int *t)
{
    if (c->have_room && now - c->room_ms < AT_ROOM_FRESH_MS) {
        *t = c->room_temp;
    } else if (c->have_cabin) {
        *t = c->cabin_temp;
    } else {
        return false;
    }
    if (*t < 0) *t = 0; /* one unsigned byte on the wire */
    if (*t > 99) *t = 99;
    return true;
}

/* No controller talking: ask the heater for its status ourselves, as the controller would. */
static void standalone_poll(at_core_t *c, uint32_t now)
{
    if (c->bypass || !panel_silent(c, now) || !at_framer_empty(&c->fh)) {
        return;
    }
    size_t slen;
    if (c->q_count && cache_get(c, AT_TYPE_STATUS, &slen)) {
        return; /* a command is about to go: leave the heater line to it */
    }
    if (c->polled && now - c->last_poll_ms < AT_OWN_POLL_MS) {
        return;
    }
    uint8_t f[AT_MAX_FRAME];
    size_t n;
    int t;
    if (cabin_source(c, now, &t) && (!c->cabin_sent || now - c->last_cabin_sent_ms >= AT_OWN_CABIN_MS)) {
        uint8_t b = (uint8_t)t;
        n = at_build_frame(AT_DEV_PANEL, AT_TYPE_CABIN_TEMP, &b, 1, f);
        c->cabin_sent = true;
        c->last_cabin_sent_ms = now;
    } else {
        n = at_build_frame(AT_DEV_PANEL, AT_TYPE_STATUS, NULL, 0, f);
    }
    c->heater.write(c->heater.ctx, f, n);
    log_add(c, now, 'O', f, n, true);
    c->polled = true;
    c->last_poll_ms = now;
    c->own_polls++;
}

void at_core_set_room_temp(at_core_t *c, float celsius, uint32_t now)
{
    c->have_room = true;
    c->room_temp = (int)(celsius + (celsius < 0 ? -0.5f : 0.5f));
    c->room_ms = now;
}

void at_core_init(at_core_t *c, at_port_t heater, at_port_t panel)
{
    memset(c, 0, sizeof(*c));
    c->heater = heater;
    c->panel = panel;
    at_framer_init(&c->fh);
    at_framer_init(&c->fp);
    at_framer_init(&c->ih);
    at_framer_init(&c->ip);
}

void at_core_step(at_core_t *c, uint32_t now)
{
    if (c->state == AT_ST_RELAY) {
        relay_pass(c, now, true);
        relay_pass(c, now, false);
        try_start_injection(c, now);
        if (c->state == AT_ST_RELAY) {
            standalone_poll(c, now);
        }
        return;
    }

    /* AT_ST_AWAIT_REPLY: we are the only reader/writer of both ports. */
    inj_ctx_t x = {.c = c, .now = now, .got_reply = false};
    service_panel(c, now);
    service_heater(c, now, &x);
    if (x.got_reply) {
        finish_injection(c, now, true);
    } else if ((int32_t)(now - c->cur_deadline_ms) >= 0) {
        finish_injection(c, now, false);
    }
}

/* ---- commands ------------------------------------------------------------- */

static bool send_panel_frame(at_core_t *c, uint8_t type, const uint8_t *payload, size_t plen,
                             uint32_t delay_ms, const char *note, uint32_t now)
{
    uint8_t f[AT_MAX_FRAME];
    size_t n = at_build_frame(AT_DEV_PANEL, type, payload, plen, f);
    return n && at_core_enqueue(c, f, n, delay_ms, false, note, now);
}

bool at_cmd_start_preheat(at_core_t *c, int minutes, uint32_t now)
{
    if (minutes < 0) minutes = 0;
    if (minutes > 600) minutes = 600;
    static const uint8_t marker[2] = {0x00, 0x1E};
    if (!send_panel_frame(c, AT_TYPE_START, marker, 2, 0, "preheat", now)) {
        return false;
    }
    uint8_t dur[2] = {(uint8_t)(minutes >> 8), (uint8_t)(minutes & 0xFF)};
    send_panel_frame(c, AT_TYPE_PREHEAT_DUR, dur, 2, 1500, "preheat-time", now);
    return true;
}

bool at_cmd_set_preheat_duration(at_core_t *c, int minutes, uint32_t now)
{
    if (minutes < 1) minutes = 1;
    if (minutes > 600) minutes = 600;
    uint8_t dur[2] = {(uint8_t)(minutes >> 8), (uint8_t)(minutes & 0xFF)};
    return send_panel_frame(c, AT_TYPE_PREHEAT_DUR, dur, 2, 0, "preheat-time", now);
}

/* The start payload is a run time in minutes: 00 22 is a 34-minute run that the panel shows as preheating.
 * ff fb is what the panel itself sends for thermostat mode, run indefinitely. */
bool at_cmd_start_thermostat(at_core_t *c, uint32_t now)
{
    static const uint8_t m[2] = {0xFF, 0xFB};
    return send_panel_frame(c, AT_TYPE_START, m, 2, 0, "thermostat", now);
}

bool at_cmd_stop(at_core_t *c, uint32_t now)
{
    return send_panel_frame(c, AT_TYPE_STOP, NULL, 0, 0, "stop", now);
}

bool at_cmd_start_pump(at_core_t *c, uint32_t now)
{
    static const uint8_t m[2] = {0x00, 0x28};
    return send_panel_frame(c, AT_TYPE_PUMP, m, 2, 0, "pump", now);
}

bool at_cmd_send_pubr0(at_core_t *c, uint32_t now)
{
    uint8_t f[AT_PUBR0_LEN];
    at_build_pubr0(f);
    return at_core_enqueue(c, f, sizeof(f), 0, true, "PUBR0", now);
}

/* ---- snapshot ------------------------------------------------------------- */

void at_core_snapshot(const at_core_t *c, uint32_t now, at_snapshot_t *s)
{
    memset(s, 0, sizeof(*s));
    s->have_status = c->have_status;
    if (c->have_status) {
        s->status = c->status;
        s->state_name = at_state_name(c->status.state_raw);
        s->fault_name = c->status.fault ? at_fault_name(c->status.fault) : NULL;
        s->status_age_ms = now - c->status_ms;
    }
    s->have_cabin = c->have_cabin;
    if (c->have_cabin) {
        s->cabin_temp = c->cabin_temp;
        s->cabin_age_ms = now - c->cabin_ms;
    }
    s->standalone = panel_silent(c, now);
    s->own_polls = c->own_polls;
    /* With no controller there is no cabin reading to go stale; the heater's status is what counts. */
    s->stale = !c->have_status || s->status_age_ms > AT_STALE_AFTER_MS ||
               (!s->standalone && (!c->have_cabin || s->cabin_age_ms > AT_STALE_AFTER_MS));
    s->have_ext = c->have_ext;
    if (c->have_ext) {
        s->ext = c->ext;
        s->ext_active = now - c->ext_ms <= AT_EXT_STALE_AFTER_MS;
    }
    s->have_last_fault = c->have_last_fault;
    if (c->have_last_fault) {
        s->last_fault_code = c->last_fault_code;
        s->last_fault_name = at_fault_name(c->last_fault_code);
        s->last_fault_ago_ms = now - c->last_fault_ms;
    }
    s->have_last_cmd = c->have_last_cmd;
    if (c->have_last_cmd) {
        memcpy(s->last_cmd, c->last_cmd, sizeof(s->last_cmd));
        s->last_cmd_ago_ms = now - c->last_cmd_ms;
        s->last_cmd_replied = c->last_cmd_replied;
    }
    s->bypass = c->bypass;
    s->queue_len = c->q_count;
    s->frames_panel = c->frames_panel;
    s->frames_heater = c->frames_heater;
    s->bad_crc_heater = c->fh.bad_crc;
    s->bad_crc_panel = c->fp.bad_crc;
    s->resyncs_heater = c->fh.resyncs;
    s->resyncs_panel = c->fp.resyncs;
    s->inject_ok = c->inject_ok_count;
    s->inject_fail = c->inject_fail_count;
    s->seen_panel = c->seen_panel;
    s->seen_heater = c->seen_heater;
}

int at_core_log_copy(const at_core_t *c, at_log_entry_t *out, int max)
{
    uint32_t total = c->log_seq;
    uint32_t count = total < AT_LOG_ENTRIES ? total : AT_LOG_ENTRIES;
    if ((int)count > max) {
        count = (uint32_t)max;
    }
    uint32_t first = total - count;
    for (uint32_t i = 0; i < count; i++) {
        out[i] = c->log[(first + i) % AT_LOG_ENTRIES];
    }
    return (int)count;
}
