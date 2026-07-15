/*
 * Phase B2 amp backend: the knob driving the REAL FA503 over USB host.
 *
 * Set State (0x05) is a destructive ATOMIC write — every field is applied and
 * a zero field means "set to zero", not "leave alone". Everything here exists
 * to make that safe. The rules, all from knob/docs/experiments.md:
 *
 *  1. NEVER write speculatively. We hold no opinion about the amp until a real
 *     0x06 0x02 read seeds us; knob events before that are dropped, not
 *     applied to a guessed state.
 *  2. Round-trip every field. We send the seeded state with only the field the
 *     user changed altered — and input_source always as NO_CHANGE, so the
 *     amp's own source selection is never clobbered.
 *  3. >=100 ms between Set State writes. The amp silently ignored a write that
 *     arrived ~50 ms after the previous one, so rapid detents are COALESCED:
 *     the target accumulates, the wire sees at most one write per gap. No
 *     detent is lost — only intermediate values are skipped.
 *  4. Verify with a fresh read after every write; never trust the write's own
 *     response (a stale buffer once reported a preset change that hadn't
 *     happened). A mismatch is logged, NOT auto-retried — re-asserting in a
 *     loop would hammer the amp.
 *  5. Volume ceiling (DONGLE_MAX_VOLUME_DB_X100) applies to INCREASES only:
 *     no knob spin or arithmetic bug can command a volume above it, but it is
 *     never used to move the amp. (An earlier revision clamped the seeded
 *     state to the ceiling — so an amp found above the ceiling was DRAGGED to
 *     it by the first detent. Seeding now records the amp as it really is,
 *     and if that is above the ceiling, increases are simply refused.)
 *  6. Slew limit: one write may move the volume at most
 *     SLEW_MAX_DB_X100 from the last volume the amp itself reported. Bigger
 *     changes ramp across several verified writes; a corrupt target cannot
 *     produce a jump.
 *  7. Startup-volume tripwire: the persistent power-on volume rides inside
 *     every Set State (bytes 21-22 — the 2026-07-15 loud incident). The
 *     transport round-trips it; this layer verifies after every write that
 *     the amp still reports the value we seeded with, and screams if not.
 */
#include "amp_backend.h"

#include "sdkconfig.h"

#if CONFIG_DONGLE_APP_MODE_USB_LIVE

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hypex_host.h"

static const char *TAG = "amp_usb";

/* Hard ceiling on anything this backend will command (rule 5). */
#define VOL_CEILING_DB_X100 CONFIG_DONGLE_MAX_VOLUME_DB_X100
/* Minimum spacing between Set State writes (rule 3 above). */
#define SET_STATE_MIN_GAP_MS 100
#define WORKER_TICK_MS 20
/* Max volume movement in a single write (rule 6): 3 dB per write at the
 * 100 ms write gap = a 30 dB/s ramp, fast enough to feel immediate, slow
 * enough that a wild target reaches the ear as a ramp, not a bang. */
#define SLEW_MAX_DB_X100 300

/* ESP-IDF's nano newlib drops %f (this is what blanked the LCD's volume
 * number), so dB is formatted by hand. */
static inline int db_whole(int16_t v) { return (v < 0 ? -v : v) / 100; }
static inline int db_frac(int16_t v) { return (v < 0 ? -v : v) % 100; }
#define DB_FMT "%s%d.%02d"
#define DB_ARG(v) ((v) < 0 ? "-" : ""), db_whole(v), db_frac(v)

static SemaphoreHandle_t s_lock;
static hypex_state_t s_target;
static bool s_seeded;         /* a real read has told us where the amp is */
static bool s_dirty;          /* s_target differs from what we last wrote */
static int64_t s_last_write_us;
static int16_t s_amp_vol;     /* last volume the AMP ITSELF reported (seed or
                                 verified read) — the anchor for rule 6 */
static int16_t s_startup_vol; /* the amp's persistent power-on volume as
                                 seeded — the tripwire baseline for rule 7 */

