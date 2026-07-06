/*
 * hypex_proto — Hypex FusionAmp (FA503 / DSP3) USB HID protocol packets.
 *
 * Pure packet building/parsing. No I/O, no OS dependencies — compiles on the
 * host for unit testing (see ../../host_tests/) and on ESP-IDF.
 *
 * Protocol reference: knob/docs/HypexUsbProtocol.md (every field there is
 * tagged VERIFIED / DOC / GUESS; this file only decodes fields at VERIFIED
 * or clearly-marked-provisional level).
 *
 * SAFETY (see "Safe-Opcode Policy" in knob/docs/experiments.md):
 *  - Read opcodes 0x03, 0x04, 0x06, 0x08 are safe.
 *  - 0x05 is Set State: a DESTRUCTIVE ATOMIC write. Every field is applied;
 *    a zero field means "set to zero", not "leave alone". Callers must
 *    round-trip current state and verify with a fresh status read afterwards.
 *  - NEVER send 0x07, 0x09, 0x0a+ — 0x09 hung the amp's USB stack on
 *    2026-05-02 and required a firmware re-flash to recover.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HYPEX_USB_VID 0x345e /* "Hypex Electronics BV" */
#define HYPEX_USB_PID 0x03e8 /* FA503 (product string "DSP3-213") */

/* Both directions use 64-byte packets on the interrupt endpoints
 * (OUT 0x01, IN 0x81). Status responses are 67 bytes on the wire; the
 * final 3 bytes are a constant trailer and hosts commonly read 64. */
#define HYPEX_PACKET_LEN 64

#define HYPEX_VOLUME_MIN_DB_X100 (-9900) /* -99.00 dB */
#define HYPEX_VOLUME_MAX_DB_X100 0       /*   0.00 dB */

typedef enum {
    HYPEX_INPUT_NO_CHANGE = 0x00, /* "SCAN" — keeps the active source; use this
                                     in writes unless deliberately switching */
    HYPEX_INPUT_XLR = 0x01,
    HYPEX_INPUT_RCA = 0x02,
    HYPEX_INPUT_SPDIF = 0x04,
    HYPEX_INPUT_AES = 0x05,
    HYPEX_INPUT_OPT = 0x06,
} hypex_input_t;

/* The four fields committed atomically by Set State (0x05). */
typedef struct {
    uint8_t preset;         /* 1..3 */
    int16_t volume_db_x100; /* dB * 100, HYPEX_VOLUME_MIN..MAX */
    bool mute;
    uint8_t input_source;   /* hypex_input_t; NO_CHANGE recommended */
} hypex_state_t;

/* Decoded status response (0x06 0x02 request, response type 0x05). */
typedef struct {
    uint8_t packet_id;             /* byte 1: echoes request type — 0x06 for a
                                      get-status, 0x00 for a Set State response */
    uint8_t preset;                /* byte 2 */
    int16_t volume_db_x100;        /* bytes 3-4 LE: commanded/target volume */
    bool mute;                     /* byte 6 bit 7 */
    uint8_t active_input;          /* byte 50 (empirical; not in vendor PDF) */
    int16_t actual_volume_db_x100; /* bytes 52-53 LE: lags target during ramps */
    bool audio_active;             /* byte 60 bit 6 — GUESS-level decode */
    /* Provisional fields, offsets pending on-hardware confirmation: */
    uint16_t vu_raw;      /* bytes 47-48 LE; HFD-pcap-derived peak level */
    uint32_t project_sig; /* bytes 8-11 LE; changes when the DSP project does */
} hypex_status_t;

/* Decoded live-counters response (0x08 NN). */
typedef struct {
    uint16_t counter_a; /* bytes 2-3 LE, free-running */
    uint16_t counter_b; /* bytes 6-7 LE, free-running */
    uint8_t boot_count; /* byte 10 — GUESS-level decode */
} hypex_counters_t;

/* --- Request builders. Each fills buf[HYPEX_PACKET_LEN]. ------------------
 * Note: no hidapi 0x00 sentinel here — that is a hidapi/Windows artifact.
 * These are the raw 64 bytes to submit on the interrupt OUT endpoint. */
void hypex_build_get_status(uint8_t *buf);        /* 0x06 0x02 */
void hypex_build_get_capabilities(uint8_t *buf);  /* 0x06 0x01 (static block) */
void hypex_build_get_calibration(uint8_t *buf);   /* 0x06 0x03 (static block) */
void hypex_build_get_filter_name(uint8_t *buf);   /* 0x03 0x08 */
void hypex_build_get_counters(uint8_t *buf, uint8_t sub); /* 0x08 NN */

/* Builds a Set State (0x05) packet. Returns false (buffer untouched) if any
 * field is out of range — this function REJECTS rather than clamps, so range
 * policy stays with the caller (see hypex_volume_clamp). */
bool hypex_build_set_state(uint8_t *buf, const hypex_state_t *s);

/* --- Response parsers. Return false if buf is not a well-formed response
 * of the expected type (wrong response-type byte or too short). ------------ */
bool hypex_parse_status(const uint8_t *buf, size_t len, hypex_status_t *out);

/* Filter/project name (response to 0x03 0x08). Copies a NUL-terminated ASCII
 * string into name (at most name_cap bytes incl. NUL). An EMPTY name with a
 * true return is meaningful: it is the fingerprint of "no DSP project
 * loaded" (post-bootloader state). */
bool hypex_parse_filter_name(const uint8_t *buf, size_t len, char *name,
                             size_t name_cap);

bool hypex_parse_counters(const uint8_t *buf, size_t len,
                          hypex_counters_t *out);

/* --- Helpers --------------------------------------------------------------*/
int16_t hypex_volume_clamp(int32_t db_x100);
bool hypex_input_is_valid(uint8_t v); /* valid in a Set State packet */

/* True only for the empirically-safe read opcode families
 * {0x03, 0x04, 0x06, 0x08}. Transport layers must refuse to send any
 * request whose first byte is neither safe-read nor 0x05. */
bool hypex_is_safe_read_opcode(uint8_t opcode);

#ifdef __cplusplus
}
#endif
