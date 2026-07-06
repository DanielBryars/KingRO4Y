/*
 * Host-side unit tests for hypex_proto.
 *
 * Golden vectors are real packets captured from the FA503 during the
 * 2026-05-02/03 sessions (knob/docs/experiments.md). Run via run_tests.py,
 * which finds a host C compiler (MSVC or gcc), builds, and executes this.
 */
#include <stdio.h>
#include <string.h>

#include "hypex_proto.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                 \
    do {                                                                   \
        g_checks++;                                                        \
        long long va = (long long)(a), vb = (long long)(b);                \
        if (va != vb) {                                                    \
            g_failures++;                                                  \
            printf("FAIL %s:%d  %s == %s  (%lld != %lld)\n", __FILE__,     \
                   __LINE__, #a, #b, va, vb);                              \
        }                                                                  \
    } while (0)

/* Real get-status response: preset 1, -40.00 dB, unmuted, input OPT.
 * Captured 2026-05-02 (experiments.md, "Status response decoded"). */
static const uint8_t STATUS_P1_M40_UNMUTED[HYPEX_PACKET_LEN] = {
    0x05, 0x06, 0x01, 0x60, 0xf0, 0x00, 0x00, 0x00,
    0x0f, 0x19, 0x00, 0x02, 0x16, 0x01, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0xf0, 0x00,
    0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xf6, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xe0, 0x01, 0x99, 0x03,
    0x16, 0x00, 0x06, 0x01, 0x60, 0xf0, 0x00, 0x26,
    0x26, 0x00, 0x00, 0x42, 0x40, 0x80, 0xda, 0x02,
};

/* Real filter-name response for the loaded KingRO4Y project ("Config.xml"),
 * captured 2026-05-03. Bytes beyond the string are zero. */
static const uint8_t FILTER_NAME_CONFIG_XML[HYPEX_PACKET_LEN] = {
    0x03, 0x00, 'C', 'o', 'n', 'f', 'i', 'g', '.', 'x', 'm', 'l', 0x00,
};

/* Real live-counters response, captured 2026-05-02 overnight sweep. */
static const uint8_t COUNTERS_SAMPLE[HYPEX_PACKET_LEN] = {
    0x08, 0x01, 0xf1, 0x1a, 0x00, 0x00, 0xa4, 0x19,
    0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x02,
};

static void test_parse_status_golden(void)
{
    hypex_status_t st;
    CHECK(hypex_parse_status(STATUS_P1_M40_UNMUTED, HYPEX_PACKET_LEN, &st));
    CHECK_EQ_INT(st.packet_id, 0x06);
    CHECK_EQ_INT(st.preset, 1);
    CHECK_EQ_INT(st.volume_db_x100, -4000); /* -40.00 dB */
    CHECK(!st.mute);
    CHECK_EQ_INT(st.active_input, HYPEX_INPUT_OPT);
    CHECK_EQ_INT(st.actual_volume_db_x100, -4000);
    CHECK(st.audio_active); /* byte 60 = 0x40 */
    CHECK_EQ_INT(st.vu_raw, 0x1603);
    CHECK_EQ_INT(st.project_sig, 0x0200190fUL);
}

static void test_parse_status_muted_factory(void)
{
    /* Synthetic: the post-bootloader factory state seen on 2026-05-03 —
     * preset 3, volume 0 dB, mute ON. */
    uint8_t buf[HYPEX_PACKET_LEN] = {0};
    buf[0] = 0x05;
    buf[1] = 0x06;
    buf[2] = 3;
    buf[3] = 0x00;
    buf[4] = 0x00;
    buf[6] = 0x80;

    hypex_status_t st;
    CHECK(hypex_parse_status(buf, HYPEX_PACKET_LEN, &st));
    CHECK_EQ_INT(st.preset, 3);
    CHECK_EQ_INT(st.volume_db_x100, 0);
    CHECK(st.mute);
    CHECK(!st.audio_active);
}

static void test_parse_status_rejects(void)
{
    hypex_status_t st;
    uint8_t zeros[HYPEX_PACKET_LEN] = {0}; /* the hidapi phantom-report-ID
                                              symptom: all-zero packet */
    CHECK(!hypex_parse_status(zeros, HYPEX_PACKET_LEN, &st));
    CHECK(!hypex_parse_status(STATUS_P1_M40_UNMUTED, 32, &st)); /* short */
    uint8_t wrong[HYPEX_PACKET_LEN] = {0x66}; /* calibration-block type */
    CHECK(!hypex_parse_status(wrong, HYPEX_PACKET_LEN, &st));
}

static void test_build_requests(void)
{
    uint8_t buf[HYPEX_PACKET_LEN];

    hypex_build_get_status(buf);
    CHECK_EQ_INT(buf[0], 0x06);
    CHECK_EQ_INT(buf[1], 0x02);
    for (int i = 2; i < HYPEX_PACKET_LEN; i++) CHECK(buf[i] == 0);

    hypex_build_get_filter_name(buf);
    CHECK_EQ_INT(buf[0], 0x03);
    CHECK_EQ_INT(buf[1], 0x08);

    hypex_build_get_capabilities(buf);
    CHECK_EQ_INT(buf[0], 0x06);
    CHECK_EQ_INT(buf[1], 0x01);

    hypex_build_get_calibration(buf);
    CHECK_EQ_INT(buf[0], 0x06);
    CHECK_EQ_INT(buf[1], 0x03);

    hypex_build_get_counters(buf, 0x00);
    CHECK_EQ_INT(buf[0], 0x08);
    CHECK_EQ_INT(buf[1], 0x00);
}

static void test_build_set_state_volume_encoding(void)
{
    /* Encodings from the verified table in HypexUsbProtocol.md §4. */
    static const struct {
        int16_t db_x100;
        uint8_t lo, hi;
    } cases[] = {
        {0, 0x00, 0x00},      {-300, 0xd4, 0xfe},  {-1000, 0x18, 0xfc},
        {-3000, 0x48, 0xf4},  {-4000, 0x60, 0xf0}, {-5000, 0x78, 0xec},
        {-6000, 0x90, 0xe8},  {-9900, 0x54, 0xd9}, {-50, 0xce, 0xff},
    };
    uint8_t buf[HYPEX_PACKET_LEN];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hypex_state_t s = {1, cases[i].db_x100, false,
                           HYPEX_INPUT_NO_CHANGE};
        CHECK(hypex_build_set_state(buf, &s));
        CHECK_EQ_INT(buf[0], 0x05);
        CHECK_EQ_INT(buf[1], HYPEX_INPUT_NO_CHANGE);
        CHECK_EQ_INT(buf[2], 1);
        CHECK_EQ_INT(buf[3], cases[i].lo);
        CHECK_EQ_INT(buf[4], cases[i].hi);
        CHECK_EQ_INT(buf[5], 0x00);
        CHECK_EQ_INT(buf[6], 0x00);
    }
}

