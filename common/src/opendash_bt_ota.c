/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_bt_ota.c
 * @brief OpenDash BLE OTA Service — NimBLE Implementation
 *
 * Implements a BLE GATT server that receives firmware binary data
 * and writes it to the ESP32 OTA partition using esp_ota_ops.h.
 *
 * Architecture:
 *   - NimBLE GATT server with 3 characteristics (control, data, status)
 *   - Data writes are buffered and written to OTA partition in sequence
 *   - Progress notifications sent to client every 4KB
 *   - 30-second advertising timeout if no client connects
 *   - Automatic reboot on successful OTA completion
 *
 * @note On ESP32 (not S3), WiFi and BLE share the radio.
 *       WiFi/ESP-NOW MUST be shut down before starting BLE.
 */

#include "opendash_bt_ota.h"
#include "opendash_identity.h"
#include "opendash_espnow.h"
#include "opendash_i2c_protocol.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_bt.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "bt_ota";

/* ────────────────────────────────────────────────────────────────────────────
 * Module State
 * ──────────────────────────────────────────────────────────────────────────── */

static bool s_active = false;
static opendash_ota_state_t s_state = OPENDASH_OTA_STATE_IDLE;
static uint16_t s_conn_handle = 0xFFFF;
static uint16_t s_status_val_handle = 0;

/* OTA session */
static const esp_partition_t *s_ota_partition = NULL;
static esp_ota_handle_t s_ota_handle = 0;
static uint32_t s_bytes_received = 0;
static uint32_t s_last_progress_notify = 0;

/* BLE device name (set at start) */
static char s_ble_name[32] = "OpenDash-OTA";

/* Own-addr-type chosen by on_ble_sync(). Cached so the disconnect handler
 * can restart advertising with the SAME type — on ESP32-S3 this is
 * BLE_OWN_ADDR_RANDOM, and using PUBLIC here causes adv_start to silently
 * fail (rc != 0), leaving the device invisible to the OTA client. */
static uint8_t s_own_addr_type = 0;

/* Advertising timeout — generous so a human has time to scan, pair, and
 * push firmware. Was 30 s, which the user could not reliably hit. */
#define BT_OTA_ADV_TIMEOUT_MS   300000   /* 5 minutes */
static EventGroupHandle_t s_evt_group = NULL;
#define EVT_CONNECTED   (1 << 0)
#define EVT_OTA_DONE    (1 << 1)
#define EVT_TIMEOUT     (1 << 2)

/* ────────────────────────────────────────────────────────────────────────────
 * Flash-write worker — keeps esp_ota_write off the NimBLE host task so the
 * controller's RX queue doesn't overflow during 4 KB erase boundaries.
 * ──────────────────────────────────────────────────────────────────────── */
#define OTA_QUEUE_DEPTH         64
#define OTA_CHUNK_OP_DATA       0
#define OTA_CHUNK_OP_FINALIZE   1   /* esp_ota_end + set_boot + signal DONE */

typedef struct {
    uint16_t op;
    uint16_t len;
    uint8_t  data[OPENDASH_OTA_CHUNK_SIZE];
} ota_chunk_t;

static QueueHandle_t s_ota_queue = NULL;
static TaskHandle_t  s_ota_worker = NULL;
/* Bytes pushed onto the worker queue (vs. s_bytes_received which is the
 * actually-flashed count maintained by the worker). The OFFSET char returns
 * s_bytes_received so client flow control is paced against committed data. */
static uint32_t s_bytes_queued = 0;
/* Progress notify cadence — every 64 KB (was 4 KB, which competed with the
 * inbound write stream on the same conn interval). */
#define OTA_PROGRESS_NOTIFY_INTERVAL  (64 * 1024)

/* Adv watchdog — re-arms advertising if NimBLE's internal reattempt fails
 * (HCI 0x202 BLE_ERR_UNK_CONN_ID after a BlueZ supervision-timeout drop). */
