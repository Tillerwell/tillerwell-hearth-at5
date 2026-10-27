#include "at_proto.h"

#include <math.h>
#include <string.h>

static uint16_t s_table[256];
static bool s_table_ready;

static void make_table(void)
{
    for (int i = 0; i < 256; i++) {
        uint16_t crc = (uint16_t)i;
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
        s_table[i] = crc;
    }
    s_table_ready = true;
}

uint16_t at_crc16(const uint8_t *data, size_t len)
{
    if (!s_table_ready) {
        make_table();
    }
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)((crc >> 8) ^ s_table[(crc ^ data[i]) & 0xFF]);
    }
    return crc;
}

size_t at_build_frame(uint8_t dev, uint8_t type, const uint8_t *payload, size_t plen, uint8_t *out)
{
    if (plen > AT_MAX_PAYLOAD) {
        return 0;
    }
    out[0] = AT_SOF;
    out[1] = dev;
    out[2] = (uint8_t)(plen & 0xFF);
    out[3] = (uint8_t)(plen >> 8);
    out[4] = type;
    if (plen) {
        memcpy(out + AT_HEADER_LEN, payload, plen);
    }
    uint16_t crc = at_crc16(out, AT_HEADER_LEN + plen);
    out[AT_HEADER_LEN + plen] = (uint8_t)(crc >> 8);
    out[AT_HEADER_LEN + plen + 1] = (uint8_t)(crc & 0xFF);
    return AT_HEADER_LEN + plen + AT_CRC_LEN;
}

