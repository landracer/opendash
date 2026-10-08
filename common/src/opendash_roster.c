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

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "opendash_protocol.h"
#include "opendash_rollover.h"

static const char *TAG = "roster";

opendash_msg_class_t opendash_msg_class(uint8_t opcode)
{
    switch (opcode) {
        /* center-issued control */
        case OPENDASH_CMD_SYSTEM:
        case OPENDASH_CMD_SET_RELAY:
        case OPENDASH_CMD_REQUEST_RELAY_STATUS:
        case OPENDASH_CMD_BOOST_LIVE_DATA:
        case OPENDASH_CMD_BOOST_SET_PARAMS:
        case OPENDASH_CMD_BOOST_SET_MODE:
        case OPENDASH_CMD_BOOST_SET_DUTY_ROW:
        case OPENDASH_CMD_BOOST_SET_SETP_ROW:
        case OPENDASH_CMD_BOOST_SET_THROTTLE:
        case OPENDASH_CMD_BOOST_PULL_ALL:
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

bool opendash_roster_bootstrap_eligible(uint8_t opcode)
{
    /* Exactly the old dispatch's is_center_cmd whitelist: frames that are
     * unicast center intent by construction. SYSTEM (GPS time-sync broadcast)
     * and BOOST are center-class but must NEVER bootstrap-latch a center. */
    switch (opcode) {
        case OPENDASH_CMD_SET_RELAY:
        case OPENDASH_CMD_REQUEST_RELAY_STATUS:
        case OPENDASH_CMD_PARACHUTE_SET_CONFIG:
        case OPENDASH_CMD_PARACHUTE_SET_ARM:
        case OPENDASH_CMD_PARACHUTE_PULL_ALL:
        case OPENDASH_CMD_PARACHUTE_DEPLOY:
        case OPENDASH_CMD_PARACHUTE_CALIBRATE:
        case OPENDASH_CMD_ROSTER_PUSH:
            return true;
        default:
            return false;
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
        /* Bootstrap: only ELIGIBLE frames pierce an unsealed roster (the
         * factory path). Caller latches sender + sets bootstrapped=1. */
        if (!r->bootstrapped)
            return opendash_roster_bootstrap_eligible(opcode)
                       ? OD_GATE_ALLOW : OD_GATE_DENY_NOT_ROSTER;
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

/* ────────────────────────────────────────────────────────────────────────
 * Node-side store — NVS-persisted roster + RAM cache (mirrors the parachute
 * config store discipline). Wiring (node mains) owns mutation via _set().
 * ──────────────────────────────────────────────────────────────────────── */

#define OD_ROSTER_NVS_NS  "roster"
#define OD_ROSTER_NVS_KEY "list"

static SemaphoreHandle_t s_od_roster_lock;
static opendash_roster_t s_od_roster;

static inline void od_lock(void)   { if (s_od_roster_lock) xSemaphoreTake(s_od_roster_lock, portMAX_DELAY); }
static inline void od_unlock(void) { if (s_od_roster_lock) xSemaphoreGive(s_od_roster_lock); }

static void od_roster_save_locked(const opendash_roster_t *r)
{
    nvs_handle_t h;
    if (nvs_open(OD_ROSTER_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(rw) failed — roster not persisted");
        return;
    }
    if (nvs_set_blob(h, OD_ROSTER_NVS_KEY, r, sizeof(*r)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

esp_err_t opendash_roster_store_init(void)
{
    if (!s_od_roster_lock) {
        s_od_roster_lock = xSemaphoreCreateMutex();
        if (!s_od_roster_lock) return ESP_ERR_NO_MEM;
    }
    opendash_roster_t r; memset(&r, 0, sizeof r);
    nvs_handle_t h;
    if (nvs_open(OD_ROSTER_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(r);
        if (nvs_get_blob(h, OD_ROSTER_NVS_KEY, &r, &len) != ESP_OK ||
            len != sizeof(r) || r.version != OPENDASH_ROSTER_VERSION) {
            memset(&r, 0, sizeof r);   /* absent/corrupt/wrong-version ⇒ factory-fresh */
        }
        nvs_close(h);
        ESP_LOGI(TAG, "Loaded persisted roster (bootstrapped=%u count=%u)",
                 r.bootstrapped, r.count);
    } else {
        memset(&r, 0, sizeof r);
        ESP_LOGI(TAG, "No saved roster — factory-fresh (bootstrap open)");
    }
    opendash_roster_sanitize(&r);
    od_lock(); s_od_roster = r; od_unlock();
    return ESP_OK;
}

esp_err_t opendash_roster_store_get(opendash_roster_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    od_lock(); *out = s_od_roster; od_unlock();
    return ESP_OK;
}

esp_err_t opendash_roster_store_set(const opendash_roster_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    opendash_roster_t r = *in;
    opendash_roster_sanitize(&r);
    od_lock();
    s_od_roster = r;
    od_roster_save_locked(&s_od_roster);
    od_unlock();
    return ESP_OK;
}