static esp_timer_handle_t s_adv_watchdog = NULL;
static void adv_watchdog_cb(void *arg);
static int gap_event_handler(struct ble_gap_event *event, void *arg);

static void adv_kick(void)
{
    if (!s_active || s_conn_handle != 0xFFFF) {
        return;
    }
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = 0x0020,
        .itvl_max  = 0x0040,
    };
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &params, gap_event_handler, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "adv_kick: advertising restarted");
    } else if (rc == BLE_HS_EALREADY) {
        /* already advertising */
    } else {
        ESP_LOGW(TAG, "adv_kick: ble_gap_adv_start rc=%d (will retry)", rc);
    }
}

static void adv_watchdog_cb(void *arg)
{
    (void)arg;
    adv_kick();
}

/* ────────────────────────────────────────────────────────────────────────────
 * Status Notification Helper
 * ──────────────────────────────────────────────────────────────────────────── */

static void notify_status(void)
{
    if (s_conn_handle == 0xFFFF || s_status_val_handle == 0) return;

    uint8_t progress = 0;
    if (s_bytes_received > 0 && s_ota_partition && s_ota_partition->size > 0) {
        uint32_t pct = (uint32_t)((s_bytes_received * 100ULL) / s_ota_partition->size);
        if (pct > 100) pct = 100;
        progress = (uint8_t)pct;
    }

    uint8_t buf[3] = {
        (uint8_t)s_state,
        progress,
        0  /* error code */
    };

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * OTA flash-write worker task. Runs at modest priority on the APP CPU so it
 * cannot starve the NimBLE host task on the PRO CPU. Reads chunks from the
 * queue and calls esp_ota_write (which can block on 4 KB erase boundaries
 * for tens of milliseconds — exactly what we needed to get off the host
 * task to stop the controller from dropping the link).
 * ──────────────────────────────────────────────────────────────────────── */
static void ota_worker_task(void *arg)
{
    static ota_chunk_t chunk;
    ESP_LOGI(TAG, "OTA worker task started (core %d)", xPortGetCoreID());
    for (;;) {
        if (xQueueReceive(s_ota_queue, &chunk, portMAX_DELAY) != pdTRUE) continue;

        if (chunk.op == OTA_CHUNK_OP_FINALIZE) {
            ESP_LOGI(TAG, "OTA FINALIZE — flashed %lu bytes, validating...",
                     (unsigned long)s_bytes_received);
            s_state = OPENDASH_OTA_STATE_VERIFYING;
            notify_status();

            esp_err_t err = esp_ota_end(s_ota_handle);
            s_ota_handle = 0;
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
                s_state = OPENDASH_OTA_STATE_ERROR;
                notify_status();
                if (s_evt_group) xEventGroupSetBits(s_evt_group, EVT_OTA_DONE);
                continue;
            }
            err = esp_ota_set_boot_partition(s_ota_partition);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
                s_state = OPENDASH_OTA_STATE_ERROR;
                notify_status();
                if (s_evt_group) xEventGroupSetBits(s_evt_group, EVT_OTA_DONE);
                continue;
            }
            s_state = OPENDASH_OTA_STATE_COMPLETE;
            notify_status();
            ESP_LOGW(TAG, "OTA COMPLETE — signaling main task for reboot");
            if (s_evt_group) xEventGroupSetBits(s_evt_group, EVT_OTA_DONE);
            continue;
        }

        /* OTA_CHUNK_OP_DATA */
        if (!s_ota_handle) continue;   /* aborted before drain */
        esp_err_t err = esp_ota_write(s_ota_handle, chunk.data, chunk.len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %lu: %s",
                     (unsigned long)s_bytes_received, esp_err_to_name(err));
            s_state = OPENDASH_OTA_STATE_ERROR;
            notify_status();
            continue;
        }
        s_bytes_received += chunk.len;
        if (s_bytes_received - s_last_progress_notify >= OTA_PROGRESS_NOTIFY_INTERVAL) {
            s_last_progress_notify = s_bytes_received;
            ESP_LOGI(TAG, "OTA progress: %lu bytes", (unsigned long)s_bytes_received);
            notify_status();
        }
    }
}

