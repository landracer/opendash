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

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
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

/* Advertising timeout */
#define BT_OTA_ADV_TIMEOUT_MS   30000
static EventGroupHandle_t s_evt_group = NULL;
#define EVT_CONNECTED   (1 << 0)
#define EVT_OTA_DONE    (1 << 1)
#define EVT_TIMEOUT     (1 << 2)

/* ────────────────────────────────────────────────────────────────────────────
 * Status Notification Helper
 * ──────────────────────────────────────────────────────────────────────────── */

static void notify_status(void)
{
    if (s_conn_handle == 0xFFFF || s_status_val_handle == 0) return;

    uint8_t buf[3] = {
        (uint8_t)s_state,
        (s_bytes_received > 0) ? (uint8_t)(s_bytes_received * 100 / 960000) : 0,  /* rough % */
        0  /* error code */
    };

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
    }
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
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            /* OTA Status — notify */
            {
                .uuid = BLE_UUID16_DECLARE(OPENDASH_BT_OTA_STATUS_UUID),
                .access_cb = ota_status_access,
                .val_handle = &s_status_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
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
            s_last_progress_notify = 0;
            s_state = OPENDASH_OTA_STATE_RECEIVING;
            ESP_LOGI(TAG, "OTA partition ready: %s (%lu bytes)",
                     s_ota_partition->label, (unsigned long)s_ota_partition->size);
            notify_status();
            break;
        }

        case OPENDASH_OTA_CMD_END: {
            ESP_LOGI(TAG, "OTA END — verifying %lu bytes...", (unsigned long)s_bytes_received);
            s_state = OPENDASH_OTA_STATE_VERIFYING;
            notify_status();

            esp_err_t err = esp_ota_end(s_ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA validation failed: %s", esp_err_to_name(err));
                s_state = OPENDASH_OTA_STATE_ERROR;
                notify_status();
                return BLE_ATT_ERR_UNLIKELY;
            }

            err = esp_ota_set_boot_partition(s_ota_partition);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
                s_state = OPENDASH_OTA_STATE_ERROR;
                notify_status();
                return BLE_ATT_ERR_UNLIKELY;
            }

            s_state = OPENDASH_OTA_STATE_COMPLETE;
            notify_status();
            ESP_LOGW(TAG, "OTA COMPLETE — rebooting in 2 seconds...");

            /* Signal main task to reboot */
            if (s_evt_group) {
                xEventGroupSetBits(s_evt_group, EVT_OTA_DONE);
            }
            break;
        }

        case OPENDASH_OTA_CMD_ABORT: {
            ESP_LOGW(TAG, "OTA ABORT");
            if (s_ota_handle) {
                esp_ota_abort(s_ota_handle);
                s_ota_handle = 0;
            }
            s_state = OPENDASH_OTA_STATE_CONNECTED;
            s_bytes_received = 0;
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

    uint8_t buf[OPENDASH_OTA_CHUNK_SIZE];
    os_mbuf_copydata(ctxt->om, 0, len, buf);

    esp_err_t err = esp_ota_write(s_ota_handle, buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA write failed at offset %lu: %s",
                 (unsigned long)s_bytes_received, esp_err_to_name(err));
        s_state = OPENDASH_OTA_STATE_ERROR;
        notify_status();
        return BLE_ATT_ERR_UNLIKELY;
    }

    s_bytes_received += len;

    /* Notify progress every 4KB */
    if (s_bytes_received - s_last_progress_notify >= 4096) {
        s_last_progress_notify = s_bytes_received;
        ESP_LOGI(TAG, "OTA progress: %lu bytes", (unsigned long)s_bytes_received);
        notify_status();
    }

    return 0;
}

/**
 * @brief OTA Status characteristic handler — read current state
 */
static int ota_status_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t buf[3] = {
            (uint8_t)s_state,
            (s_bytes_received > 0) ? (uint8_t)(s_bytes_received * 100 / 960000) : 0,
            0
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
            } else {
                ESP_LOGW(TAG, "BLE connection failed: %d", event->connect.status);
                s_conn_handle = 0xFFFF;
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE disconnected (reason=%d)", event->disconnect.reason);
            s_conn_handle = 0xFFFF;

            /* If OTA was in progress, abort it */
            if (s_state == OPENDASH_OTA_STATE_RECEIVING && s_ota_handle) {
                ESP_LOGW(TAG, "Disconnect during OTA — aborting");
                esp_ota_abort(s_ota_handle);
                s_ota_handle = 0;
                s_bytes_received = 0;
            }
            s_state = OPENDASH_OTA_STATE_READY;

            /* Re-advertise */
            ESP_LOGI(TAG, "Re-advertising...");
            ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                              &(struct ble_gap_adv_params){
                                  .conn_mode = BLE_GAP_CONN_MODE_UND,
                                  .disc_mode = BLE_GAP_DISC_MODE_GEN,
                              },
                              gap_event_handler, NULL);
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
            break;

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
    /* Use public address */
    ble_hs_id_infer_auto(0, NULL);

    /* Set device name */
    ble_svc_gap_device_name_set(s_ble_name);

    /* Start advertising */
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x0020,  /* 20ms */
        .itvl_max = 0x0040,  /* 40ms */
    };

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

    ble_gap_adv_set_fields(&fields);
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                       &adv_params, gap_event_handler, NULL);

    s_state = OPENDASH_OTA_STATE_READY;
    ESP_LOGI(TAG, "BLE advertising as \"%s\" — waiting for connection", s_ble_name);
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
    ESP_LOGI(TAG, "║  Connect via Chrome Web Bluetooth to update  ║");
    ESP_LOGI(TAG, "║  30s timeout if no connection                ║");
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

    /* Start NimBLE host task */
    nimble_port_freertos_init(ble_host_task);

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
                                pdMS_TO_TICKS(300000));  /* 5 min max for OTA */

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
