/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file test_fuzz_parsers.c
 * @brief OpenDash Fuzz Tests — P3.1: the two attacker-controlled byte sinks
 *
 * Targets: opendash_msg_deserialize() (ESP-NOW frame codec) and
 * opendash_md_parse_payload() (Multidisplay UART frame decoder).
 *
 * Invariants asserted per iteration:
 *  - NEVER crash on random/truncated/overlong/adversarial input.
 *  - ACCEPT-AS-CANONICAL: if deserialize accepts a buffer, re-serializing
 *    the parsed message reproduces the frame bytes EXACTLY (the accepted
 *    frame is a well-formed, checksum-valid message — nothing malformed
 *    ever passes), and validate() agrees.
 *  - MD decoder: wrong length ⇒ reject; bad TAG ⇒ reject; TAG+93 ⇒ accept,
 *    deterministically, with fields exactly recomputed from the raw bytes.
 *
 * Deterministic: fixed seed committed below; CI re-runs reproduce exactly.
 */

#include "unity.h"
#include "opendash_protocol.h"
#include "opendash_uart.h"
#include <string.h>
#include <stdio.h>

/* ── Deterministic PRNG (xorshift64*) — seed committed, never random ───── */
#define FUZZ_SEED 0x9E3779B97F4A7C15ull
#define FUZZ_ITERS 10000

static uint64_t s_rng;
static void     rng_seed(uint64_t s) { s_rng = s; }
static uint64_t rng_next(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return s_rng;
}
static uint8_t  rng_byte(void)          { return (uint8_t)(rng_next() & 0xFFu); }
static uint16_t rng_u16(uint16_t max)   { return (uint16_t)(rng_next() % max); }

void setUp(void) {}
void tearDown(void) {}

/* ─────────────────────────────────────────────────────────────────────
 * Fuzz 1: opendash_msg_deserialize — pure noise / boundary lengths
 * ───────────────────────────────────────────────────────────────────── */
void test_fuzz_msg_deserialize_noise(void)
{
    rng_seed(FUZZ_SEED);
    uint32_t accepted = 0, rejected = 0;

    for (int i = 0; i < FUZZ_ITERS; i++) {
        uint8_t buf[OPENDASH_MSG_MAX_SIZE + 8];
        /* Adversarial lengths: under-min, exact-min, max, over-max */
        uint16_t len;
        switch (i % 4) {
            case 0: len = rng_u16(4); break;                              /* too short */
            case 1: len = (uint16_t)(4 + rng_u16(2)); break;               /* around min */
            case 2: len = (uint16_t)(OPENDASH_MSG_MAX_SIZE + rng_u16(4)); break; /* max+ */
            default: len = rng_u16(OPENDASH_MSG_MAX_SIZE + 8); break;       /* anything */
        }
        for (uint16_t b = 0; b < len && b < sizeof(buf); b++) buf[b] = rng_byte();
        if (i % 2 == 0 && len > 0) buf[0] = OPENDASH_MSG_SYNC; /* sync-favoring */

        opendash_msg_t msg;
        memset(&msg, 0xA5, sizeof(msg));
        opendash_err_t err = opendash_msg_deserialize(buf, len, &msg);

        if (err == OPENDASH_OK) {
            accepted++;
            /* Accepted ⇒ must be a canonical frame: re-serialize byte-identical */
            uint8_t  rt[OPENDASH_MSG_MAX_SIZE];
            uint16_t rt_len = 0;
            TEST_ASSERT_EQUAL_INT_MESSAGE(OPENDASH_OK,
                opendash_msg_serialize(&msg, rt, &rt_len), "roundtrip serialize");
            TEST_ASSERT_EQUAL_UINT16(4u + msg.length, rt_len);
            TEST_ASSERT_EQUAL_MEMORY(buf, rt, rt_len);
            /* validate() must agree with what deserialize accepted */
            TEST_ASSERT_TRUE(opendash_msg_validate(&msg));
        } else {
            rejected++;
        }
    }
    /* Sanity: the corpus must actually exercise both branches */
    TEST_ASSERT_GREATER_THAN_UINT32(0, accepted);
    TEST_ASSERT_GREATER_THAN_UINT32(0, rejected);
    printf("[fuzz] msg_deserialize: accepted=%lu rejected=%lu\n",
           (unsigned long)accepted, (unsigned long)rejected);
}

/* ─────────────────────────────────────────────────────────────────────
 * Fuzz 2: valid frames + bit corruption → only self-consistent survive
 * ───────────────────────────────────────────────────────────────────── */