static void ota_worker_ensure_started(void)
{
    if (!s_ota_queue) {
        s_ota_queue = xQueueCreate(OTA_QUEUE_DEPTH, sizeof(ota_chunk_t));
    }
    if (!s_ota_worker && s_ota_queue) {
        /* Pin to core 1 (APP_CPU). NimBLE host task lives on core 0. */
        xTaskCreatePinnedToCore(ota_worker_task, "ota_flash", 4096, NULL,
                                5, &s_ota_worker, 1);
    }
}

static void ota_worker_drain_queue(void)
{
    if (!s_ota_queue) return;
    ota_chunk_t junk;
    while (xQueueReceive(s_ota_queue, &junk, 0) == pdTRUE) { /* drop */ }
}

/* ────────────────────────────────────────────────────────────────────────────
 * GATT Access Callbacks
 * ──────────────────────────────────────────────────────────────────────────── */

static int ota_ctrl_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ota_data_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ota_status_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ota_offset_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);

/* GATT service definition */
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(OPENDASH_BT_OTA_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            /* OTA Control — write */
            {
                .uuid = BLE_UUID16_DECLARE(OPENDASH_BT_OTA_CTRL_UUID),
                .access_cb = ota_ctrl_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            /* OTA Data — write-no-response for throughput */
            {
                .uuid = BLE_UUID16_DECLARE(OPENDASH_BT_OTA_DATA_UUID),
                .access_cb = ota_data_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            /* OTA Status — notify */
            {
                .uuid = BLE_UUID16_DECLARE(OPENDASH_BT_OTA_STATUS_UUID),
                .access_cb = ota_status_access,
                .val_handle = &s_status_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            /* OTA Offset — read-only, returns s_bytes_received as LE u32.
             * Client reads this after reconnect to resume in-flight transfer. */
            {
                .uuid = BLE_UUID16_DECLARE(OPENDASH_BT_OTA_OFFSET_UUID),
                .access_cb = ota_offset_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },  /* terminate */
        },
    },
    { 0 },  /* terminate */
};

/**
 * @brief OTA Control characteristic handler
 */
static int ota_ctrl_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t cmd;
    os_mbuf_copydata(ctxt->om, 0, 1, &cmd);

    switch (cmd) {
        case OPENDASH_OTA_CMD_BEGIN: {
            ESP_LOGI(TAG, "OTA BEGIN — erasing partition...");
            /* Drop any stale chunks from a previous half-finished session. */
            ota_worker_drain_queue();
            /* If a prior session was still alive (kept across disconnect for
             * resume), abort it cleanly before re-beginning at offset 0. */
            if (s_ota_handle) {
                esp_ota_abort(s_ota_handle);
                s_ota_handle = 0;
            }
            s_ota_partition = esp_ota_get_next_update_partition(NULL);
            if (!s_ota_partition) {
                ESP_LOGE(TAG, "No OTA partition available!");
                s_state = OPENDASH_OTA_STATE_ERROR;
                notify_status();
                return BLE_ATT_ERR_UNLIKELY;
            }

            esp_err_t err = esp_ota_begin(s_ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
                s_state = OPENDASH_OTA_STATE_ERROR;
                notify_status();
                return BLE_ATT_ERR_UNLIKELY;
            }

            s_bytes_received = 0;
            s_bytes_queued = 0;
            s_last_progress_notify = 0;
            ota_worker_ensure_started();
            s_state = OPENDASH_OTA_STATE_RECEIVING;
            ESP_LOGI(TAG, "OTA partition ready: %s (%lu bytes)",
                     s_ota_partition->label, (unsigned long)s_ota_partition->size);
            notify_status();
            break;
        }

        case OPENDASH_OTA_CMD_RESUME: {
            /* Payload: [cmd:1][offset_LE:4]. Resume an in-flight session
             * after a disconnect/reconnect without re-erasing the partition. */
            if (len < 5) {
                ESP_LOGW(TAG, "RESUME too short (%u)", (unsigned)len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            uint8_t obuf[4];
            os_mbuf_copydata(ctxt->om, 1, 4, obuf);
            uint32_t want = (uint32_t)obuf[0] | ((uint32_t)obuf[1] << 8) |
                            ((uint32_t)obuf[2] << 16) | ((uint32_t)obuf[3] << 24);
            if (!s_ota_handle || want != s_bytes_received) {
                ESP_LOGW(TAG, "RESUME mismatch: client=%lu server=%lu handle=%d",
                         (unsigned long)want, (unsigned long)s_bytes_received,
                         (int)(s_ota_handle != 0));
                s_state = OPENDASH_OTA_STATE_ERROR;
                notify_status();
                return BLE_ATT_ERR_UNLIKELY;
            }
            /* Any stale chunks from before the disconnect must go — flow
             * control caps client at +16 KB but the worker's queue can hold
             * up to OTA_QUEUE_DEPTH chunks the worker hadn't flushed yet. */
            ota_worker_drain_queue();
            s_bytes_queued = s_bytes_received;
            ota_worker_ensure_started();
            ESP_LOGI(TAG, "OTA RESUME at offset %lu", (unsigned long)s_bytes_received);
            s_state = OPENDASH_OTA_STATE_RECEIVING;
            notify_status();
            break;
        }

        case OPENDASH_OTA_CMD_END: {
            ESP_LOGI(TAG, "OTA END — queued=%lu flashed=%lu, finalizing on worker",
                     (unsigned long)s_bytes_queued,
                     (unsigned long)s_bytes_received);
            /* Post a FINALIZE sentinel; worker drains remaining DATA chunks,
             * calls esp_ota_end + set_boot_partition, then sets EVT_OTA_DONE
             * so opendash_bt_ota_start can reboot. Must not call esp_ota_end
             * here on the host task — it can block on flash. */
            ota_chunk_t fin = { .op = OTA_CHUNK_OP_FINALIZE, .len = 0 };
            if (xQueueSend(s_ota_queue, &fin, pdMS_TO_TICKS(1000)) != pdTRUE) {
                ESP_LOGE(TAG, "OTA queue full posting FINALIZE");
                s_state = OPENDASH_OTA_STATE_ERROR;
                notify_status();
                return BLE_ATT_ERR_UNLIKELY;
            }
            break;
        }

        case OPENDASH_OTA_CMD_ABORT: {
            ESP_LOGW(TAG, "OTA ABORT");
            ota_worker_drain_queue();
            if (s_ota_handle) {
                esp_ota_abort(s_ota_handle);
                s_ota_handle = 0;
            }
            s_state = OPENDASH_OTA_STATE_CONNECTED;
            s_bytes_received = 0;
            s_bytes_queued = 0;
            notify_status();
            break;
        }

        case OPENDASH_OTA_CMD_VERSION: {
            ESP_LOGI(TAG, "Version request: %s", OPENDASH_VERSION_STR);
            /* Version is included in status notify */
            notify_status();
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown OTA command: 0x%02X", cmd);
            break;
    }

    return 0;
}

/**
 * @brief OTA Data characteristic handler — receives firmware chunks
 */
static int ota_data_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    if (s_state != OPENDASH_OTA_STATE_RECEIVING) {
        ESP_LOGW(TAG, "Data write rejected — not in RECEIVING state");
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > OPENDASH_OTA_CHUNK_SIZE) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    /* Enqueue chunk for the OTA worker. Copying out of the mbuf is required
     * because the NimBLE stack reclaims it as soon as we return. We keep this
     * handler's wall time to a few µs so the controller's RX queue can drain
     * even when the next flash write is mid-erase. */
    ota_chunk_t chunk;
    chunk.op  = OTA_CHUNK_OP_DATA;
    chunk.len = len;
    os_mbuf_copydata(ctxt->om, 0, len, chunk.data);

    /* Non-blocking: if the worker is mid-erase and the queue is full, we
     * MUST NOT block the NimBLE host task — blocking here stalls controller
     * RX drain and the central drops the link on supervision timeout. The
     * client retries via OFFSET / RESUME, so dropping a chunk is cheap. */
    if (xQueueSend(s_ota_queue, &chunk, 0) != pdTRUE) {
        ESP_LOGW(TAG, "OTA queue full (queued=%lu received=%lu) — backpressure",
                 (unsigned long)s_bytes_queued, (unsigned long)s_bytes_received);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    s_bytes_queued += len;
    return 0;
}

/**
 * @brief OTA Status characteristic handler — read current state
 */
static int ota_status_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t progress = 0;
        if (s_bytes_received > 0 && s_ota_partition && s_ota_partition->size > 0) {
            uint32_t pct = (uint32_t)((s_bytes_received * 100ULL) / s_ota_partition->size);
            if (pct > 100) pct = 100;
            progress = (uint8_t)pct;
        }
        uint8_t buf[3] = {
            (uint8_t)s_state,
            progress,
            0
        };
        os_mbuf_append(ctxt->om, buf, sizeof(buf));
    }
    return 0;
}

/**
 * @brief OTA Offset characteristic handler — returns bytes_received LE u32.
 */
static int ota_offset_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint32_t v = s_bytes_received;
        uint8_t buf[4] = {
            (uint8_t)(v & 0xFF),
            (uint8_t)((v >> 8) & 0xFF),
            (uint8_t)((v >> 16) & 0xFF),
            (uint8_t)((v >> 24) & 0xFF),
        };
        os_mbuf_append(ctxt->om, buf, sizeof(buf));
    }
    return 0;
}

