#include "input_map.h"

#include "amp_backend.h"
#include "esp_log.h"

static const char *TAG = "input_map";

/*
 * VOL20 mapping (see knob/docs/Architecture.md, tier 2):
 *   rotate            -> volume up/down in INPUT_MAP_VOL_STEP steps
 *   single-click      -> play/pause  -> amp mute toggle
 *   double-click      -> next track  -> amp preset up
 *   triple-click      -> prev track  -> amp preset down
 *
 * The VOL20's own 2s-long-press mute (usage 0xE2) also maps to mute toggle,
 * but avoid using it: the VOL20 locks itself after its own mute until
 * unmuted on the device (manual FAQ 4).
 */
void input_map_handle(input_event_t evt)
{
    switch (evt) {
    case INPUT_EVT_VOL_UP:
        amp_backend_volume_delta(INPUT_MAP_VOL_STEP_DB_X100);
        break;
    case INPUT_EVT_VOL_DOWN:
        amp_backend_volume_delta(-INPUT_MAP_VOL_STEP_DB_X100);
        break;
    case INPUT_EVT_MUTE:
    case INPUT_EVT_PLAY_PAUSE:
        amp_backend_mute_toggle();
        break;
    case INPUT_EVT_NEXT:
        amp_backend_preset_step(+1);
        break;
    case INPUT_EVT_PREV:
        amp_backend_preset_step(-1);
        break;
    case INPUT_EVT_UNKNOWN:
    default:
        ESP_LOGD(TAG, "unmapped event %d", evt);
        break;
    }
}