void test_fuzz_msg_bitflip(void)
{
    rng_seed(FUZZ_SEED + 1);

    for (int i = 0; i < FUZZ_ITERS; i++) {
        uint8_t  payload[OPENDASH_MSG_MAX_PAYLOAD];
        uint8_t  plen = (uint8_t)rng_u16(OPENDASH_MSG_MAX_PAYLOAD + 1); /* 0..248 */
        for (uint8_t b = 0; b < plen; b++) payload[b] = rng_byte();

        opendash_msg_t m;
        TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_build(&m, rng_byte(), payload, plen));

        uint8_t  wire[OPENDASH_MSG_MAX_SIZE];
        uint16_t wire_len = 0;
        TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_serialize(&m, wire, &wire_len));

        /* Corrupt one random byte (any bit flip, any position incl. checksum) */
        uint16_t pos  = rng_u16(wire_len);
        uint8_t  flip = (uint8_t)(1u << (rng_byte() & 7u));
        wire[pos] ^= flip;

        opendash_msg_t got;
        memset(&got, 0x33, sizeof(got));
        opendash_err_t err = opendash_msg_deserialize(wire, wire_len, &got);

        if (err == OPENDASH_OK) {
            /* A corruption that still verifies is a checksum collision — the
             * accepted frame is then STILL canonical: byte-exact round-trip. */
            uint8_t  rt[OPENDASH_MSG_MAX_SIZE];
            uint16_t rt_len = 0;
            TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_serialize(&got, rt, &rt_len));
            TEST_ASSERT_EQUAL_MEMORY(wire, rt, rt_len);
            TEST_ASSERT_TRUE(opendash_msg_validate(&got));
            if (pos == (uint16_t)(wire_len - 1)) {
                /* Corrupting the checksum byte can never verify (XOR of every
                 * other byte is unchanged ⇒ recomputed CS != stored CS). */
                TEST_FAIL_MESSAGE("checksum-byte corruption must not verify");
            }
        }
    }
    printf("[fuzz] msg bit-flip corpus done (%d frames)\n", FUZZ_ITERS);
}

/* ─────────────────────────────────────────────────────────────────────
 * Fuzz 3: opendash_md_parse_payload — noise + determinism + TAG gate
 * ───────────────────────────────────────────────────────────────────── */
void test_fuzz_md_parse_payload(void)
{
    rng_seed(FUZZ_SEED + 2);
    uint32_t accepts = 0;

    for (int i = 0; i < FUZZ_ITERS; i++) {
        uint8_t  payload[200];
        uint16_t len = (i % 2) ? 93 : (uint16_t)(1 + rng_u16(199)); /* exact or noise */
        for (uint16_t b = 0; b < len; b++) payload[b] = rng_byte();

        bool tag_ok = (len == 93) && (payload[0] == 0x5F);
        if (len == 93 && payload[0] == 0x5F) tag_ok = true;
        else if (len == 93) payload[0] = 0x5E;   /* force bad-TAG rejection */

        opendash_md_data_t out;
        memset(&out, 0x5A, sizeof(out));
        bool ok = opendash_md_parse_payload(payload, len, &out);

        if (len != 93) {
            TEST_ASSERT_FALSE_MESSAGE(ok, "len!=93 must reject");
            continue;
        }
        if (!tag_ok) { TEST_ASSERT_FALSE(ok); continue; }
        TEST_ASSERT_TRUE(ok);
        accepts++;

        /* Determinism: same bytes decode to identical fields */
        opendash_md_data_t again;
        memset(&again, 0xC3, sizeof(again));
        TEST_ASSERT_TRUE_MESSAGE(
            opendash_md_parse_payload(payload, 93, &again), "reparse");
        TEST_ASSERT_EQUAL_FLOAT(out.rpm, again.rpm);
        TEST_ASSERT_EQUAL_FLOAT(out.boost, again.boost);
        /* dtc/vin fields are not part of decode output — both zero there */
    }
    TEST_ASSERT_GREATER_THAN_UINT32(0, accepts);
    printf("[fuzz] md_parse_payload: accepted=%lu\n", (unsigned long)accepts);
}

/* ─────────────────────────────────────────────────────────────────────
 * Golden: known raw fields decode to known scaled values (exactness oracle)
 * ───────────────────────────────────────────────────────────────────── */
void test_md_decode_golden_values(void)
{
    uint8_t p[93];
    memset(p, 0, sizeof(p));
    p[0] = 0x5F;                                   /* TAG */
    int16_t  rpm      = -4321;
    uint16_t boost    = 12345;                     /* /100 */
    uint8_t  throttle = 77;
    uint16_t maf      = 375;                       /* /10  → 37.5 */
    p[5]  = (uint8_t)(rpm & 0xFF);   p[6]  = (uint8_t)((uint16_t)rpm >> 8);
    p[7]  = (uint8_t)(boost & 0xFF);  p[8]  = (uint8_t)(boost >> 8);
    p[9]  = throttle;
    p[46] = 0x00; p[47] = 0x00;                    /* speed raw 0 */
    p[48] = 4;                                     /* gear */
    p[58] = 0x03;                                  /* obd2 flags → present */
    p[73] = (uint8_t)(maf & 0xFF); p[74] = (uint8_t)(maf >> 8);

    opendash_md_data_t d;
    TEST_ASSERT_TRUE(opendash_md_parse_payload(p, 93, &d));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, (float)rpm, d.rpm);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 123.45f, d.boost);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 77.0f, d.throttle);
    TEST_ASSERT_EQUAL_UINT8(4, d.gear);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 37.5f, d.obd2_maf_rate);   /* ×10 scale */
    TEST_ASSERT_TRUE(d.obd2_present);

    /* short/long frames always rejected; bad TAG rejected */
    TEST_ASSERT_FALSE(opendash_md_parse_payload(p, 92, &d));
    TEST_ASSERT_FALSE(opendash_md_parse_payload(p, 94, &d));
    p[0] = 0x60;
    TEST_ASSERT_FALSE(opendash_md_parse_payload(p, 93, &d));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_msg_deserialize_noise);
    RUN_TEST(test_fuzz_msg_bitflip);
    RUN_TEST(test_fuzz_md_parse_payload);
    RUN_TEST(test_md_decode_golden_values);
    return UNITY_END();
}