/* ────────────────────────────────────────────────────────────────────────────
 * GAP Event Handler
 * ──────────────────────────────────────────────────────────────────────────── */

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                s_state = OPENDASH_OTA_STATE_CONNECTED;
                ESP_LOGI(TAG, "BLE connected (handle=%d)", s_conn_handle);
                if (s_evt_group) {
                    xEventGroupSetBits(s_evt_group, EVT_CONNECTED);
                }

                /* Set preferred MTU — client will initiate exchange */
                ble_att_set_preferred_mtu(517);

                /* PPCP already advertises sup_timeout=3200 which BlueZ honors,
                 * but it picks its own default 60ms interval. Request a
                 * 15-30ms interval now that the link is stable. Keep
                 * sup_timeout=3200 so we're not asking BlueZ to shorten it. */
                struct ble_gap_upd_params upd = {
                    .itvl_min          = 12,    /* 15.0 ms */
                    .itvl_max          = 24,    /* 30.0 ms */
                    .latency           = 0,
                    .supervision_timeout = 3200,/* 32 s */
                    .min_ce_len        = 0,
                    .max_ce_len        = 0,
                };
                int rc_upd = ble_gap_update_params(s_conn_handle, &upd);
                if (rc_upd != 0) {
                    ESP_LOGW(TAG, "ble_gap_update_params rc=%d", rc_upd);
                }

                /* Request LE 2M PHY for both directions — doubles raw rate
                 * vs. default 1M PHY. BlueZ supports 2M PHY. */
                int rc_phy = ble_gap_set_prefered_le_phy(s_conn_handle,
                                                          BLE_GAP_LE_PHY_2M_MASK,
                                                          BLE_GAP_LE_PHY_2M_MASK,
                                                          0);
                if (rc_phy != 0) {
                    ESP_LOGW(TAG, "ble_gap_set_prefered_le_phy rc=%d", rc_phy);
                }
            } else {
                ESP_LOGW(TAG, "BLE connection failed: %d", event->connect.status);
                s_conn_handle = 0xFFFF;
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE disconnected (reason=%d)", event->disconnect.reason);
            s_conn_handle = 0xFFFF;

            /* Keep s_ota_handle and s_bytes_received intact so the next
             * connection can RESUME at the same offset. Only flip state
             * so writes are rejected until the client re-arms via RESUME. */
            if (s_state == OPENDASH_OTA_STATE_RECEIVING) {
                ESP_LOGW(TAG, "Disconnect mid-OTA at %lu bytes — awaiting RESUME",
                         (unsigned long)s_bytes_received);
            }
            s_state = OPENDASH_OTA_STATE_READY;

            /* Re-advertise using the SAME addr type the host stack chose at
             * sync time. ESP32-S3 has no factory public BLE address so this
             * is BLE_OWN_ADDR_RANDOM; passing PUBLIC silently fails. */
            ESP_LOGI(TAG, "Re-advertising (addr_type=%d)...", s_own_addr_type);
            int rc_adv = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                              &(struct ble_gap_adv_params){
                                  .conn_mode = BLE_GAP_CONN_MODE_UND,
                                  .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                  .itvl_min = 0x0020,
                                  .itvl_max = 0x0040,
                              },
                              gap_event_handler, NULL);
            if (rc_adv != 0) {
                ESP_LOGE(TAG, "Re-advertise failed: %d", rc_adv);
            }
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
            break;

        case BLE_GAP_EVENT_CONN_UPDATE: {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
                ESP_LOGI(TAG,
                    "CONN_UPDATE status=%d itvl=%u latency=%u sup_timeout=%u (units 10ms->%ums)",
                    event->conn_update.status,
                    desc.conn_itvl, desc.conn_latency, desc.supervision_timeout,
                    desc.supervision_timeout * 10);
            } else {
                ESP_LOGI(TAG, "CONN_UPDATE status=%d (conn_find failed)",
                         event->conn_update.status);
            }
            break;
        }

        default:
            break;
    }
    return 0;
}

