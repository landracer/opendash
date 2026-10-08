/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file test_roster.c — A+ P1.3: roster gate + vote fusion truth tables.
 * Every gate row both ways, bootstrap one-shot latch, node-id binding,
 * seq wrap safety, TTL expiry, unanimity, manual override, dead-iron state.
 */
#include "unity.h"
#include "opendash_roster.h"
#include "opendash_protocol.h"
#include "opendash_rollover.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const uint8_t MAC_CENTER[6]  = {0xA0,1,2,3,4,5};
static const uint8_t MAC_VOTER[6]   = {0xB0,1,2,3,4,5};
static const uint8_t MAC_STRANGER[6]= {0xC0,1,2,3,4,5};

static opendash_roster_t sealed_roster(void)
{
    opendash_roster_t r;
    memset(&r, 0, sizeof r);
    r.version = OPENDASH_ROSTER_VERSION;
    r.bootstrapped = 1;
    r.count = 2;
    memcpy(r.entry[0].mac, MAC_CENTER, 6);
    r.entry[0].roles = OPENDASH_ROLE_CENTER;
    r.entry[0].node  = 0x01;
    memcpy(r.entry[1].mac, MAC_VOTER, 6);
    r.entry[1].roles = OPENDASH_ROLE_VOTER;
    r.entry[1].node  = 0x03;
    return r;
}

void test_msg_class(void)
{
    TEST_ASSERT_EQUAL_INT(OD_MSG_CLASS_CENTER, opendash_msg_class(OPENDASH_CMD_SET_RELAY));
    TEST_ASSERT_EQUAL_INT(OD_MSG_CLASS_CENTER, opendash_msg_class(OPENDASH_CMD_PARACHUTE_SET_ARM));
    TEST_ASSERT_EQUAL_INT(OD_MSG_CLASS_CENTER, opendash_msg_class(OPENDASH_CMD_PARACHUTE_DEPLOY));
    TEST_ASSERT_EQUAL_INT(OD_MSG_CLASS_CENTER, opendash_msg_class(OPENDASH_CMD_ROSTER_PUSH));
    TEST_ASSERT_EQUAL_INT(OD_MSG_CLASS_VOTE,   opendash_msg_class(OPENDASH_CMD_PARACHUTE_VOTE));
    TEST_ASSERT_EQUAL_INT(OD_MSG_CLASS_INVALID,opendash_msg_class(0x77));
}

void test_bootstrap_center_allow_once_then_sealed(void)
{
    opendash_roster_t r; memset(&r, 0, sizeof r);
    r.version = OPENDASH_ROSTER_VERSION;  /* factory: not bootstrapped */
    /* first center-class sender wins */
    TEST_ASSERT_EQUAL_INT(OD_GATE_ALLOW,
        opendash_gate_decide(&r, MAC_STRANGER, 0x09, OPENDASH_CMD_SET_RELAY));
    /* but votes are NEVER honored pre-bootstrap */
    TEST_ASSERT_EQUAL_INT(OD_GATE_DENY_NOT_ROSTER,
        opendash_gate_decide(&r, MAC_STRANGER, 0x09, OPENDASH_CMD_PARACHUTE_VOTE));
    /* sealed roster: stranger now denied */
    opendash_roster_t s = sealed_roster();
    TEST_ASSERT_EQUAL_INT(OD_GATE_DENY_NOT_ROSTER,
        opendash_gate_decide(&s, MAC_STRANGER, 0x09, OPENDASH_CMD_SET_RELAY));
    /* one-shot latch: a second sender cannot re-latch the center away */
    TEST_ASSERT_EQUAL_INT(OD_GATE_DENY_WRONG_ROLE,
        opendash_gate_decide(&s, MAC_VOTER, 0x03, OPENDASH_CMD_PARACHUTE_SET_ARM));
}