static void test_build_set_state_mute_and_input(void)
{
    uint8_t buf[HYPEX_PACKET_LEN];
    hypex_state_t s = {2, -6000, true, HYPEX_INPUT_OPT};
    CHECK(hypex_build_set_state(buf, &s));
    CHECK_EQ_INT(buf[1], HYPEX_INPUT_OPT);
    CHECK_EQ_INT(buf[2], 2);
    CHECK_EQ_INT(buf[3], 0x90);
    CHECK_EQ_INT(buf[4], 0xe8);
    CHECK_EQ_INT(buf[6], 0x80);
    for (int i = 7; i < HYPEX_PACKET_LEN; i++) CHECK(buf[i] == 0);
}

static void test_build_set_state_rejects(void)
{
    uint8_t buf[HYPEX_PACKET_LEN];
    hypex_state_t s;

    s = (hypex_state_t){0, -4000, false, HYPEX_INPUT_NO_CHANGE};
    CHECK(!hypex_build_set_state(buf, &s)); /* preset 0: the May-02 all-zeros
                                               incident state — must reject */
    s = (hypex_state_t){4, -4000, false, HYPEX_INPUT_NO_CHANGE};
    CHECK(!hypex_build_set_state(buf, &s));
    s = (hypex_state_t){1, 100, false, HYPEX_INPUT_NO_CHANGE};
    CHECK(!hypex_build_set_state(buf, &s)); /* above 0 dB */
    s = (hypex_state_t){1, -10000, false, HYPEX_INPUT_NO_CHANGE};
    CHECK(!hypex_build_set_state(buf, &s));
    s = (hypex_state_t){1, -4000, false, 0x03};
    CHECK(!hypex_build_set_state(buf, &s)); /* 0x03 is not a valid input */
}

