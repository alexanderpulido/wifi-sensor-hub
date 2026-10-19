/**
 * @file wifi_station.h
 * @brief Wi-Fi Station (STA) helper with automatic reconnection.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Station credentials. */
typedef struct
{
    const char *ssid;      /**< Network name (max 31 characters). */
    const char *password;  /**< WPA2 passphrase; empty string for an open network. */
} wifi_station_params_t;

/**
 * @brief Start Wi-Fi in STA mode and connect asynchronously.
 *
 * Requires NVS to be initialised. Returns immediately; the driver keeps
 * reconnecting in the background after any disconnect.
 *
 * @param[in] params Credentials (strings are copied).
 * @return ESP_OK on success or an esp_wifi / esp_netif error code.
 */
esp_err_t wifi_station_start(const wifi_station_params_t *params);

/**
 * @brief Block until an IP address has been obtained.
 *
 * @param[in] timeout Maximum wait in ticks (portMAX_DELAY to wait forever).
 * @return true if connected, false on timeout.
 */
bool wifi_station_wait_connected(TickType_t timeout);

/** @brief Non-blocking connection state. */
bool wifi_station_is_connected(void);

#ifdef __cplusplus
}
#endif