size_t at_build_pubr0(uint8_t *out)
{
    static const uint8_t body[13] = {AT_SOF, 'P', 'U', 'B', 'R', '0', 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(out, body, sizeof(body));
    uint16_t crc = at_crc16(body, sizeof(body));
    out[13] = (uint8_t)(crc & 0xFF); /* least-significant byte first */
    out[14] = (uint8_t)(crc >> 8);
    return AT_PUBR0_LEN;
}

bool at_is_extended_frame(const uint8_t *raw, size_t len)
{
    return len > 4 && raw[1] == 0x02 && raw[4] == 0x01;
}

/* ---- framer ------------------------------------------------------------- */

static bool known_dev(uint8_t d)
{
    return d == 0x00 || d == 0x02 || d == 0x03 || d == 0x04;
}

void at_framer_init(at_framer_t *f)
{
    memset(f, 0, sizeof(*f));
}

static void slip(at_framer_t *f, at_framer_cb cb, void *ctx)
{
    f->resyncs++;
    cb(AT_EV_STRAY, f->buf, 1, false, ctx);
    memmove(f->buf, f->buf + 1, f->len - 1);
    f->len--;
}

/* Parse as much of the buffer as possible. */
static void drain(at_framer_t *f, at_framer_cb cb, void *ctx)
{
    for (;;) {
        size_t start = 0;
        while (start < f->len && f->buf[start] != AT_SOF) {
            start++;
        }
        if (start == f->len) {
            if (f->len) {
                cb(AT_EV_STRAY, f->buf, f->len, false, ctx);
                f->dropped += (uint32_t)f->len;
                f->len = 0;
            }
            return;
        }
        if (start > 0) {
            cb(AT_EV_STRAY, f->buf, start, false, ctx);
            f->dropped += (uint32_t)start;
            memmove(f->buf, f->buf + start, f->len - start);
            f->len -= start;
        }
        if (f->len < AT_HEADER_LEN) {
            return;
        }
        size_t plen = (size_t)f->buf[2] | ((size_t)f->buf[3] << 8);
        if (plen > AT_MAX_PAYLOAD) {
            slip(f, cb, ctx);
            continue;
        }
        size_t total = AT_HEADER_LEN + plen + AT_CRC_LEN;
        if (f->len < total) {
            return;
        }
        uint16_t crc = at_crc16(f->buf, total - AT_CRC_LEN);
        bool ok = f->buf[total - 2] == (uint8_t)(crc >> 8) && f->buf[total - 1] == (uint8_t)(crc & 0xFF);
        if (ok) {
            cb(AT_EV_FRAME, f->buf, total, true, ctx);
            memmove(f->buf, f->buf + total, f->len - total);
            f->len -= total;
        } else {
            if (known_dev(f->buf[1])) {
                f->bad_crc++;
                cb(AT_EV_FRAME, f->buf, total, false, ctx);
            }
            slip(f, cb, ctx);
        }
    }
}

void at_framer_feed(at_framer_t *f, const uint8_t *data, size_t n, at_framer_cb cb, void *ctx)
{
    while (n) {
        size_t room = sizeof(f->buf) - f->len;
        size_t take = n < room ? n : room;
        memcpy(f->buf + f->len, data, take);
        f->len += take;
        data += take;
        n -= take;
        drain(f, cb, ctx);
    }
}

/* ---- status ------------------------------------------------------------- */

bool at_decode_status(const uint8_t *p, size_t len, at_status_t *out)
{
    if (len < 18) {
        return false;
    }
    out->state_raw = p[0];
    out->substate = p[1];
    out->fault = p[2];
    out->coolant_temp = ((float)p[3] + (float)p[4]) / 2.0f;
    out->supply_v = (float)p[6] / 10.0f;
    out->elapsed_min = p[9];
    out->elapsed_sec = p[11];
    out->burner_active = p[12] == 0xFF;
    return true;
}

const char *at_state_name(uint8_t s)
{
    switch (s) {
    case 0x00: return "idle";
    case 0x02: return "running";
    case 0x03: return "late-run";
    case 0x04: return "cooldown";
    case 0x05: return "final-shutdown";
    default: return NULL;
    }
}

const char *at_fault_name(uint8_t code)
{
    switch (code) {
    case 0: return "No faults";
    case 1: return "Overheat";
    case 2: return "Possible overheat";
    case 3: return "Overheat";
    case 4: return "Liquid temperature sensor";
    case 5: return "Flame temperature sensor";
    case 6: return "Board temperature sensor";
    case 9: return "Malfunction of a glow plug";
    case 10: return "Turnover mismatch";
    case 12: return "Increased supply voltage";
    case 13: return "No ignition";
    case 14: return "Faulty water pump";
    case 15: return "Low voltage";
    case 16: return "Blowing time exceeded";
    case 17: return "Faulty fuel pump";
    case 20: return "No connection";
    case 22: return "Faulty fuel pump";
    case 24: return "Temperature sensor off-scale";
    case 25: return "Temperature growing too fast";
    case 26: return "Fan overloaded";
    case 27: return "Fan. No rotation";
    case 28: return "Fan. Autorotation";
    case 29: return "Flame breaks too often";
    case 30: return "No connection";
    case 37: return "Overheat locking";
    case 78: return "Flame break during running";
    default: return NULL;
    }
}

/* Vendor "Mode of operation" table for AUTOTERM FLOW 5 / BINAR-5S, indexed by
 * state*10 + substate. */
static const char *const FLOW5_MODES[] = {
    "unknown", "waiting for a command", "cooling the flame sensor", "air blowing", "fuel pumping",
    "unknown", "unknown", "unknown", "unknown", "unknown",
    "waiting for temperature reduction", "locked", "unknown", "unknown", "unknown",
    "unknown", "unknown", "unknown", "unknown", "unknown",
    "cooling", "glow plug warming up", "preparation for ignition", "Ignition 1", "Ignition 2",
    "blowing", "combustion chamber heating", "blowing", "unknown", "unknown",
    "low", "unknown", "High", "unknown", "blowing",
    "waiting", "blowing", "pump only", "middle", "unknown",
    "blowing", "blowing", "blowing", "shutting down",
};
#define FLOW5_MODES_N (sizeof(FLOW5_MODES) / sizeof(FLOW5_MODES[0]))

const char *at_mode_name(uint8_t state, uint8_t substate)
{
    size_t code = (size_t)state * 10 + substate;
    return substate < 10 && code < FLOW5_MODES_N ? FLOW5_MODES[code] : "unknown";
}

bool at_decode_ext(const uint8_t *p, size_t len, at_ext_t *o)
{
    if (len < AT_EXT_MIN_PAYLOAD) {
        return false;
    }
    o->state_raw = p[0];
    o->substate = p[1];
    o->mode_code = p[0] * 10 + p[1];
    o->mode_name = (size_t)o->mode_code < FLOW5_MODES_N ? FLOW5_MODES[o->mode_code] : "unknown";
    o->flame_temp_c = p[17] * 256 + p[18] - 273;
    o->liquid_temp_c = p[19];
    o->overheat_temp_c = p[20];
    o->board_temp_c = p[21];
    o->running_time_s = ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];
    o->defined_rev = p[11];
    o->measured_rev = p[12];
    o->glow_plug = p[13] > 0;
    o->fuel_pump_hz = roundf((float)p[15]) / 10.0f;
    o->voltage = (float)(p[22] * 256 + p[23]) / 10.0f;
    o->fault_code = p[36];
    o->engine_state = p[51];
    o->relay_state = p[52];
    o->fan_current_ma = (uint16_t)(p[54] * 256 + p[55]);
    float pct = o->fuel_pump_hz / 4.2f * 100.0f;
    o->output_pct = (int)lroundf(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
    return true;
}
