/*
 * Autoterm UART protocol primitives: CRC, frame building, incremental frame
 * splitting, and decoding of the heater's status frames.
 *
 * C port of autoterm_protocol.py / autoterm_addon.py from
 * https://github.com/sinise/autoterm-heater-control (MIT). See
 * docs/PROTOCOL.md for the protocol derivation.
 *
 * Frame:  AA | dev | len (u16 LE) | type | payload[len] | crc16 (big-endian)
 * CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF) over everything before
 * the CRC, including the leading 0xAA.
 *
 * Pure C, no ESP-IDF dependency, so it builds and is unit-tested on a host.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_SOF          0xAA
#define AT_DEV_PANEL    0x03
#define AT_DEV_HEATER   0x04
#define AT_MAX_PAYLOAD  250
#define AT_HEADER_LEN   5
#define AT_CRC_LEN      2
#define AT_MAX_FRAME    (AT_HEADER_LEN + AT_MAX_PAYLOAD + AT_CRC_LEN)

#define AT_TYPE_STATUS      0x0F  /* heater status poll / reply */
#define AT_TYPE_CABIN_TEMP  0x11  /* panel's cabin temperature report */
#define AT_TYPE_START       0x01
#define AT_TYPE_PREHEAT_DUR 0x02
#define AT_TYPE_STOP        0x03
#define AT_TYPE_PUMP        0x21

uint16_t at_crc16(const uint8_t *data, size_t len);

/* Build a complete frame into out (needs AT_HEADER_LEN + plen + AT_CRC_LEN
 * bytes). Returns the frame length, or 0 if plen > AT_MAX_PAYLOAD. */
size_t at_build_frame(uint8_t dev, uint8_t type, const uint8_t *payload, size_t plen, uint8_t *out);

/* The vendor diagnostic tool's literal "PUBR0" handshake (15 bytes). Note its
 * CRC is transmitted least-significant byte first, unlike every other frame. */
#define AT_PUBR0_LEN 15
size_t at_build_pubr0(uint8_t *out);

/* True for the extended-telemetry frame (dev 0x02, type 0x01) that the
 * physical panel must never see. raw is a complete frame. */
bool at_is_extended_frame(const uint8_t *raw, size_t len);

/* ---- incremental framer ------------------------------------------------- */

typedef enum { AT_EV_STRAY = 0, AT_EV_FRAME = 1 } at_ev_kind_t;

/* Called for each stray run of bytes (crc_ok unused) and each complete frame.
 * Frames with a bad CRC are only reported when the dev byte is a known one. */
typedef void (*at_framer_cb)(at_ev_kind_t kind, const uint8_t *data, size_t len, bool crc_ok, void *ctx);

typedef struct {
    uint8_t buf[AT_MAX_FRAME * 2];
    size_t len;
    uint32_t resyncs, dropped, bad_crc;
} at_framer_t;

void at_framer_init(at_framer_t *f);
void at_framer_feed(at_framer_t *f, const uint8_t *data, size_t n, at_framer_cb cb, void *ctx);
/* True if no partial frame is buffered (bus is between frames). */
static inline bool at_framer_empty(const at_framer_t *f) { return f->len == 0; }

/* ---- status decoding ---------------------------------------------------- */

/* 18-byte payload of dev 0x04, type 0x0F. */
typedef struct {
    uint8_t state_raw;
    uint8_t substate;
    uint8_t fault;
    float coolant_temp;
    uint8_t elapsed_min;
    uint8_t elapsed_sec;
    bool burner_active;
} at_status_t;

bool at_decode_status(const uint8_t *payload, size_t len, at_status_t *out);
/* "idle", "running", "late-run", "cooldown", "final-shutdown" or NULL. */
const char *at_state_name(uint8_t state_raw);
/* Fault name from the Flow 5 / BINAR-5S table, or NULL if not in the table. */
const char *at_fault_name(uint8_t code);

/* Extended telemetry (dev 0x02, type 0x01, 58 bytes). Flow 5 / BINAR-5S layout
 * only, the one profile confirmed against real hardware. */
typedef struct {
    uint8_t state_raw, substate;
    int mode_code;              /* state*10 + substate */
    const char *mode_name;      /* never NULL */
    int flame_temp_c, liquid_temp_c, overheat_temp_c, board_temp_c;
    uint32_t running_time_s;
    uint8_t defined_rev, measured_rev;
    bool glow_plug;
    float fuel_pump_hz;
    float voltage;
    uint8_t fault_code;
    uint8_t engine_state, relay_state;
    uint16_t fan_current_ma;
    int output_pct;             /* derived: fuel pump Hz / 4.2 * 100, clamped */
} at_ext_t;

#define AT_EXT_MIN_PAYLOAD 56
bool at_decode_ext(const uint8_t *payload, size_t len, at_ext_t *out);

#ifdef __cplusplus
}
#endif