/* Rule 1: learn the amp's actual state before we ever command it.
 * Deliberately NO ceiling clamp here: the seed records reality. Clamping it
 * would make the ceiling the target, and the first detent would then command
 * a jump to the ceiling — the bug behind the 2026-07-15 scare. */
static void seed_from_amp(void)
{
    hypex_status_t st;
    if (hypex_host_read_status(&st) != ESP_OK) {
        return; /* try again next tick */
    }
    if (st.preset < 1 || st.preset > 3) {
        ESP_LOGW(TAG, "implausible seed (preset %u) — staying unseeded",
                 st.preset);
        return;
    }

    if (st.volume_db_x100 > VOL_CEILING_DB_X100) {
        ESP_LOGW(TAG, "amp is at " DB_FMT " dB, above our " DB_FMT " dB "
                      "ceiling — volume increases are disabled until it is "
                      "turned down below the ceiling",
                 DB_ARG(st.volume_db_x100), DB_ARG((int16_t)VOL_CEILING_DB_X100));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_target.preset = st.preset;
    s_target.volume_db_x100 = st.volume_db_x100;
    s_target.mute = st.mute;
    s_target.input_source = st.active_input;
    s_amp_vol = st.volume_db_x100;
    s_startup_vol = st.startup_volume_db_x100;
    s_seeded = true;
    s_dirty = false; /* seeding is not a reason to write */
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "seeded from amp: preset %u | " DB_FMT " dB | %s | "
                  "input 0x%02x | startup " DB_FMT " dB",
             st.preset, DB_ARG(st.volume_db_x100),
             st.mute ? "MUTED" : "unmuted", st.active_input,
             DB_ARG(st.startup_volume_db_x100));
}

static void flush_to_amp(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hypex_state_t want = s_target;
    /* Clear before writing: a detent arriving mid-write re-sets s_dirty, so
     * the newer value goes out on the next pass instead of being lost. */
    s_dirty = false;
    int16_t anchor = s_amp_vol;
    xSemaphoreGive(s_lock);

    /* Rule 2: never touch the amp's input selection. */
    hypex_state_t wire = want;
    wire.input_source = HYPEX_INPUT_NO_CHANGE;

    /* Rule 6: one write moves at most SLEW_MAX from where the amp last
     * reported itself. A bigger difference goes out as a ramp: this write
     * takes a step, stays dirty, and the worker sends the next step after
     * the usual gap — each one anchored to a fresh verified read. */
    int32_t diff = (int32_t)want.volume_db_x100 - anchor;
    bool partial = false;
    if (diff > SLEW_MAX_DB_X100) {
        wire.volume_db_x100 = (int16_t)(anchor + SLEW_MAX_DB_X100);
        partial = true;
    } else if (diff < -SLEW_MAX_DB_X100) {
        wire.volume_db_x100 = (int16_t)(anchor - SLEW_MAX_DB_X100);
        partial = true;
    }

    esp_err_t err = hypex_host_write_state(&wire);
    s_last_write_us = esp_timer_get_time();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set State failed (%s) — will retry", esp_err_to_name(err));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_dirty = true;
        xSemaphoreGive(s_lock);
        return;
    }

    /* Rule 4: verify with a fresh read. */
    hypex_status_t st;
    if (hypex_host_read_status(&st) != ESP_OK) {
        ESP_LOGW(TAG, "verify read failed after Set State");
        return;
    }

    /* Rule 7: the persistent power-on volume must never move because of us. */
    if (st.startup_volume_db_x100 != s_startup_vol) {
        ESP_LOGE(TAG, "STARTUP VOLUME CHANGED: was " DB_FMT " dB, amp now "
                      "reports " DB_FMT " dB — STOP and investigate "
                      "(this is the 2026-07-15 corruption signature)",
                 DB_ARG(s_startup_vol), DB_ARG(st.startup_volume_db_x100));
    }

    if (st.volume_db_x100 != wire.volume_db_x100 || st.preset != wire.preset ||
        st.mute != wire.mute) {
        /* Logged, deliberately not re-asserted — see rule 4. */
        ESP_LOGW(TAG, "verify MISMATCH: wanted preset %u " DB_FMT " dB %s; "
                      "amp reports preset %u " DB_FMT " dB %s",
                 wire.preset, DB_ARG(wire.volume_db_x100),
                 wire.mute ? "muted" : "unmuted", st.preset,
                 DB_ARG(st.volume_db_x100), st.mute ? "muted" : "unmuted");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* The verified read is the new slew anchor. */
    s_amp_vol = st.volume_db_x100;
    /* The amp is authoritative about the input source. */
    s_target.input_source = st.active_input;
    if (partial) {
        s_dirty = true; /* keep ramping toward the target */
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "preset %u | " DB_FMT " dB | %s | input 0x%02x%s", st.preset,
             DB_ARG(st.volume_db_x100), st.mute ? "MUTED" : "unmuted",
             st.active_input, partial ? " (ramping)" : "");
}

