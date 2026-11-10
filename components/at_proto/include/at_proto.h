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
#define AT_TYPE_PUMP        0x21  /* water (circulation) pump only, not the fuel pump */
/* From the vendor test tool (docs/PROTOCOL.md, "Vendor test utility decoded"). */
#define AT_TYPE_VERSION     0x06  /* device type and firmware version, 4 bytes */
#define AT_TYPE_DIAG        0x07  /* 01 starts the periodic parameter stream, 00 stops it */
#define AT_TYPE_EEPROM      0x0B  /* read the operation report: 00 <address> 40 = 64 bytes */
#define AT_TYPE_UNLOCK      0x0D  /* clear the heater's lockout (state 1.1, "locked") */
#define AT_TYPE_SETTINGS    0x20  /* the heater's settings block: empty reads it (the controller does at power-up),
                                     the block itself writes it */
#define AT_TYPE_FAN_TEST    0x08  /* spin the blower without lighting; the tool sends 60 */
#define AT_TYPE_FUEL_PUMP   0x12  /* run the fuel pump without lighting: priming after the tank ran dry */
#define AT_SETTINGS_WAIT_TEMP_MIN 20
#define AT_SETTINGS_WAIT_TEMP_MAX 95

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
    float supply_v; /* byte [6] / 10: supply voltage, to be confirmed against the extended frame (docs/PROTOCOL.md) */
    uint8_t elapsed_min;
    uint8_t elapsed_sec;
    bool burner_active;
} at_status_t;

bool at_decode_status(const uint8_t *payload, size_t len, at_status_t *out);
/* "idle", "running", "late-run", "cooldown", "final-shutdown" or NULL. */
const char *at_state_name(uint8_t state_raw);
/* The vendor profile's name for a state/substate pair ("High", "Ignition 1", ...), never NULL. */
const char *at_mode_name(uint8_t state_raw, uint8_t substate);
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

/* The settings block (type 0x20 reply, 9 bytes, 10 on some firmware), Binar-5S / Flow 5 layout from the vendor
 * tool's profile. The controller's reply on our heater: 58 00 55 01 28 00 00 00 00. */
typedef struct {
    int wait_temp;          /* a1: coolant temperature at which the heater stops burning and waits, °C (20-95) */
    int warmup_mode;        /* a2: engine warm-up, 0 off, 1 auto, 2 manual */
    int warmup_wait_temp;   /* a3: wait temperature in warm-up, °C (80-95) */
    bool relay_auto;        /* a4: 0 auto, 1 off */
    int relay_on_temp;      /* a5: relay on temperature, °C (30-60) */
    bool pump_impulse;      /* a6: water pump signal, 0 constant, 1 impulse */
    bool pump_with_engine;  /* a7: water pump while the engine runs, 0 off, 1 on */
    bool pump_standby;      /* a8: water pump in standby, 0 on, 1 off */
    bool signal_channel;    /* a9: signalling channel, 0 on, 1 off */
    int extra;              /* a10, unlabelled in the tool, or -1 when the block has 9 bytes */
} at_heater_settings_t;

#define AT_SETTINGS_MIN 9
bool at_decode_heater_settings(const uint8_t *payload, size_t len, at_heater_settings_t *out);

/* The operation report: the heater's 256-byte EEPROM, read as four 64-byte pages. Offsets are the vendor
 * profile's (its aN is byte N-1 here). Lifetime counters, read only. */
#define AT_EEPROM_PAGES     4
#define AT_EEPROM_PAGE_LEN  64
#define AT_EEPROM_LEN       (AT_EEPROM_PAGES * AT_EEPROM_PAGE_LEN)
typedef struct {
    uint16_t product_code;
    uint32_t serial;
    uint16_t starts, starts_ok, starts_fault;
    uint16_t first_attempt, second_attempt;
    uint16_t no_ignition;       /* starts that used up their ignition attempts */
    uint8_t power_cuts;         /* supply lost while running */
    uint16_t water_pump_runs, warmup_runs;
    uint32_t total_s, high_s, middle_s, low_s, wait_s, pump_only_s;
    uint8_t last_fault_column;
} at_heater_report_t;

void at_decode_heater_report(const uint8_t *eeprom, at_heater_report_t *out);

/* The fault history in the same report: 10 entries of 17 bytes from byte 55, and the report's "last fault column"
 * (1-10) is the newest. Layout from the vendor profile's fault table; not yet checked on a real heater. */
#define AT_FAULT_HISTORY_N 10
typedef struct {
    uint8_t code;
    uint8_t repeats;
    uint8_t state, substate;    /* the stage and mode it happened in, as in the status frame */
    uint16_t run_s;             /* total work time, s */
    uint16_t mode_s;            /* time in that mode, s */
    float voltage;
    int flame_temp, liquid_temp, overheat_temp, board_temp;
    uint8_t second_attempts;
    uint8_t flame_breaks;
} at_heater_fault_t;

/* Fills out newest first, skipping empty entries (code 0); returns how many. */
int at_decode_heater_faults(const uint8_t *eeprom, at_heater_fault_t *out, int max);

#ifdef __cplusplus
}
#endif
