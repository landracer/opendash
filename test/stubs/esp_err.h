/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file esp_err.h (host test stub)
 * @brief Host-side stand-in for ESP-IDF's esp_err.h. Numeric values match
 *        the real ESP-IDF constants so error-code comparisons behave the
 *        same as on target.
 */
#ifndef OD_HOST_ESP_ERR_H
#define OD_HOST_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK               0
#define ESP_FAIL            -1
#define ESP_ERR_NO_MEM       0x101
#define ESP_ERR_INVALID_ARG  0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND    0x105
#define ESP_ERR_TIMEOUT      0x107

const char *esp_err_to_name(esp_err_t err);

#endif /* OD_HOST_ESP_ERR_H */
