/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file test_node_health.c — node health state machine, on the host.
 *
 * HOW TO READ THIS FILE: node_health.c is the real firmware code, compiled
 * into a plain Linux program together with fake NVS and a TEST-CONTROLLED
 * virtual clock (stubs/stubs.c). od_stub_clock_advance_ms(61000) is how we
 * make "61 seconds passed" happen in zero wall-clock seconds. Every
 * node_health_rx()/ack()/evaluate() call below is the exact same function
 * center calls on the vehicle.
 *
 * Invariants under test (docs/espnow-protocol.md §4):
 *   - ANY data ⇒ immediate ONLINE
 *   - freq-mode node: 3 consecutive silent windows ⇒ OFFLINE; 2 ⇒ DEGRADED
 *   - ACK upgrades OFFLINE ⇒ DEGRADED (radio alive)
 *   - heartbeat-mode node: honest silence — ~2 missed heartbeats ⇒ DEGRADED,
 *     ~4 ⇒ OFFLINE; being heard again restores ONLINE (fleet decision
 *     2026-10-06, replacing the old "once heard, ONLINE forever" sticky rule
 *     that showed powered-off boards as active)
 *   - AWAITING + never heard + boot grace expired ⇒ OFFLINE; NVS restore
 *     always comes back AWAITING (a stored MAC is not proof of life)
 */
#include "unity.h"
#include "node_health.h"
#include "opendash_common.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <string.h>

/* test-stub controls (defined in stubs.c) */
void od_stub_clock_reset(void);
void od_stub_clock_advance_ms(uint32_t ms);
void od_stub_nvs_reset(void);

static const uint8_t MAC_LEFT[6]  = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x61 };
static const uint8_t MAC_RIGHT[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x62 };

/* Virtual clock + fake NVS start empty at process start; the suites are a
 * sequential story, so no reset between tests by design. */
void setUp(void) {}
void tearDown(void) {}

void test_init_then_awaiting_after_register(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_flash_init());
    TEST_ASSERT_EQUAL_INT(ESP_OK, node_health_init());
    TEST_ASSERT_EQUAL(NODE_STATE_UNKNOWN, node_health_get_state(OPENDASH_NODE_LEFT));
    node_health_register_mac(OPENDASH_NODE_LEFT, MAC_LEFT);
    TEST_ASSERT_EQUAL(NODE_STATE_AWAITING, node_health_get_state(OPENDASH_NODE_LEFT));
}

void test_freq_mode_online_degraded_offline_online(void)
{
    node_health_register_mac(OPENDASH_NODE_LEFT, MAC_LEFT);
    od_stub_clock_advance_ms(1000);
    node_health_rx(OPENDASH_NODE_LEFT, -50);
    TEST_ASSERT_EQUAL(NODE_STATE_ONLINE, node_health_get_state(OPENDASH_NODE_LEFT));
    TEST_ASSERT_TRUE(node_health_is_alive(OPENDASH_NODE_LEFT));

    /* window with data = still ONLINE */
    od_stub_clock_advance_ms(1000);
    node_health_rx(OPENDASH_NODE_LEFT, -50);
    node_health_evaluate();
    TEST_ASSERT_EQUAL(NODE_STATE_ONLINE, node_health_get_state(OPENDASH_NODE_LEFT));

    /* window 2 silent -> still ONLINE (missed=1); window 3 -> DEGRADED */
    od_stub_clock_advance_ms(1000);
    node_health_evaluate();
    TEST_ASSERT_EQUAL(NODE_STATE_ONLINE, node_health_get_state(OPENDASH_NODE_LEFT));
    od_stub_clock_advance_ms(1000);
    node_health_evaluate();
    TEST_ASSERT_EQUAL(NODE_STATE_DEGRADED, node_health_get_state(OPENDASH_NODE_LEFT));
    TEST_ASSERT_TRUE(node_health_is_alive(OPENDASH_NODE_LEFT)); /* degraded = alive */

    /* window 4 silent -> missed=3 -> OFFLINE */
    od_stub_clock_advance_ms(1000);
    node_health_evaluate();
    TEST_ASSERT_EQUAL(NODE_STATE_OFFLINE, node_health_get_state(OPENDASH_NODE_LEFT));
    TEST_ASSERT_FALSE(node_health_is_alive(OPENDASH_NODE_LEFT));

    /* ACK upgrades OFFLINE -> DEGRADED (radio alive) */
    node_health_ack(OPENDASH_NODE_LEFT);
    TEST_ASSERT_EQUAL(NODE_STATE_DEGRADED, node_health_get_state(OPENDASH_NODE_LEFT));
    TEST_ASSERT_TRUE(node_health_is_alive(OPENDASH_NODE_LEFT));
}