/* ────────────────────────────────────────────────────────────────────────────
 * BLE Host Task
 * ──────────────────────────────────────────────────────────────────────────── */

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_ble_sync(void)
{
    int rc;

    /* Seed an address (public if factory-programmed, else random-static
     * derived from factory MAC). ESP32-S3 has no factory PUBLIC BLE addr,
     * so this is REQUIRED before any GAP activity or NimBLE will crash
     * inside ble_hs_id_addr_type_usable (StoreProhibited). */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }

    /* Infer which own-addr-type NimBLE should use for adv. NULL out-ptr
     * (as previously coded) causes a StoreProhibited inside NimBLE. */
    uint8_t own_addr_type = 0;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }
    s_own_addr_type = own_addr_type;

    /* Log the actual BLE MAC we will advertise with. */
    uint8_t addr_val[6] = {0};
    if (ble_hs_id_copy_addr(own_addr_type, addr_val, NULL) == 0) {
        ESP_LOGI(TAG, "BLE addr type=%d %02X:%02X:%02X:%02X:%02X:%02X",
                 own_addr_type,
                 addr_val[5], addr_val[4], addr_val[3],
                 addr_val[2], addr_val[1], addr_val[0]);
    }

    /* Set device name */
    ble_svc_gap_device_name_set(s_ble_name);

    /* Build advertising payload */
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_ble_name;
    fields.name_len = strlen(s_ble_name);
    fields.name_is_complete = 1;

    /* Advertise the OTA service UUID */
    ble_uuid16_t svc_uuid = BLE_UUID16_INIT(OPENDASH_BT_OTA_SVC_UUID);
    fields.uuids16 = &svc_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    /* Start advertising */
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x0020,  /* 20ms */
        .itvl_max = 0x0040,  /* 40ms */
    };

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return;
    }

    s_state = OPENDASH_OTA_STATE_READY;
    ESP_LOGI(TAG, "BLE advertising as \"%s\" - waiting for connection", s_ble_name);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t opendash_bt_ota_start(opendash_node_t node)
{
    if (s_active) return ESP_ERR_INVALID_STATE;

    /* Build BLE device name */
    snprintf(s_ble_name, sizeof(s_ble_name), "OpenDash-%s-OTA",
             opendash_node_name(node));

    ESP_LOGI(TAG, "╔══════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  BLE OTA Mode — %s", s_ble_name);
    ESP_LOGI(TAG, "║  Push firmware via opendash/ble_ota.py       ║");
    ESP_LOGI(TAG, "║  Advertising window: 5 min if no connection  ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════╝");

    s_active = true;
    s_state = OPENDASH_OTA_STATE_IDLE;
    s_conn_handle = 0xFFFF;
    s_ota_handle = 0;
    s_bytes_received = 0;

    /* Create event group for synchronization */
    s_evt_group = xEventGroupCreate();
    if (!s_evt_group) {
        s_active = false;
        return ESP_ERR_NO_MEM;
    }

    /* Initialize NimBLE */
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(ret));
        s_active = false;
        vEventGroupDelete(s_evt_group);
        return ret;
    }

    /* Register GATT services */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ret = ble_gatts_count_cfg(s_gatt_svcs);
    if (ret != 0) {
        ESP_LOGE(TAG, "GATT count failed: %d", ret);
        s_active = false;
        return ESP_FAIL;
    }

    ret = ble_gatts_add_svcs(s_gatt_svcs);
    if (ret != 0) {
        ESP_LOGE(TAG, "GATT add services failed: %d", ret);
        s_active = false;
        return ESP_FAIL;
    }

    /* Configure host */
    ble_hs_cfg.sync_cb = on_ble_sync;

    /* Boost BLE TX power to max (+9 dBm) to survive 2.4 GHz interference
     * from co-located ESP-NOW slaves still beaconing on ch 1. */
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P9);

    /* Start NimBLE host task */
    nimble_port_freertos_init(ble_host_task);

    /* Periodic adv watchdog — re-arms advertising every 1s if NimBLE's
     * internal reattempt died (rc=3 after HCI 0x202). */
    if (s_adv_watchdog == NULL) {
        const esp_timer_create_args_t a = {
            .callback = adv_watchdog_cb,
            .name = "bt_ota_adv_wd",
        };
        esp_timer_create(&a, &s_adv_watchdog);
    }
    if (s_adv_watchdog) {
        esp_timer_start_periodic(s_adv_watchdog, 1000 * 1000);
    }

    /* Wait for connection or timeout */
    EventBits_t bits = xEventGroupWaitBits(s_evt_group,
                                            EVT_CONNECTED | EVT_TIMEOUT,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(BT_OTA_ADV_TIMEOUT_MS));

    if (!(bits & EVT_CONNECTED)) {
        ESP_LOGW(TAG, "No BLE connection within %dms — exiting OTA mode",
                 BT_OTA_ADV_TIMEOUT_MS);
        opendash_bt_ota_stop();
        return ESP_ERR_TIMEOUT;
    }

    /* Connected — wait for OTA completion or disconnect */
    ESP_LOGI(TAG, "Client connected — waiting for OTA data...");
    bits = xEventGroupWaitBits(s_evt_group,
                                EVT_OTA_DONE,
                                pdTRUE, pdFALSE,
                                pdMS_TO_TICKS(1800000));  /* 30 min max for OTA — slow links need headroom */

    if (bits & EVT_OTA_DONE) {
        ESP_LOGW(TAG, "OTA complete! Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        /* never reaches here */
    }

    ESP_LOGW(TAG, "OTA timed out or was aborted");
    opendash_bt_ota_stop();
    return ESP_ERR_TIMEOUT;
}

