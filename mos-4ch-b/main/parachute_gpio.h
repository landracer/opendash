/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file parachute_gpio.h
 * @brief MOS-4CH-B — Parachute deploy output GPIO assignment.
 *
 * Per-node deploy pinout for the shared actuator (opendash_parachute.h /
 * opendash_parachute_actuator.c). BOTH MOS nodes carry the deploy code; the
 * end-user may populate only one. Leave GPIO = -1 until the deploy channel is
 * wired on the bench — the actuator stays safely INHIBITED while unassigned.
 *
 * EASY ADD: set OPENDASH_PARACHUTE_DEPLOY_GPIO to the chosen MOS channel GPIO.
 *   MOS-B channels (from main.c mos_config):
 *     CH1=GPIO16  CH2=GPIO17  CH3=GPIO26  CH4=GPIO27
 *   Pick a channel NOT used by the boost PWM output (boost = CH3/GPIO26).
 *
 * @see opendash_parachute.h  (all tunable thresholds — single source of truth)
 * @see TODO.md §11.7         (deployment-system design)
 */

#ifndef MOS_4CH_B_PARACHUTE_GPIO_H
#define MOS_4CH_B_PARACHUTE_GPIO_H

#include "opendash_parachute.h"

/** Deploy output GPIO. -1 = unassigned (actuator inhibited). Set on bench. */
#ifndef OPENDASH_PARACHUTE_DEPLOY_GPIO
#define OPENDASH_PARACHUTE_DEPLOY_GPIO   (-1)
#endif

/** true = drive GPIO HIGH to deploy (MOS-FET modules are active-high). */
#ifndef OPENDASH_PARACHUTE_DEPLOY_ACTIVE_HIGH
#define OPENDASH_PARACHUTE_DEPLOY_ACTIVE_HIGH   true
#endif

#endif /* MOS_4CH_B_PARACHUTE_GPIO_H */
