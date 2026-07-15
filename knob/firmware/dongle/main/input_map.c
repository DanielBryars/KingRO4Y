#include "input_map.h"

#include "amp_backend.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "input_map";

/*
 * VOL20 mapping (see knob/docs/Architecture.md, tier 2):
 *   rotate            -> volume up/down in INPUT_MAP_VOL_STEP steps,
 *                        accelerated when detents arrive in quick succession
 *   single-click      -> play/pause  -> amp mute toggle
 *   double-click      -> next track  -> amp preset up
 *   triple-click      -> prev track  -> amp preset down
 *
 * The VOL20's own 2s-long-press mute (usage 0xE2) also maps to mute toggle,
 * but avoid using it: the VOL20 locks itself after its own mute until
 * unmuted on the device (manual FAQ 4).
 */

/* Spin acceleration: a leisurely click stays one fine step (precision);
 * a fast spin multiplies it, so sweeping the volume doesn't take a wrist
 * workout. A direction change always resets to the fine step — correcting
 * an overshoot must never itself overshoot. The backend's slew limiter and
 * ceiling still bound whatever this asks for. */
static int16_t accel_step(int dir)
{
    static int64_t s_last_us;
    static int s_last_dir;

    int64_t now = esp_timer_get_time();
    int64_t gap_ms = (now - s_last_us) / 1000;
    s_last_us = now;

    int mult = 1;
    if (dir == s_last_dir) {
        if (gap_ms < 60) {
            mult = 4; /* fast spin: 2.0 dB per detent */
        } else if (gap_ms < 150) {
            mult = 2; /* brisk turn: 1.0 dB per detent */
        }
    }
    s_last_dir = dir;
    return (int16_t)(dir * mult * INPUT_MAP_VOL_STEP_DB_X100);
}

void input_map_handle(input_event_t evt)
{
    switch (evt) {
    case INPUT_EVT_VOL_UP:
        amp_backend_volume_delta(accel_step(+1));
        break;
    case INPUT_EVT_VOL_DOWN:
        amp_backend_volume_delta(accel_step(-1));
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
