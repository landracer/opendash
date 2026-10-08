/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_roster.h
 * @brief OpenDash Roster Policy Gate — shared wire types + pure decision core
 *        (A+ program P1.3, owner-ratified design: docs/POLICY_GATE.md)
 *
 * The trust model since ruling D4: every safety-capable node pins a ROSTER
 * (its center + its designated gyro/IMU 3D-spatial voters). Control frames are
 * accepted ONLY from roster members, per opcode class. Silence never disarms
 * while powered; a power-loss reboot legitimately comes up DISARMED (standard),
 * and cross-reboot persistence is an opt-in user choice (PERSIST_ARM), never
 * built-in. Pairing has NO hardware button: fresh nodes run in bootstrap
 * ("first center-class sender wins"), after which only roster members mutate
 * anything. All functions here are PURE — node wiring and NVS live elsewhere;
 * this module's full truth tables run in the host test suite (test/test_roster.c).
 */

#ifndef OPENDASH_ROSTER_H
#define OPENDASH_ROSTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Wire opcode (M→S, center-class): push/replace a node's roster blob. */
#define OPENDASH_CMD_ROSTER_PUSH 0x2C

/** Roster schema version (bump on field/semantics change). */
#define OPENDASH_ROSTER_VERSION 1

/** Fixed capacity — four roster entries per node today. */
#define OPENDASH_ROSTER_MAX 4u

/* Role bits (entry.roles). A peer may hold both. */
#define OPENDASH_ROLE_CENTER (1u << 0)  /**< may issue center-class control    */
#define OPENDASH_ROLE_VOTER  (1u << 1)  /**< gyro/IMU node whose VOTE is obeyed */
#define OPENDASH_ROLE_MASK   (OPENDASH_ROLE_CENTER | OPENDASH_ROLE_VOTER)

/** @brief One roster member: self-identified node id + roles + pinned MAC. */
typedef struct __attribute__((packed)) {
    uint8_t node;        /**< opendash_node_t the peer self-identifies as */
    uint8_t roles;       /**< OPENDASH_ROLE_* bits                         */
    uint8_t reserved;    /**< pad (0)                                      */
    uint8_t mac[6];      /**< pinned source MAC                            */
} opendash_roster_entry_t;

/** @brief NVS-persisted trust set of one node (wire payload for ROSTER_PUSH). */
typedef struct __attribute__((packed)) {
    uint8_t version;      /**< OPENDASH_ROSTER_VERSION  */
    uint8_t count;        /**< valid entries in entry[] (0..MAX) */
    uint8_t bootstrapped; /**< 1 = center latched; bootstrap pass CLOSED */
    uint8_t reserved;     /**< pad (0)                                   */
    opendash_roster_entry_t entry[OPENDASH_ROSTER_MAX];
} opendash_roster_t;

/* ── Gate ─────────────────────────────────────────────────────────────── */

/** @brief Message classes the gate knows about. Anything else is refused. */
typedef enum {
    OD_MSG_CLASS_INVALID = 0, /**< unknown opcode — never honored      */
    OD_MSG_CLASS_CENTER,      /**< center-issued control (ROLE_CENTER) */
    OD_MSG_CLASS_VOTE,        /**< detector vote (ROLE_VOTER)          */
} opendash_msg_class_t;

/** @brief Verdict of the roster gate for one inbound frame. */
typedef enum {
    OD_GATE_ALLOW = 0,        /**< sender's role permits this class */
    OD_GATE_DENY_BAD_ARG,    /**< NULL args / wrong roster version  */
    OD_GATE_DENY_NOT_ROSTER, /**< sender MAC is not on the roster   */
    OD_GATE_DENY_WRONG_ROLE, /**< on roster, but role forbids class */
} opendash_gate_decision_t;

/** Classify an opcode (pure). Unlisted opcodes are INVALID. */
opendash_msg_class_t opendash_msg_class(uint8_t opcode);

/** Stamp version, clamp count to MAX, mask unknown role bits, zero reserved.
 *  Idempotent. Wiring calls this on every blob that enters NVS. */
void opendash_roster_sanitize(opendash_roster_t *r);

/** Find the entry whose pinned MAC matches @p mac. NULL if absent. */
const opendash_roster_entry_t *opendash_roster_find(const opendash_roster_t *r,
                                                    const uint8_t mac[6]);

/**
 * @brief THE gate (pure): may a frame from @p src_mac self-identifying as
 *        @p src_node carry @p opcode against this roster?
 *
 * Bootstrap semantics: while r->bootstrapped == 0, center-class frames are
 * ALLOWed from ANY sender ("first sender wins", the factory path — the caller
 * latches the sender as ROLE_CENTER and sets bootstrapped=1; that latch is the
 * ONLY bootstrap mutation and can never happen twice). Votes are NEVER honored
 * before bootstrap. After bootstrap: MAC must match an entry, and that entry's
 * roles must permit the class; votes must also match the pinned node id.
 */
opendash_gate_decision_t opendash_gate_decide(const opendash_roster_t *r,
                                              const uint8_t src_mac[6],
                                              uint8_t src_node,
                                              uint8_t opcode);

/* ── Vote fusion (MOS-local mirror of the center's rule) ────────────────── */

/** Per-sender cached vote state (wiring owns storage; fusion owns decision). */
typedef struct {
    bool     valid;   /**< entry in use                                        */
    bool     rolling; /**< last vote: local roll-detect active                 */
    bool     manual;  /**< last vote: operator manual release held             */
    uint8_t  node;    /**< voter node id (OPENDASH_NODE_* self-identification) */
    uint32_t seq;     /**< last accepted seq (dedupe)                          */
    int64_t  rx_us;   /**< esp_timer us at accept; freshness judged by TTL     */
} opendash_vote_cache_t;

/** @brief Result of one fusion evaluation tick. */
typedef struct {
    bool    fire;          /**< TRUE = deploy criteria met (verdict still gates) */
    bool    manual;        /**< a fresh manual vote exists                        */
    uint8_t rolling_count; /**< fresh rolling votes counted                       */
} opendash_fusion_result_t;

/** Monotonic-seq freshness, wrap-safe: TRUE iff rx is strictly newer than last. */
bool opendash_seq_fresh(uint32_t last, uint32_t rx);

/**
 * @brief Pure deploy-criteria evaluation: fresh votes only (TTL from
 *        opendash_rollover.h), fire = fresh manual override OR unanimous
 *        rolling across exactly voter_count voters. voter_count == 0 (no
 *        voters pinned) ⇒ auto-fire can never originate — honest dead iron.
 *        The P1.2 fire verdict STILL gates the actual energize.
 */
opendash_fusion_result_t opendash_fusion_eval(const opendash_vote_cache_t *votes,
                                             size_t n,
                                             uint8_t voter_count,
                                             int64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_ROSTER_H */