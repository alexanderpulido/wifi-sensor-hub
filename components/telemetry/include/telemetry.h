/**
 * @file telemetry.h
 * @brief Serialises sensor samples to JSON and sends them over HTTP or MQTT.
 *
 * Example payload:
 * @code{.json}
 * {"device_id":"esp32-hub-01","timestamp_ms":123456,
 *  "temperature_c":23.41,"humidity_pct":45.20,"pressure_hpa":1013.25}
 * @endcode
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Available transports. */
typedef enum
{
    TELEMETRY_HTTP,  /**< HTTP POST of the JSON body to a URL. */
    TELEMETRY_MQTT,  /**< MQTT publish (QoS 1) to a topic. */
} telemetry_transport_t;

/** @brief Transport configuration. */
typedef struct
{
    telemetry_transport_t transport;  /**< Which transport to use. */
    const char *device_id;            /**< Identifier placed in every payload. */
    const char *http_url;             /**< Full URL, used when transport is TELEMETRY_HTTP. */
    const char *mqtt_broker_uri;      /**< e.g. "mqtt://host:1883", used for TELEMETRY_MQTT. */
    const char *mqtt_topic;           /**< Topic to publish to, used for TELEMETRY_MQTT. */
} telemetry_config_t;

/** @brief One timestamped measurement. */
typedef struct
{
    int64_t timestamp_ms;  /**< Milliseconds since boot when the sample was taken. */
    float temperature_c;   /**< Degrees Celsius. */
    float humidity_pct;    /**< %RH. */
    float pressure_hpa;    /**< hPa. */
} telemetry_sample_t;

/**
 * @brief Initialise the selected transport (starts the MQTT client if used).
 *
 * Wi-Fi should be started first. Strings in @p cfg are copied.
 *
 * @param[in] cfg Configuration.
 * @return ESP_OK or an error code.
 */
esp_err_t telemetry_init(const telemetry_config_t *cfg);

/**
 * @brief Format a sample as a JSON object.
 *
 * @param[in]  sample Sample to serialise.
 * @param[out] buf    Destination buffer.
 * @param[in]  len    Size of @p buf.
 * @return Number of characters written (excluding the terminator), or a
 *         negative value if the output did not fit.
 */
int telemetry_format_json(const telemetry_sample_t *sample, char *buf, size_t len);

/**
 * @brief Serialise and send one sample.
 *
 * @param[in] sample Sample to send.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if the transport is not
 *         ready (e.g. MQTT disconnected), ESP_FAIL on a failed or non-2xx request.
 */
esp_err_t telemetry_publish(const telemetry_sample_t *sample);

#ifdef __cplusplus
}
#endif