static void worker_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(WORKER_TICK_MS));

        if (!hypex_host_is_connected()) {
            continue;
        }
        if (!s_seeded) {
            seed_from_amp();
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool dirty = s_dirty;
        xSemaphoreGive(s_lock);
        if (!dirty) {
            continue;
        }

        /* Rule 3: coalesce — hold off until the gap has elapsed. */
        if (esp_timer_get_time() - s_last_write_us <
            (int64_t)SET_STATE_MIN_GAP_MS * 1000) {
            continue;
        }
        flush_to_amp();
    }
}

void amp_backend_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    /* Defensive default: minimum volume. We never write before seeding, but
     * if that guarantee were ever broken, the failure should be silence. */
    s_target = (hypex_state_t){
        .preset = 1,
        .volume_db_x100 = HYPEX_VOLUME_MIN_DB_X100,
        .mute = false,
        .input_source = HYPEX_INPUT_NO_CHANGE,
    };

    xTaskCreate(worker_task, "amp_usb", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "USB amp backend: ceiling " DB_FMT " dB, >=%d ms between writes",
             DB_ARG((int16_t)VOL_CEILING_DB_X100), SET_STATE_MIN_GAP_MS);
}

hypex_state_t amp_backend_get_state(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hypex_state_t copy = s_target;
    xSemaphoreGive(s_lock);
    return copy;
}

/* Knob events only mutate the target; the worker decides when it reaches the
 * wire. Dropped (not queued) until seeded — see rule 1. */
static bool reject_unseeded(void)
{
    if (!s_seeded) {
        ESP_LOGW(TAG, "ignoring knob — amp state not known yet");
        return true;
    }
    return false;
}

void amp_backend_volume_delta(int16_t delta_db_x100)
{
    if (reject_unseeded()) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int16_t cur = s_target.volume_db_x100;
    int16_t v = hypex_volume_clamp((int32_t)cur + delta_db_x100);
    if (delta_db_x100 > 0) {
        /* Rule 5: the ceiling gates INCREASES only. It is never a value we
         * move the amp to — if the amp is already above it (someone else put
         * it there), increases are refused and decreases work normally. */
        int16_t cap = cur > VOL_CEILING_DB_X100 ? cur
                                                : (int16_t)VOL_CEILING_DB_X100;
        if (v > cap) {
            v = cap;
        }
    }
    if (v != cur) {
        s_target.volume_db_x100 = v;
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
}

void amp_backend_mute_toggle(void)
{
    if (reject_unseeded()) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_target.mute = !s_target.mute;
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

void amp_backend_preset_step(int dir)
{
    if (reject_unseeded()) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int p = (int)s_target.preset - 1 + dir;
    s_target.preset = (uint8_t)((p % 3 + 3) % 3 + 1);
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

#endif /* CONFIG_DONGLE_APP_MODE_USB_LIVE */
