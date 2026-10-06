/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file test_protocol.c — wire codec round-trips and hostile-input handling.
 *
 * Invariants under test (docs/espnow-protocol.md §1):
 *   - frame = SYNC(0xAA) + CMD + LEN + PAYLOAD + XOR CHECKSUM
 *   - bad sync / bad checksum / truncated frame ⇒ always rejected
 *   - a frame that validates is byte-identical after re-serialization
 */
#include "unity.h"
#include "opendash_protocol.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static uint8_t xor_cs(const uint8_t *b, uint16_t n)
{
    uint8_t c = 0;
    for (uint16_t i = 0; i < n; i++) c ^= b[i];
    return c;
}

void test_build_validate_roundtrip(void)
{
    uint8_t payload[6];
    uint16_t id = 0x0117; float v = 1234.5f;
    payload[0] = (uint8_t)(id >> 8); payload[1] = (uint8_t)id;
    memcpy(&payload[2], &v, 4);

    opendash_msg_t msg;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK,
                          opendash_msg_build(&msg, OPENDASH_CMD_DATA_RESPONSE,
                                             payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_UINT8(OPENDASH_MSG_SYNC, msg.sync);
    TEST_ASSERT_EQUAL_UINT8(OPENDASH_CMD_DATA_RESPONSE, msg.cmd);
    TEST_ASSERT_EQUAL_UINT8(sizeof(payload), msg.length);
    TEST_ASSERT_TRUE(msg.checksum == xor_cs((const uint8_t[]){msg.sync, msg.cmd,
                       msg.length, payload[0], payload[1], payload[2],
                       payload[3], payload[4], payload[5]}, 9));
    TEST_ASSERT_TRUE(opendash_msg_validate(&msg));
}

void test_serialize_deserialize_roundtrip(void)
{
    uint8_t pl[] = { 0x01, 0xAA, 0x55, 0x42 };
    opendash_msg_t a, b;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_build(&a, 0x88, pl, sizeof(pl)));

    uint8_t buf[OPENDASH_MSG_MAX_SIZE];
    uint16_t len = 0;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_serialize(&a, buf, &len));
    TEST_ASSERT_EQUAL_UINT16(3 + sizeof(pl) + 1, len);   /* header+payload+cs */

    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_deserialize(buf, len, &b));
    TEST_ASSERT_EQUAL_UINT8(a.sync, b.sync);
    TEST_ASSERT_EQUAL_UINT8(a.cmd, b.cmd);
    TEST_ASSERT_EQUAL_UINT8(a.length, b.length);
    TEST_ASSERT_EQUAL_MEMORY(&a.payload, &b.payload, a.length);
    TEST_ASSERT_EQUAL_UINT8(a.checksum, b.checksum);
}

void test_bad_sync_rejected(void)
{
    uint8_t buf[] = { 0x00, 0x81, 0x01, 0x42, 0x00 };  /* sync != 0xAA */
    opendash_msg_t m;
    TEST_ASSERT_NOT_EQUAL(OPENDASH_OK, opendash_msg_deserialize(buf, sizeof(buf), &m));
}

void test_corrupted_payload_byte_rejected(void)
{
    opendash_msg_t a;
    uint8_t pl[] = { 0x11, 0x22 };
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_build(&a, 0x01, pl, sizeof(pl)));
    uint8_t buf[16]; uint16_t len = 0;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_serialize(&a, buf, &len));

    buf[3] ^= 0xFF;   /* flip one payload byte — checksum must catch it */
    opendash_msg_t m;
    TEST_ASSERT_EQUAL_INT(OPENDASH_ERR_CHECKSUM,
                          opendash_msg_deserialize(buf, len, &m));
}

void test_truncated_frame_rejected(void)
{
    opendash_msg_t a; uint8_t pl[] = { 1, 2, 3 };
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_build(&a, 0x88, pl, sizeof(pl)));
    uint8_t buf[16]; uint16_t len = 0;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_serialize(&a, buf, &len));

    opendash_msg_t m;
    /* short than the minimum 4-byte frame */
    TEST_ASSERT_NOT_EQUAL(OPENDASH_OK, opendash_msg_deserialize(buf, 3, &m));
    /* declared payload longer than the buffer actually carries */
    TEST_ASSERT_NOT_EQUAL(OPENDASH_OK, opendash_msg_deserialize(buf, 4, &m));
}

void test_max_payload_boundary(void)
{
    opendash_msg_t a;
    uint8_t pl[OPENDASH_MSG_MAX_PAYLOAD];
    for (int i = 0; i < (int)sizeof(pl); i++) pl[i] = (uint8_t)i;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_build(&a, 0x88, pl, sizeof(pl)));
    uint8_t buf[OPENDASH_MSG_MAX_SIZE]; uint16_t len = 0;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_serialize(&a, buf, &len));
    TEST_ASSERT_EQUAL_UINT16(OPENDASH_MSG_MAX_SIZE, len);

    opendash_msg_t m;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_deserialize(buf, len, &m));
    TEST_ASSERT_EQUAL_UINT8(OPENDASH_MSG_MAX_PAYLOAD, m.length);
}

void test_zero_length_payload(void)
{
    opendash_msg_t a;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_build(&a, 0x29, NULL, 0));
    uint8_t buf[8]; uint16_t len = 0;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_serialize(&a, buf, &len));
    TEST_ASSERT_EQUAL_UINT16(4, len);   /* SYNC CMD LEN CS */
    opendash_msg_t m;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_msg_deserialize(buf, len, &m));
    TEST_ASSERT_EQUAL_UINT8(0, m.length);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_validate_roundtrip);
    RUN_TEST(test_serialize_deserialize_roundtrip);
    RUN_TEST(test_bad_sync_rejected);
    RUN_TEST(test_corrupted_payload_byte_rejected);
    RUN_TEST(test_truncated_frame_rejected);
    RUN_TEST(test_max_payload_boundary);
    RUN_TEST(test_zero_length_payload);
    return UNITY_END();
}
