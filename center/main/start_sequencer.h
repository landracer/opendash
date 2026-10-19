/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file start_sequencer.h
 * @brief Center-side declarative START ENGINE sequencer.
 *
 * The cranking/startup routine is DATA, not code: an ordered list of steps,
 * persisted in NVS, executed by a runner task. Step kinds:
 *
 *   SEND     — send SET_RELAY (channel command) to a bound node/channel
 *   WAIT_DP  — block until a data point lands in the dp cache within
 *              [dp_min, dp_max], or fail on timeout (absence of data = fail)
 *   DELAY    — fixed wait
 *
 * Editing the step list happens over the serial console (see main.c "seq"
 * commands); running it is a panel button.
 */

#ifndef OPENDASH_CENTER_START_SEQUENCER_H
#define OPENDASH_CENTER_START_SEQUENCER_H

#include "opendash_common.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Hard cap on programmed steps (NVS blob has fixed size). */
#define OPENDASH_SEQ_MAX_STEPS  8

/** @brief Step kinds. */
typedef enum {
    OPENDASH_SEQ_SEND = 0,    /**< Send SET_RELAY(channel, on) to node */
    OPENDASH_SEQ_WAIT_DP = 1, /**< Wait for dp_id value within [dp_min, dp_max] */
    OPENDASH_SEQ_DELAY = 2,   /**< Fixed delay (delay_100ms × 100 ms) */
} opendash_seq_step_kind_t;

/** @brief One declarative step. Fields used per kind are marked. */
typedef struct {
    uint8_t         kind;         /**< opendash_seq_step_kind_t */
    /* SEND */
    opendash_node_t node;         /**< Target relay/MOS node */
    uint8_t         channel;      /**< Channel to command */
    uint8_t         on;           /**< 1=energize, 0=de-energize */
    /* WAIT_DP */
    uint16_t        dp_id;        /**< Data point id to watch */
    float           dp_min;       /**< Accept if value >= min ... */
    float           dp_max;       /**< ... and <= max (timeout ⇒ fail) */
    uint16_t        timeout_100ms;/**< Wait window in 100 ms units */
    /* DELAY (+ WAIT poll granularity) */
    uint16_t        delay_100ms;  /**< Delay in 100 ms units */
} opendash_seq_step_t;

/** @brief Sequencer state machine. */
typedef enum {
    OPENDASH_SEQ_IDLE = 0,  /**< Not running (defaults/edited) */
    OPENDASH_SEQ_RUNNING,   /**< Runner task executing steps */
    OPENDASH_SEQ_DONE,      /**< All steps satisfied */
    OPENDASH_SEQ_FAILED,    /**< A WAIT_DP timed out — run aborted */
} opendash_seq_state_t;

/** @brief State/progress push into the UI (called from seq task context). */
typedef void (*opendash_seq_notify_fn)(opendash_seq_state_t state,
                                       int step_idx, int step_count);

/** @brief Load step list from NVS (built-in demo program when absent).
 *  Registers the UI notify callback. Call once at boot BEFORE ui_manager_init. */
esp_err_t start_sequencer_init(opendash_seq_notify_fn notify_cb);

/** @brief Run the programmed sequence from step 0. Fails if already running. */
esp_err_t start_sequencer_start(void);

/** @brief Abort a running sequence (IDLE after this); no-op otherwise. */
void start_sequencer_stop(void);

opendash_seq_state_t start_sequencer_state(void);
int start_sequencer_step_index(void);  /* step the runner is on / failed at */
int start_sequencer_step_count(void);
const opendash_seq_step_t *start_sequencer_step(int idx);  /* NULL if out of range */

/* ── Editing (refuse while RUNNING) ────────────────────────────────────── */
esp_err_t start_sequencer_add(const opendash_seq_step_t *step);
esp_err_t start_sequencer_clear(void);
esp_err_t start_sequencer_save(void);   /* persist current list to NVS */

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_CENTER_START_SEQUENCER_H */
