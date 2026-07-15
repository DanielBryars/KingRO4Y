/*
 * hypex_host — USB Host half of the dongle (ESP32-S3), phase B milestone 1.
 *
 * READ-ONLY probe: powers the USB-A host port on the ESP32-S3-USB-OTG
 * board, enumerates the attached Hypex FA503, verifies VID/PID, discovers
 * its interrupt endpoints, and performs a single SAFE status read
 * (0x06 0x02) which it decodes via hypex_proto. It never issues a Set State
 * (0x05) or any non-safe opcode — see the Safe-Opcode Policy in
 * knob/docs/experiments.md.
 *
 * This milestone deliberately does NOT implement the amp_backend interface
 * (no writes). Milestone B2 adds writes behind amp_backend_usb.c.
 *
 * On-board LED status (green=GPIO15, yellow=GPIO16), so the result is
 * visible even though enabling USB host takes over the USB PHY and drops
 * the USB-Serial-JTAG console:
 *   yellow solid          VBUS on, waiting for a device
 *   green solid           amp enumerated + status read OK
 *   yellow fast blink     a device enumerated but it isn't the FA503
 *   both alternate blink  error (no/!bad response, transfer failure)
 */
#pragma once

#include "esp_err.h"
#include "hypex_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional: called once on each successful decoded status read performed
 * automatically when the amp is (re)connected. */
typedef void (*hypex_host_status_cb_t)(const hypex_status_t *status);

/* Brings up the board's USB host power path and the USB Host Library, then
 * waits for the FA503. On connect it verifies VID/PID, discovers the
 * interrupt endpoints, claims the interface, and performs one safe status
 * read (firing `cb`). The link is then held open for hypex_host_read_status
 * / hypex_host_write_state below. Returns ESP_ERR_NOT_SUPPORTED on a target
 * without USB-OTG. `cb` may be NULL. */
esp_err_t hypex_host_start(hypex_host_status_cb_t cb);

/* True once the amp is enumerated and the interface is claimed. */
bool hypex_host_is_connected(void);

/* Reports faults (and odd-but-survivable observations) to the caller so they
 * can be surfaced somewhere visible. Installing USB host kills the console, so
 * without this a failure to link is a silent blinking LED and nothing more.
 * `data` may be NULL. Set before hypex_host_start(). */
typedef void (*hypex_host_diag_cb_t)(const char *msg, const uint8_t *data,
                                     int len);
void hypex_host_set_diag_cb(hypex_host_diag_cb_t cb);

/* --- Transactions. Thread-safe (serialised on an internal mutex); safe to
 * call from any task once hypex_host_start() has returned. Both return
 * ESP_ERR_INVALID_STATE if the amp is not currently connected. -------------*/

/* Sends the SAFE get-status request (0x06 0x02) and decodes the reply. */
esp_err_t hypex_host_read_status(hypex_status_t *out);

/* Sends a Set State (0x05) — the ONLY write this transport will ever emit.
 *
 * DANGER: Set State is a destructive ATOMIC write. Every field is applied;
 * a zero field means "set to zero", not "leave alone" — INCLUDING persistent
 * config hidden in the packet tail (startup volume at bytes 21-22). This
 * function therefore performs a read-modify-write: it takes a fresh status
 * frame under the link mutex, round-trips bytes 7..36 verbatim, and only
 * patches input/preset/volume/mute from `st`. Callers still own state policy
 * (see amp_backend_usb.c: seeds from a real read, never writes speculatively,
 * rate-limits to >=100 ms, verifies with a fresh read afterwards).
 *
 * Returns ESP_ERR_INVALID_ARG if `st` is out of range (hypex_build_set_state
 * rejects rather than clamps) or — defensively — if the built packet's
 * opcode is not 0x05. Fails without writing if the pre-write read fails. */
esp_err_t hypex_host_write_state(const hypex_state_t *st);

#ifdef __cplusplus
}
#endif
