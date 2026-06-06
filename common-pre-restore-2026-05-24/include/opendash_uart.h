/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_uart.h
 * @brief UART receiver for multidisplay serial data (SERIALOUT_BINARY format)
 *
 * Architecture: This module lives in common/ and has NO display-specific
 * dependencies. It reads UART data, parses the multidisplay serial format,
 * and updates the shared opendash_data_store. Display-specific actions
 * (UI labels, buzzer) must be handled by the caller in main.c.
 *
 * Multidisplay SERIALOUT_BINARY wire format:
 *   STX TAG time[4] RPM[2] boost[2] throttle[1] lambda[2] LMM[2] casetemp[2]
 *       EGT0-7[16] batV[2] VDOp1-3[6] VDOt1-3[6] speed[2] gear[1] N75[1]
 *       reqBoost[2] reqPWM[1] flags[1] efrSpeed[2] knock[2] reserved[35] ETX
 *
 *   Total: 95 bytes (STX + 93 payload + ETX)
 *   TAG = 0x5F (95), all multi-byte fields are little-endian
 *   NO byte-stuffing — 0x02/0x03 can appear in data fields
 *
 * HC-05 note: The HC-05 must be pre-configured as master via external AT
 * commands.  This module operates in passive listen mode on the configured
 * UART RX pin.
 */

#ifndef OPENDASH_UART_H
#define OPENDASH_UART_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Compile-time toggle ──────────────────────────────────────────────────
 * Set to 0 to completely disable multidisplay UART at compile time.       */
#ifndef OPENDASH_MULTIDISPLAY_CONNECTION
#define OPENDASH_MULTIDISPLAY_CONNECTION  1
#endif

/* ── Bluetooth target device name ─────────────────────────────────────────
 * The HC-05 inquiry scans for nearby devices and matches this name.
 * Change this to whatever your multidisplay HC-06 is broadcasting as.
 * Examples: "mdv2", "MD02", "rAtTrax", "multidisplay"                    */
#ifndef OPENDASH_MD_BT_NAME
#define OPENDASH_MD_BT_NAME  "mdv2"
#endif

/* ── Serial debug logging ─────────────────────────────────────────────────
 * Set to 1 to enable verbose UART debug output on the serial monitor.
 * Shows raw bytes, frame parsing, field values, status transitions.       */
#ifndef OPENDASH_UART_DEBUG
#define OPENDASH_UART_DEBUG  0
#endif

/* ── Demo data toggle ─────────────────────────────────────────────────────
 * Set to 1 to enable demo sweep data (drag race simulation).
 * Set to 0 to disable demo data and only show real sensor values.
 * Useful for bench testing without a live multidisplay connection.        */
#ifndef OPENDASH_DEMO_DATA
#define OPENDASH_DEMO_DATA  1
#endif

/* ── UART pin configuration ──────────────────────────────────────────────
 * GPIO 20 = J9 DP (USB D+) on the Waveshare 2.8C board.
 * The USB-Serial/JTAG PHY is disabled in software to reclaim GPIO19/20
 * for general-purpose use.  Flashing via USB still works (ROM bootloader
 * re-enables USB-Serial/JTAG).  App console output routes through UART0
 * (GPIO43) → CH343 → UART USB-C port instead of /dev/ttyACM.
 *
 * J10 (RXD/TXD) is NOT usable — the CH343P bridge chip holds GPIO44.
 * TX is needed for HC-05 AT commands. Set to -1 if receive-only.
 * HC-05 KEY pin: -1 = not connected (pre-configured externally).        */
#ifndef OPENDASH_UART_RX_PIN
#define OPENDASH_UART_RX_PIN  20  /* GPIO20 — J9 DP (USB D+) reclaimed for UART1 */
#endif
#ifndef OPENDASH_UART_TX_PIN
#define OPENDASH_UART_TX_PIN  (-1) /* -1 = TX not connected (receive-only) */
#endif
#ifndef OPENDASH_HC05_KEY_PIN
#define OPENDASH_HC05_KEY_PIN (-1) /* -1 = KEY not connected (no AT mode toggle) */
#endif

/* ── Parsed data snapshot ─────────────────────────────────────────────
 * The UART task fills this struct; main.c reads it with the LVGL lock
 * to push values to ui_manager_update_value().                         */
typedef struct {
    float    rpm;
    float    boost;
    float    throttle;
    float    lambda;
    float    lmm;           /* mass air flow */
    float    case_temp;
    float    egt[8];        /* all 8 EGT channels (°C) */
    float    bat_volt;
    float    vdo_pres1;
    float    vdo_pres2;
    float    vdo_pres3;
    float    vdo_temp1;
    float    vdo_temp2;
    float    vdo_temp3;
    float    speed;
    uint8_t  gear;
    uint8_t  n75_duty;
    float    req_boost;
    float    efr_speed;
    float    knock;
    uint32_t frame_count;   /* increments on each valid parse */
} opendash_md_data_t;
typedef enum {
    OPENDASH_UART_DISABLED   = 0,  /* Feature toggled off */
    OPENDASH_UART_WAITING    = 1,  /* UART initialized, no data yet */
    OPENDASH_UART_RECEIVING  = 2,  /* Actively receiving valid frames */
    OPENDASH_UART_TIMEOUT    = 3,  /* Was receiving but data stopped */
} opendash_uart_status_t;

/**
 * @brief Initialize UART and start the receiver task.
 *
 * Call once during app_main(). Does nothing if
 * OPENDASH_MULTIDISPLAY_CONNECTION == 0.
 *
 * @return true on success (or if feature disabled), false on UART error
 */
bool opendash_uart_init(void);

/**
 * @brief Get current connection status.
 */
opendash_uart_status_t opendash_uart_get_status(void);

/**
 * @brief Get a human-readable status string (for LCD labels).
 *
 * Returns a pointer to a static string — do not free.
 */
const char *opendash_uart_status_str(void);

/**
 * @brief Get a snapshot of the latest parsed multidisplay data.
 *
 * Copies the latest parsed values into *out.  Thread-safe (critical section).
 * Returns true if at least one frame has been parsed (frame_count > 0).
 */
bool opendash_uart_get_data(opendash_md_data_t *out);

/**
 * @brief Returns true when status transitions to RECEIVING for the first time
 *        (or after a TIMEOUT→RECEIVING recovery).  Resets after one read.
 *        Caller can use this to trigger a buzzer beep in main.c.
 */
bool opendash_uart_connected_event(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_UART_H */