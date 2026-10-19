/**
 * @file telemetry.c
 * @brief Telemetry transports. See telemetry.h for the public API.
 */
#include "telemetry.h"

#include <stdio.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "telemetry";

/** Size of the on-stack buffer holding one serialised JSON message. */
#define JSON_BUF_LEN 192

/** How long to wait for the HTTP server before giving up. */
#define HTTP_TIMEOUT_MS 5000

/* Module state, set once by telemetry_init(). */
static telemetry_transport_t s_transport;
static char s_device_id[48];
static char s_topic[96];
static esp_http_client_handle_t s_http;
static esp_mqtt_client_handle_t s_mqtt;

/** True while the MQTT client holds a live broker connection. */
static volatile bool s_mqtt_connected;

/** @brief Tracks broker connectivity so publishes can fail fast when offline. */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // Suppress unused parameter warnings
    (void)arg;
    (void)base;
    (void)data;

    if (id == MQTT_EVENT_CONNECTED)
    {
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_connected = true;
    }
    else if (id == MQTT_EVENT_DISCONNECTED)
    {
        /* The client reconnects on its own; we only record the state. */
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
    }
}

esp_err_t telemetry_init(const telemetry_config_t *cfg)
{
    if (!cfg || !cfg->device_id)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Keep our own copies so the caller's strings need not outlive this call. */
    s_transport = cfg->transport;
    strlcpy(s_device_id, cfg->device_id, sizeof(s_device_id));

    if (cfg->transport == TELEMETRY_HTTP)
    {
        if (!cfg->http_url)
        {
            return ESP_ERR_INVALID_ARG;
        }

        /* One reusable client; the connection is opened on the first request. */
        const esp_http_client_config_t http_cfg =
        {
            .url = cfg->http_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = HTTP_TIMEOUT_MS,
        };
        s_http = esp_http_client_init(&http_cfg);
        if (!s_http)
        {
            return ESP_FAIL;
        }
        return esp_http_client_set_header(s_http, "Content-Type", "application/json");
    }

    /* MQTT path. */
    if (!cfg->mqtt_broker_uri || !cfg->mqtt_topic)
    {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_topic, cfg->mqtt_topic, sizeof(s_topic));

    const esp_mqtt_client_config_t mqtt_cfg =
    {
        .broker.address.uri = cfg->mqtt_broker_uri,
    };
    s_mqtt = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt)
    {
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    /* Runs in its own task and keeps retrying until the broker is reachable. */
    return esp_mqtt_client_start(s_mqtt);
}

int telemetry_format_json(const telemetry_sample_t *s, char *buf, size_t len)
{
    /* Values are printed with 2 decimals; that exceeds the sensor's accuracy. */
    int n = snprintf(buf, len,
                     "{\"device_id\":\"%s\",\"timestamp_ms\":%lld,"
                     "\"temperature_c\":%.2f,\"humidity_pct\":%.2f,\"pressure_hpa\":%.2f}",
                     s_device_id, (long long)s->timestamp_ms,
                     s->temperature_c, s->humidity_pct, s->pressure_hpa);

    /* snprintf returns the length it *wanted*; >= len means it was truncated. */
    return (n < 0 || (size_t)n >= len) ? -1 : n;
}

/** @brief POST the JSON body and require a 2xx response. */
static esp_err_t publish_http(const char *json, int len)
{
    esp_err_t err = esp_http_client_set_post_field(s_http, json, len);
    if (err != ESP_OK)
    {
        return err;
    }

    /* Blocking call: connects if needed, sends, and waits for the response. */
    err = esp_http_client_perform(s_http);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* The request went through, but the server may still have rejected it. */
    const int status = esp_http_client_get_status_code(s_http);
    if (status < 200 || status >= 300)
    {
        ESP_LOGW(TAG, "HTTP status %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** @brief Publish the JSON body to the configured topic with QoS 1. */
static esp_err_t publish_mqtt(const char *json, int len)
{
    /* Fail fast rather than queueing messages for a broker that is offline. */
    if (!s_mqtt_connected)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* Args: client, topic, data, len, qos=1 (at least once), retain=0. */
    return esp_mqtt_client_publish(s_mqtt, s_topic, json, len, 1, 0) >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t telemetry_publish(const telemetry_sample_t *sample)
{
    if (!sample)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Serialise once, then hand the same text to whichever transport is active. */
    char json[JSON_BUF_LEN];
    const int len = telemetry_format_json(sample, json, sizeof(json));
    if (len < 0)
    {
        return ESP_ERR_NO_MEM;
    }

    return s_transport == TELEMETRY_HTTP ? publish_http(json, len) : publish_mqtt(json, len);
}