void test_heartbeat_mode_honest_timeout(void)
{
    node_health_register_mac(OPENDASH_NODE_RIGHT, MAC_RIGHT);
    node_health_rx(OPENDASH_NODE_RIGHT, -70);   /* one announcement */
    TEST_ASSERT_EQUAL(NODE_STATE_ONLINE, node_health_get_state(OPENDASH_NODE_RIGHT));

    /* Silence past ~2 missed heartbeats -> honest DEGRADED, not "active". */
    od_stub_clock_advance_ms(NODE_HEALTH_HEARTBEAT_DEGRADED_MS + 1000);
    node_health_evaluate();
    TEST_ASSERT_EQUAL(NODE_STATE_DEGRADED, node_health_get_state(OPENDASH_NODE_RIGHT));
    TEST_ASSERT_TRUE(node_health_is_alive(OPENDASH_NODE_RIGHT)); /* degraded = alive */

    /* Silence past ~4 missed heartbeats -> honest OFFLINE. This is the case
     * the old sticky-ONLINE rule could never show: a powered-off board. */
    od_stub_clock_advance_ms(NODE_HEALTH_HEARTBEAT_OFFLINE_MS
                             - NODE_HEALTH_HEARTBEAT_DEGRADED_MS + 1000);
    node_health_evaluate();
    TEST_ASSERT_EQUAL(NODE_STATE_OFFLINE, node_health_get_state(OPENDASH_NODE_RIGHT));
    TEST_ASSERT_FALSE(node_health_is_alive(OPENDASH_NODE_RIGHT));

    /* Heard again -> ONLINE immediately (rx refreshes the silence clock). */
    node_health_rx(OPENDASH_NODE_RIGHT, -60);
    TEST_ASSERT_EQUAL(NODE_STATE_ONLINE, node_health_get_state(OPENDASH_NODE_RIGHT));
}

void test_never_seen_node_goes_offline_after_grace(void)
{
    /* POD3 (heartbeat mode) never registered and never heard: stays UNKNOWN
     * (skipped by evaluate) and is not alive. */
    TEST_ASSERT_FALSE(node_health_is_alive(OPENDASH_NODE_POD3));
    TEST_ASSERT_EQUAL(NODE_STATE_UNKNOWN, node_health_get_state(OPENDASH_NODE_POD3));

    /* Registered (AWAITING) but never heard + boot grace expired -> OFFLINE */
    static const uint8_t mac[6] = {1,2,3,4,5,6};
    node_health_register_mac(OPENDASH_NODE_POD4, mac);
    TEST_ASSERT_EQUAL(NODE_STATE_AWAITING, node_health_get_state(OPENDASH_NODE_POD4));
    /* boot clock is at 0 + advance beyond the 60 s grace */
    od_stub_clock_advance_ms(61000);
    node_health_evaluate();
    TEST_ASSERT_EQUAL(NODE_STATE_OFFLINE, node_health_get_state(OPENDASH_NODE_POD4));
}

void test_find_by_mac_and_bounds(void)
{
    node_health_register_mac(OPENDASH_NODE_LEFT, MAC_LEFT);
    TEST_ASSERT_EQUAL(OPENDASH_NODE_LEFT, node_health_find_by_mac(MAC_LEFT));

    static const uint8_t unknown[6] = {9,9,9,9,9,9};
    TEST_ASSERT_EQUAL(OPENDASH_NODE_COUNT, node_health_find_by_mac(unknown));

    /* out-of-range node ids must be safely ignored, not crash */
    node_health_rx(OPENDASH_NODE_COUNT, 0);
    node_health_ack(OPENDASH_NODE_COUNT);
    TEST_ASSERT_EQUAL(NODE_STATE_UNKNOWN, node_health_get_state(OPENDASH_NODE_COUNT));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_then_awaiting_after_register);
    RUN_TEST(test_freq_mode_online_degraded_offline_online);
    RUN_TEST(test_heartbeat_mode_honest_timeout);
    RUN_TEST(test_never_seen_node_goes_offline_after_grace);
    RUN_TEST(test_find_by_mac_and_bounds);
    return UNITY_END();
}
