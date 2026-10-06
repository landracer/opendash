/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file test_parachute.c — deployment config store: safe defaults, hostile
 * config sanitization (a bad frame must NEVER install a nonsense threshold),
 * and NVS round-trip (simulated reboot re-load).
 */
#include "unity.h"
#include "opendash_parachute.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <math.h>
#include <string.h>

void od_stub_clock_reset(void);
void od_stub_clock_advance_ms(uint32_t ms);
void od_stub_nvs_reset(void);

void setUp(void) {}
void tearDown(void) {}

void test_defaults_installed_on_first_boot(void)
{
    od_stub_nvs_reset();
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_flash_init());
    TEST_ASSERT_EQUAL_INT(ESP_OK, opendash_parachute_config_init());

    opendash_parachute_config_t c;
    TEST_ASSERT_EQUAL_INT(ESP_OK, opendash_parachute_config_get(&c));
    TEST_ASSERT_EQUAL_UINT8(OPENDASH_PARACHUTE_CONFIG_VERSION, c.version);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, OPENDASH_PARACHUTE_MIN_SPEED_MPH, c.min_speed_mph);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, OPENDASH_PARACHUTE_ROLL_DEPLOY_DEG, c.roll_deploy_deg);
    TEST_ASSERT_EQUAL_UINT16(OPENDASH_PARACHUTE_ROLL_SUSTAIN_MS, c.sustain_ms);
    TEST_ASSERT_EQUAL_UINT16(OPENDASH_PARACHUTE_DEPLOY_PULSE_MS, c.pulse_ms);
}

void test_hostile_config_is_sanitized(void)
{
    opendash_parachute_config_t junk;
    memset(&junk, 0, sizeof(junk));
    junk.version         = 99;                    /* must be forced to v1 */
    junk.enabled         = 7;                     /* must clamp to 1 */
    junk.channel_mask    = 0xA5;                  /* keep low nibble -> 0x05 */
    junk.flags           = 0xAA;                  /* 0xAA & FLAG_MASK(0x03) = 0x02 */
    junk.min_speed_mph   = -5.0f;                 /* negative -> default */
    junk.roll_deploy_deg = 1e9f;                  /* absurd -> clamped */
    junk.roll_rate_deg_s = -1.0f;                 /* negative -> default */
    junk.sustain_ms      = 65000;                 /* -> 5000 max */
    junk.pulse_ms        = 65000;                 /* -> 10000 max */

    TEST_ASSERT_EQUAL_INT(ESP_OK, opendash_parachute_config_set(&junk));

    opendash_parachute_config_t c;
    TEST_ASSERT_EQUAL_INT(ESP_OK, opendash_parachute_config_get(&c));
    TEST_ASSERT_EQUAL_UINT8(OPENDASH_PARACHUTE_CONFIG_VERSION, c.version);
    TEST_ASSERT_EQUAL_UINT8(1, c.enabled);
    TEST_ASSERT_EQUAL_UINT8(0x05, c.channel_mask);   /* 0xA5 & 0x0F */
    TEST_ASSERT_EQUAL_UINT8(OPENDASH_PARACHUTE_FLAG_AUTO_DETECT, c.flags); /* 0xAA & 0x03 = 0x02 */
    /* NaN poisoning: sanitize's `!(x >= 0)` guard must catch non-finite junk */
    opendash_parachute_config_t nan_probe = c;
    nan_probe.min_speed_mph = NAN;
    TEST_ASSERT_EQUAL_INT(ESP_OK, opendash_parachute_config_set(&nan_probe));
    opendash_parachute_config_get(&c);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, OPENDASH_PARACHUTE_MIN_SPEED_MPH, c.min_speed_mph);
    TEST_ASSERT_TRUE(c.roll_deploy_deg <= 180.0f);
    TEST_ASSERT_TRUE(c.sustain_ms <= 5000);
    TEST_ASSERT_TRUE(c.pulse_ms <= 10000);
}

void test_null_args_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, opendash_parachute_config_get(NULL));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, opendash_parachute_config_set(NULL));
}

void test_reboot_reloads_persisted_config(void)
{
    opendash_parachute_config_t c;
    opendash_parachute_config_get(&c);
    c.min_speed_mph = 42.0f;
    c.enabled = 1;
    TEST_ASSERT_EQUAL_INT(ESP_OK, opendash_parachute_config_set(&c));

    /* Simulated reboot: init again loads the blob back from fake-NVS */
    TEST_ASSERT_EQUAL_INT(ESP_OK, opendash_parachute_config_init());
    opendash_parachute_config_t reread;
    TEST_ASSERT_EQUAL_INT(ESP_OK, opendash_parachute_config_get(&reread));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 42.0f, reread.min_speed_mph);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_installed_on_first_boot);
    RUN_TEST(test_hostile_config_is_sanitized);
    RUN_TEST(test_null_args_rejected);
    RUN_TEST(test_reboot_reloads_persisted_config);
    return UNITY_END();
}