void test_sealed_gate_table(void)
{
    opendash_roster_t s = sealed_roster();
    TEST_ASSERT_EQUAL_INT(OD_GATE_ALLOW,
        opendash_gate_decide(&s, MAC_CENTER, 0x01, OPENDASH_CMD_SET_RELAY));
    /* center-class from voter → WRONG_ROLE (voters cannot command) */
    TEST_ASSERT_EQUAL_INT(OD_GATE_DENY_WRONG_ROLE,
        opendash_gate_decide(&s, MAC_VOTER, 0x03, OPENDASH_CMD_SET_RELAY));
    /* vote from voter with matching node id → ALLOW */
    TEST_ASSERT_EQUAL_INT(OD_GATE_ALLOW,
        opendash_gate_decide(&s, MAC_VOTER, 0x03, OPENDASH_CMD_PARACHUTE_VOTE));
    /* vote from voter MAC but WRONG self-identified node id → denied */
    TEST_ASSERT_EQUAL_INT(OD_GATE_DENY_WRONG_ROLE,
        opendash_gate_decide(&s, MAC_VOTER, 0x07, OPENDASH_CMD_PARACHUTE_VOTE));
    /* vote from center (not a voter) → WRONG_ROLE */
    TEST_ASSERT_EQUAL_INT(OD_GATE_DENY_WRONG_ROLE,
        opendash_gate_decide(&s, MAC_CENTER, 0x01, OPENDASH_CMD_PARACHUTE_VOTE));
    /* invalid opcode → refused outright */
    TEST_ASSERT_EQUAL_INT(OD_GATE_DENY_BAD_ARG,
        opendash_gate_decide(&s, MAC_CENTER, 0x01, 0x77));
    /* NULL args / wrong version → fail-safe deny */
    TEST_ASSERT_EQUAL_INT(OD_GATE_DENY_BAD_ARG,
        opendash_gate_decide(NULL, MAC_CENTER, 0x01, OPENDASH_CMD_SET_RELAY));
    opendash_roster_t bad = s; bad.version = 99;
    TEST_ASSERT_EQUAL_INT(OD_GATE_DENY_BAD_ARG,
        opendash_gate_decide(&bad, MAC_CENTER, 0x01, OPENDASH_CMD_SET_RELAY));
}

void test_sanitize_clamps(void)
{
    opendash_roster_t r; memset(&r, 0, sizeof r);
    r.version = 99; r.count = 200; r.bootstrapped = 7;
    r.entry[0].roles = 0xAA; r.entry[0].reserved = 0xFF;
    opendash_roster_sanitize(&r);
    TEST_ASSERT_EQUAL_UINT8(OPENDASH_ROSTER_VERSION, r.version);
    TEST_ASSERT_EQUAL_UINT8(OPENDASH_ROSTER_MAX, r.count);
    TEST_ASSERT_EQUAL_UINT8(1, r.bootstrapped);
    TEST_ASSERT_EQUAL_UINT8(OPENDASH_ROLE_VOTER, r.entry[0].roles); /* 0xAA&0x03=0x02 */
    TEST_ASSERT_EQUAL_UINT8(0, r.entry[0].reserved);
}

void test_seq_wrap_safe(void)
{
    TEST_ASSERT_EQUAL_INT(true,  opendash_seq_fresh(5, 6));
    TEST_ASSERT_EQUAL_INT(false, opendash_seq_fresh(5, 5));   /* dup   */
    TEST_ASSERT_EQUAL_INT(false, opendash_seq_fresh(5, 4));    /* stale */
    TEST_ASSERT_EQUAL_INT(true,  opendash_seq_fresh(0xFFFFFFFF, 1));        /* wrap */
    TEST_ASSERT_EQUAL_INT(false, opendash_seq_fresh(1, 0xFFFFFFFF));        /* back */
}

void test_fusion_unanimity_and_expiry(void)
{
    opendash_vote_cache_t v[3] = {0};
    for (int i = 0; i < 3; i++) { v[i].valid = true; v[i].rx_us = 1000; }
    int64_t now = 1000;

    /* zero voters pinned ⇒ honest dead iron, even with rolling votes */
    for (int i = 0; i < 3; i++) v[i].rolling = true;
    TEST_ASSERT_FALSE(opendash_fusion_eval(v, 3, 0, now).fire);

    /* 2-of-3 not unanimous → hold */
    v[2].rolling = false;
    opendash_fusion_result_t r2 = opendash_fusion_eval(v, 3, 3, now);
    TEST_ASSERT_FALSE(r2.fire);
    TEST_ASSERT_EQUAL_UINT8(2, r2.rolling_count);

    /* unanimous → fire */
    v[2].rolling = true;
    TEST_ASSERT_TRUE(opendash_fusion_eval(v, 3, 3, now).fire);

    /* TTL: one goes stale → no longer unanimous → hold */
    v[0].rx_us = now - ((int64_t)OPENDASH_ROLLOVER_VOTE_EXPIRY_MS * 1000 + 1);
    TEST_ASSERT_FALSE(opendash_fusion_eval(v, 3, 3, now).fire);

    /* manual override is the hard path */
    v[0].rolling = false; v[1].rolling = false;
    v[1].manual = true;
    TEST_ASSERT_TRUE(opendash_fusion_eval(v, 3, 3, now).fire);

    /* NULL votes → inert */
    opendash_fusion_result_t inert = opendash_fusion_eval(NULL, 0, 3, now);
    TEST_ASSERT_FALSE(inert.fire);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_msg_class);
    RUN_TEST(test_bootstrap_center_allow_once_then_sealed);
    RUN_TEST(test_sealed_gate_table);
    RUN_TEST(test_sanitize_clamps);
    RUN_TEST(test_seq_wrap_safe);
    RUN_TEST(test_fusion_unanimity_and_expiry);
    return UNITY_END();
}