static void test_parse_filter_name(void)
{
    char name[64];
    CHECK(hypex_parse_filter_name(FILTER_NAME_CONFIG_XML, HYPEX_PACKET_LEN,
                                  name, sizeof(name)));
    CHECK(strcmp(name, "Config.xml") == 0);

    /* Empty template = no DSP project loaded (post-bootloader fingerprint) */
    uint8_t empty[HYPEX_PACKET_LEN] = {0x03, 0x00};
    CHECK(hypex_parse_filter_name(empty, HYPEX_PACKET_LEN, name,
                                  sizeof(name)));
    CHECK_EQ_INT((int)strlen(name), 0);

    /* Truncation respects name_cap */
    char tiny[4];
    CHECK(hypex_parse_filter_name(FILTER_NAME_CONFIG_XML, HYPEX_PACKET_LEN,
                                  tiny, sizeof(tiny)));
    CHECK(strcmp(tiny, "Con") == 0);
}

static void test_parse_counters(void)
{
    hypex_counters_t c;
    CHECK(hypex_parse_counters(COUNTERS_SAMPLE, HYPEX_PACKET_LEN, &c));
    CHECK_EQ_INT(c.counter_a, 0x1af1);
    CHECK_EQ_INT(c.counter_b, 0x19a4);
    CHECK_EQ_INT(c.boot_count, 8);

    uint8_t wrong[HYPEX_PACKET_LEN] = {0x06};
    CHECK(!hypex_parse_counters(wrong, HYPEX_PACKET_LEN, &c));
}

static void test_helpers(void)
{
    CHECK_EQ_INT(hypex_volume_clamp(-20000), HYPEX_VOLUME_MIN_DB_X100);
    CHECK_EQ_INT(hypex_volume_clamp(500), 0);
    CHECK_EQ_INT(hypex_volume_clamp(-4000), -4000);

    CHECK(hypex_is_safe_read_opcode(0x03));
    CHECK(hypex_is_safe_read_opcode(0x04));
    CHECK(hypex_is_safe_read_opcode(0x06));
    CHECK(hypex_is_safe_read_opcode(0x08));
    CHECK(!hypex_is_safe_read_opcode(0x05)); /* write, not read */
    CHECK(!hypex_is_safe_read_opcode(0x07)); /* DO NOT SEND */
    CHECK(!hypex_is_safe_read_opcode(0x09)); /* DO NOT SEND, EVER */
    CHECK(!hypex_is_safe_read_opcode(0x0a));
    CHECK(!hypex_is_safe_read_opcode(0x65)); /* HFD-observed but unprobed by
                                                our firmware — keep out of the
                                                allowlist until tested */

    CHECK(hypex_input_is_valid(HYPEX_INPUT_NO_CHANGE));
    CHECK(hypex_input_is_valid(HYPEX_INPUT_OPT));
    CHECK(!hypex_input_is_valid(0x03));
    CHECK(!hypex_input_is_valid(0x07));
}

int main(void)
{
    test_parse_status_golden();
    test_parse_status_muted_factory();
    test_parse_status_rejects();
    test_build_requests();
    test_build_set_state_volume_encoding();
    test_build_set_state_mute_and_input();
    test_build_set_state_rejects();
    test_parse_filter_name();
    test_parse_counters();
    test_helpers();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
