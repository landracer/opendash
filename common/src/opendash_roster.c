/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_roster.c
 * @brief Roster gate + vote fusion — PURE decision core (A+ P1.3).
 *
 * No NVS, no ESP-NOW, no timers in here: wiring feeds state in, reads verdicts
 * out, and owns persistence. That is deliberate — every branch of the safety
 * decision is table-tested on the host (test/test_roster.c), same discipline
 * that made the P1.2 fire verdict bulletproof.
 */

#include "opendash_roster.h"

#include <string.h>

#include "opendash_protocol.h"
#include "opendash_rollover.h"

opendash_msg_class_t opendash_msg_class(uint8_t opcode)
{
    switch (opcode) {
        /* center-issued control */
        case OPENDASH_CMD_SET_RELAY:
        case OPENDASH_CMD_REQUEST_RELAY_STATUS:
        case OPENDASH_CMD_PARACHUTE_SET_CONFIG:
        case OPENDASH_CMD_PARACHUTE_SET_ARM:
        case OPENDASH_CMD_PARACHUTE_PULL_ALL:
        case OPENDASH_CMD_PARACHUTE_DEPLOY:
        case OPENDASH_CMD_PARACHUTE_CALIBRATE:
        case OPENDASH_CMD_ROSTER_PUSH:
            return OD_MSG_CLASS_CENTER;
        /* detector vote */
        case OPENDASH_CMD_PARACHUTE_VOTE:
            return OD_MSG_CLASS_VOTE;
        default:
            return OD_MSG_CLASS_INVALID;
    }
}

void opendash_roster_sanitize(opendash_roster_t *r)
{
    if (!r) return;
    r->version = OPENDASH_ROSTER_VERSION;
    if (r->count > OPENDASH_ROSTER_MAX) r->count = OPENDASH_ROSTER_MAX;
    r->bootstrapped = r->bootstrapped ? 1 : 0;
    r->reserved     = 0;
    for (uint8_t i = 0; i < OPENDASH_ROSTER_MAX; i++) {
        r->entry[i].roles   &= OPENDASH_ROLE_MASK;
        r->entry[i].reserved = 0;
    }
}

const opendash_roster_entry_t *opendash_roster_find(const opendash_roster_t *r,
                                                    const uint8_t mac[6])
{
    if (!r || !mac) return NULL;
    for (uint8_t i = 0; i < r->count && i < OPENDASH_ROSTER_MAX; i++) {
        if (memcmp(r->entry[i].mac, mac, 6) == 0) return &r->entry[i];
    }
    return NULL;
}

opendash_gate_decision_t opendash_gate_decide(const opendash_roster_t *r,
                                              const uint8_t src_mac[6],
                                              uint8_t src_node,
                                              uint8_t opcode)
{
    if (!r || !src_mac)                        return OD_GATE_DENY_BAD_ARG;
    if (r->version != OPENDASH_ROSTER_VERSION) return OD_GATE_DENY_BAD_ARG;

    const opendash_msg_class_t cls = opendash_msg_class(opcode);

    if (cls == OD_MSG_CLASS_CENTER) {
        /* Bootstrap: factory-fresh node honors the first center-class sender
         * and is then sealed (caller latches + sets bootstrapped). */
        if (!r->bootstrapped) return OD_GATE_ALLOW;
        const opendash_roster_entry_t *e = opendash_roster_find(r, src_mac);
        if (!e) return OD_GATE_DENY_NOT_ROSTER;
        return (e->roles & OPENDASH_ROLE_CENTER) ? OD_GATE_ALLOW
                                                 : OD_GATE_DENY_WRONG_ROLE;
    }

    if (cls == OD_MSG_CLASS_VOTE) {
        /* Votes never ride bootstrap — a voter must be pinned first. */
        const opendash_roster_entry_t *e = opendash_roster_find(r, src_mac);
        if (!e) return OD_GATE_DENY_NOT_ROSTER;
        if (!(e->roles & OPENDASH_ROLE_VOTER)) return OD_GATE_DENY_WRONG_ROLE;
        /* bind self-identified node id to the pinned entry */
        return (e->node == src_node) ? OD_GATE_ALLOW : OD_GATE_DENY_WRONG_ROLE;
    }

    return OD_GATE_DENY_BAD_ARG;  /* ungated class: refuse, do not guess */
}

bool opendash_seq_fresh(uint32_t last, uint32_t rx)
{
    return (int32_t)(rx - last) > 0;   /* wrap-safe monotonic compare */
}

opendash_fusion_result_t opendash_fusion_eval(const opendash_vote_cache_t *votes,
                                              size_t n,
                                              uint8_t voter_count,
                                              int64_t now_us)
{
    opendash_fusion_result_t res = { .fire = false, .manual = false,
                                     .rolling_count = 0 };
    if (!votes) return res;

    const int64_t expiry_us = (int64_t)OPENDASH_ROLLOVER_VOTE_EXPIRY_MS * 1000;
    for (size_t i = 0; i < n; i++) {
        const opendash_vote_cache_t *v = &votes[i];
        if (!v->valid) continue;
        if ((now_us - v->rx_us) > expiry_us) continue;  /* stale = no vote */
        if (v->rolling) res.rolling_count++;
        if (v->manual)  res.manual = true;
    }
    /* Unanimity among pinned voters; voter_count==0 ⇒ never originates. */
    res.fire = res.manual || (voter_count > 0 &&
                              res.rolling_count == voter_count);
    return res;
}