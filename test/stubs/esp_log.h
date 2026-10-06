/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file esp_log.h (host test stub)
 * @brief ESP_LOGx → silent-by-default variadic sink (printed when
 *        OD_VERBOSE_LOG=1). The sink still evaluates tag + every vararg so
 *        the code under test compiles under the same -Wall -Wextra -Werror
 *        discipline as on target — no host/target unused-variable drift.
 */
#ifndef OD_HOST_ESP_LOG_H
#define OD_HOST_ESP_LOG_H

#include <stdio.h>

void esp_stub_logf(const char *lvl, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, fmt, ...) esp_stub_logf("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_stub_logf("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_stub_logf("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) esp_stub_logf("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) esp_stub_logf("V", tag, fmt, ##__VA_ARGS__)
#define ESP_ERROR_CHECK(x)    do { (void)(x); } while (0)

#endif /* OD_HOST_ESP_LOG_H */