bool opendash_bt_ota_is_active(void)
{
    return s_active;
}

esp_err_t opendash_bt_ota_stop(void)
{
    if (!s_active) return ESP_OK;

    ESP_LOGI(TAG, "Shutting down BLE OTA service");

    if (s_adv_watchdog) {
        esp_timer_stop(s_adv_watchdog);
    }

    if (s_ota_handle && s_state == OPENDASH_OTA_STATE_RECEIVING) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
    }

    /* Stop advertising and disconnect */
    ble_gap_adv_stop();
    if (s_conn_handle != 0xFFFF) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    /* Deinit NimBLE */
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    }

    if (s_evt_group) {
        vEventGroupDelete(s_evt_group);
        s_evt_group = NULL;
    }

    s_active = false;
    s_state = OPENDASH_OTA_STATE_IDLE;
    s_conn_handle = 0xFFFF;

    ESP_LOGI(TAG, "BLE OTA service stopped");
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_bt_ota_enter — shared "go into BLE OTA mode" entry point.
 *
 * Every slave's ENTER_BT_OTA handler funnels through here so the protocol
 * sequence is identical across all 11 nodes:
 *
 *   1. Best-effort STATUS_REPORT to center with OPENDASH_STATUS_FLAG_BLE_OTA
 *      so center's Device Management screen shows "OTA-MODE" instead of
 *      "ONLINE" (or "OFFLINE" once we tear down ESP-NOW seconds later).
 *   2. Brief delay so the packet drains before we kill the radio.
 *   3. opendash_espnow_deinit() — releases WiFi so BLE can use the radio.
 *   4. opendash_bt_ota_start() — blocks while advertising / serving GATT.
 *   5. Whether OTA succeeded, timed out, or failed, esp_restart() returns
 *      the node to a clean ESP-NOW state.
 * ──────────────────────────────────────────────────────────────────────────── */
