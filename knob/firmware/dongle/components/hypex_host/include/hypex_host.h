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

/* Optional: called on each successful decoded status read. */
typedef void (*hypex_host_status_cb_t)(const hypex_status_t *status);

/* Brings up the board's USB host power path and the USB Host Library,
 * then runs the read-only probe described above. Returns
 * ESP_ERR_NOT_SUPPORTED on a target without USB-OTG. `cb` may be NULL. */
esp_err_t hypex_host_start(hypex_host_status_cb_t cb);

#ifdef __cplusplus
}
#endif
