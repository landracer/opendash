/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file nvs.h (host test stub)
 * @brief In-memory NVS fake. Blob get/set by (namespace,key); values persist
 *        inside the test process so "reboot" (de-init/init) can reload them.
 */
#ifndef OD_HOST_NVS_H
#define OD_HOST_NVS_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

#define ESP_ERR_NVS_NOT_FOUND 0x115

esp_err_t nvs_open(const char *namespace_, nvs_open_mode_t mode, nvs_handle_t *out);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *buf, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *buf, size_t len);
esp_err_t nvs_commit(nvs_handle_t h);
void      nvs_close(nvs_handle_t h);

/* Test control: wipe all fake storage between suites */
void od_stub_nvs_reset(void);

#endif /* OD_HOST_NVS_H */