esp_err_t opendash_bt_ota_enter(opendash_node_t self, const uint8_t center_mac[6])
{
    /* 1. Notify center we are going into OTA so the UI can display a badge.
     *    Best-effort: if center MAC is unknown or send fails, continue anyway.
     *    Payload format matches the existing STATUS_REPORT contract:
     *      [node_id:1][flags_lo:1][flags_hi:1] */
    if (center_mac) {
        uint8_t status_payload[3] = {
            (uint8_t)self,
            (uint8_t)(OPENDASH_STATUS_FLAG_RUNNING | OPENDASH_STATUS_FLAG_BLE_OTA),
            0x00,
        };
        opendash_i2c_msg_t resp;
        if (opendash_i2c_build_msg(&resp, OPENDASH_CMD_STATUS_REPORT,
                                    status_payload, sizeof(status_payload)) == OPENDASH_OK) {
            uint8_t buf[OPENDASH_ESPNOW_MAX_DATA];
            uint16_t len = 0;
            if (opendash_i2c_serialize(&resp, buf, &len) == OPENDASH_OK) {
                opendash_espnow_send(center_mac, buf, len);
                /* Give the radio ~50ms to actually drain before we kill WiFi. */
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }

    /* 2. Tear down ESP-NOW so BLE can own the radio. */
    opendash_espnow_deinit();

    /* 2b. Disable task watchdog. Once ESP-NOW is gone, per-node bcast/recv
     *     tasks block forever in opendash_espnow_recv() and never feed WDT.
     *     We're committing to a reboot at the end of this call, so killing
     *     the WDT is safe and removes log spam during the OTA window. */
    esp_task_wdt_deinit();

    /* 3. Start BLE OTA service (blocks until completion or 30s timeout). */
    esp_err_t ret = opendash_bt_ota_start(self);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "opendash_bt_ota_start failed: %s — rebooting to recover",
                 esp_err_to_name(ret));
    }

    /* 4. Always restart so we come back up in normal ESP-NOW mode whether
     *    the OTA succeeded, timed out, or failed to start. */
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();

    /* esp_restart() does not return, but keep the compiler happy. */
    return ret;
}
