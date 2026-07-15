/*
 * blackbox — persist a fault across a reboot so it can be reported while the
 * console still exists.
 *
 * Once usb_host_install() seizes the USB PHY, the USB-Serial-JTAG console is
 * gone: any fault during the USB phase is invisible except as a blinking LED
 * or a line on the LCD that a human has to read out. That is not observability.
 *
 * So: record the fault to NVS when it happens, and dump it on the NEXT boot,
 * during DONGLE_USB_HOST_START_DELAY_S while the console is still alive and a
 * host can read it over the COM port.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log any fault stored by a previous boot. Call early — before USB host
 * starts and kills the console. No-op if nothing was recorded. */
void blackbox_report_previous(void);

/* Record a fault. Only the FIRST call per boot is persisted: these faults can
 * repeat many times a second, and NVS is flash — writing each one would burn
 * the sector for no extra information. */
void blackbox_record(const char *msg, const uint8_t *data, int len);

#ifdef __cplusplus
}
#endif
