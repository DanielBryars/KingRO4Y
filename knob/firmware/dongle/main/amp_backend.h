/*
 * amp_backend — the dongle's amp-facing side, behind a small interface so
 * the BLE input half doesn't care what implements it:
 *
 *   phase A: amp_backend_log.c  — simulated amp state, logged to console
 *                                 (runs on any board, no amp needed)
 *   phase B: amp_backend_usb.c  — ESP32-S3 USB host driving the real FA503
 *                                 (same interface; per HypexUsbProtocol.md,
 *                                 with the >=100 ms Set State rate limit and
 *                                 verify-after-write reads)
 */
#pragma once

#include "hypex_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

void amp_backend_init(void);

hypex_state_t amp_backend_get_state(void);

/* Nudge volume by delta (dB x 100); clamped to the protocol range. */
void amp_backend_volume_delta(int16_t delta_db_x100);

void amp_backend_mute_toggle(void);

/* Step the preset by +1 / -1, wrapping within 1..3. */
void amp_backend_preset_step(int dir);

#ifdef __cplusplus
}
#endif
