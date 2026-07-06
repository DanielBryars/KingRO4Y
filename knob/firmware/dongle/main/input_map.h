/* input_map — turns decoded controller events into amp commands. */
#pragma once

#include "ble_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Volume change per Volume+/- event, in dB x 100. The VOL20 emits one
 * event per detent. TODO: move to NVS once the feel has been tuned. */
#define INPUT_MAP_VOL_STEP_DB_X100 50 /* 0.5 dB */

void input_map_handle(input_event_t evt);

#ifdef __cplusplus
}
#endif
