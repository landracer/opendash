/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file test_data_model.c — data store semantics + domain-separation
 * invariants (DATAFLOW.md §4: MD-domain and ECU/OBD-domain ids NEVER share
 * an id, and one domain's value can never occupy the other's slot).
 */
#include "unity.h"
#include "opendash_data_model.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_set_get_roundtrip(void)
{
    opendash_data_store_t s;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_data_init(&s));
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_data_set(&s, OPENDASH_DP_RPM, 4523.0f, 1));

    float v = 0;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_data_get(&s, OPENDASH_DP_RPM, &v));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 4523.0f, v);
}

void test_update_in_place_not_duplicate(void)
{
    opendash_data_store_t s;
    opendash_data_init(&s);
    opendash_data_set(&s, OPENDASH_DP_COOLANT_TEMP, 90.0f, 1);
    opendash_data_set(&s, OPENDASH_DP_COOLANT_TEMP, 92.0f, 2);
    TEST_ASSERT_EQUAL_UINT16(1, s.count);   /* update, not insert */

    float v = 0;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_data_get(&s, OPENDASH_DP_COOLANT_TEMP, &v));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 92.0f, v);
}

void test_not_found(void)
{
    opendash_data_store_t s;
    opendash_data_init(&s);
    float v = 0;
    TEST_ASSERT_EQUAL_INT(OPENDASH_ERR_NOT_FOUND, opendash_data_get(&s, 0x7FFF, &v));
}

void test_store_full(void)
{
    opendash_data_store_t s;
    opendash_data_init(&s);
    for (uint16_t i = 0; i < OPENDASH_MAX_DATA_POINTS; i++)
        TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_data_set(&s, (uint16_t)(0x1000 + i), 1.0f, i));
    TEST_ASSERT_EQUAL_INT(OPENDASH_ERR_NO_MEM, opendash_data_set(&s, 0x7FFF, 1.0f, 1));
}

void test_domains_never_collide(void)
{
    /* The core cross-domain invariant: MD-native ids and shared ECU ids are
     * distinct values — MD_RPM can never populate an ECU-bound widget. */
    TEST_ASSERT_NOT_EQUAL(OPENDASH_DP_RPM,   OPENDASH_DP_MD_RPM);
    TEST_ASSERT_NOT_EQUAL(OPENDASH_DP_RPM,   0x0117); /* sanity of the table */
    TEST_ASSERT_EQUAL_UINT16(0x0117, OPENDASH_DP_MD_RPM);
    TEST_ASSERT_EQUAL_UINT16(0x0100, OPENDASH_DP_RPM);

    /* Both can coexist in one store with independent values (proof by data). */
    opendash_data_store_t s;
    opendash_data_init(&s);
    opendash_data_set(&s, OPENDASH_DP_RPM,    1000.0f, 1);
    opendash_data_set(&s, OPENDASH_DP_MD_RPM, 2000.0f, 1);
    float a = 0, b = 0;
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_data_get(&s, OPENDASH_DP_RPM, &a));
    TEST_ASSERT_EQUAL_INT(OPENDASH_OK, opendash_data_get(&s, OPENDASH_DP_MD_RPM, &b));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1000.0f, a);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2000.0f, b);
    TEST_ASSERT_EQUAL_UINT16(2, s.count);
}

void test_null_args_rejected(void)
{
    opendash_data_store_t s;
    TEST_ASSERT_EQUAL_INT(OPENDASH_ERR_INVALID_ARG, opendash_data_init(NULL));
    opendash_data_init(&s);
    TEST_ASSERT_EQUAL_INT(OPENDASH_ERR_INVALID_ARG, opendash_data_set(NULL, 1, 1, 1));
    TEST_ASSERT_EQUAL_INT(OPENDASH_ERR_INVALID_ARG, opendash_data_get(&s, 1, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_set_get_roundtrip);
    RUN_TEST(test_update_in_place_not_duplicate);
    RUN_TEST(test_not_found);
    RUN_TEST(test_store_full);
    RUN_TEST(test_domains_never_collide);
    RUN_TEST(test_null_args_rejected);
    return UNITY_END();
}
