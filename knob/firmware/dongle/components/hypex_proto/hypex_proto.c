#include "hypex_proto.h"

#include <string.h>

/* Byte offsets in the status response. Sources: vendor PDF where noted,
 * otherwise empirical decode logged in knob/docs/experiments.md. */
#define STATUS_RESPONSE_TYPE 0x05
#define OFF_PACKET_ID 1
#define OFF_PRESET 2
#define OFF_VOLUME_LO 3
#define OFF_VOLUME_HI 4
#define OFF_FLAGS 6
#define OFF_PROJECT_SIG 8   /* 4 bytes LE (GUESS) */
#define OFF_VU_LO 47        /* PROVISIONAL — from HFD pcap decode */
#define OFF_VU_HI 48
#define OFF_ACTIVE_INPUT 50 /* empirical, not in vendor PDF */
#define OFF_ACTUAL_VOL_LO 52
#define OFF_ACTUAL_VOL_HI 53
#define OFF_AUDIO_FLAGS 60  /* bit 6 = audio active (GUESS) */

#define MUTE_BIT 0x80
#define AUDIO_ACTIVE_BIT 0x40

static void build2(uint8_t *buf, uint8_t b0, uint8_t b1)
{
    memset(buf, 0, HYPEX_PACKET_LEN);
    buf[0] = b0;
    buf[1] = b1;
}

void hypex_build_get_status(uint8_t *buf) { build2(buf, 0x06, 0x02); }
void hypex_build_get_capabilities(uint8_t *buf) { build2(buf, 0x06, 0x01); }
void hypex_build_get_calibration(uint8_t *buf) { build2(buf, 0x06, 0x03); }
void hypex_build_get_filter_name(uint8_t *buf) { build2(buf, 0x03, 0x08); }

void hypex_build_get_counters(uint8_t *buf, uint8_t sub)
{
    build2(buf, 0x08, sub);
}

bool hypex_input_is_valid(uint8_t v)
{
    switch (v) {
    case HYPEX_INPUT_NO_CHANGE:
    case HYPEX_INPUT_XLR:
    case HYPEX_INPUT_RCA:
    case HYPEX_INPUT_SPDIF:
    case HYPEX_INPUT_AES:
    case HYPEX_INPUT_OPT:
        return true;
    default:
        return false;
    }
}

int16_t hypex_volume_clamp(int32_t db_x100)
{
    if (db_x100 < HYPEX_VOLUME_MIN_DB_X100) return HYPEX_VOLUME_MIN_DB_X100;
    if (db_x100 > HYPEX_VOLUME_MAX_DB_X100) return HYPEX_VOLUME_MAX_DB_X100;
    return (int16_t)db_x100;
}

bool hypex_is_safe_read_opcode(uint8_t opcode)
{
    return opcode == 0x03 || opcode == 0x04 || opcode == 0x06 ||
           opcode == 0x08;
}

bool hypex_build_set_state(uint8_t *buf, const hypex_state_t *s)
{
    if (s == NULL || buf == NULL) return false;
    if (s->preset < 1 || s->preset > 3) return false;
    if (s->volume_db_x100 < HYPEX_VOLUME_MIN_DB_X100 ||
        s->volume_db_x100 > HYPEX_VOLUME_MAX_DB_X100)
        return false;
    if (!hypex_input_is_valid(s->input_source)) return false;

    memset(buf, 0, HYPEX_PACKET_LEN);
    buf[0] = 0x05;
    buf[1] = s->input_source;
    buf[2] = s->preset;
    buf[3] = (uint8_t)(s->volume_db_x100 & 0xff);
    buf[4] = (uint8_t)((s->volume_db_x100 >> 8) & 0xff);
    buf[5] = 0x00;
    buf[6] = s->mute ? MUTE_BIT : 0x00;
    return true;
}

static int16_t rd_i16le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool hypex_parse_status(const uint8_t *buf, size_t len, hypex_status_t *out)
{
    if (buf == NULL || out == NULL || len < HYPEX_PACKET_LEN) return false;
    if (buf[0] != STATUS_RESPONSE_TYPE) return false;

    out->packet_id = buf[OFF_PACKET_ID];
    out->preset = buf[OFF_PRESET];
    out->volume_db_x100 = rd_i16le(&buf[OFF_VOLUME_LO]);
    out->mute = (buf[OFF_FLAGS] & MUTE_BIT) != 0;
    out->active_input = buf[OFF_ACTIVE_INPUT];
    out->actual_volume_db_x100 = rd_i16le(&buf[OFF_ACTUAL_VOL_LO]);
    out->audio_active = (buf[OFF_AUDIO_FLAGS] & AUDIO_ACTIVE_BIT) != 0;
    out->vu_raw = (uint16_t)buf[OFF_VU_LO] | ((uint16_t)buf[OFF_VU_HI] << 8);
    out->project_sig = (uint32_t)buf[OFF_PROJECT_SIG] |
                       ((uint32_t)buf[OFF_PROJECT_SIG + 1] << 8) |
                       ((uint32_t)buf[OFF_PROJECT_SIG + 2] << 16) |
                       ((uint32_t)buf[OFF_PROJECT_SIG + 3] << 24);
    return true;
}

bool hypex_parse_filter_name(const uint8_t *buf, size_t len, char *name,
                             size_t name_cap)
{
    if (buf == NULL || name == NULL || name_cap == 0) return false;
    if (len < HYPEX_PACKET_LEN || buf[0] != 0x03) return false;

    /* NUL-terminated ASCII starting at byte 2. */
    size_t n = 0;
    while (n < name_cap - 1 && 2 + n < len && buf[2 + n] != 0) {
        name[n] = (char)buf[2 + n];
        n++;
    }
    name[n] = '\0';
    return true;
}

bool hypex_parse_counters(const uint8_t *buf, size_t len,
                          hypex_counters_t *out)
{
    if (buf == NULL || out == NULL || len < HYPEX_PACKET_LEN) return false;
    if (buf[0] != 0x08) return false;

    out->counter_a = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    out->counter_b = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
    out->boot_count = buf[10];
    return true;
}
