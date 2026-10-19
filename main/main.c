/**
 * @file main.c
 * @brief Application entry point: two FreeRTOS tasks joined by a queue.
 *
 * @mainpage WiFi Sensor Hub
 *
 * An ESP32 samples a BME280 over I2C, converts the raw registers to
 * degC / %RH / hPa, and publishes JSON over Wi-Fi via HTTP POST or MQTT.
 *
 * @section wiring Wiring (defaults, changeable in menuconfig)
 * | BME280 | ESP32   |
 * |--------|---------|
 * | VIN    | 3V3     |
 * | GND    | GND     |
 * | SDA    | GPIO 21 |
 * | SCL    | GPIO 22 |
 *
 * @section arch Architecture
 * - **sensor_task** reads the BME280 every CONFIG_HUB_SAMPLE_PERIOD_MS and
 *   pushes a telemetry_sample_t into a queue. It never touches the network.
 * - **publish_task** pops samples, waits for Wi-Fi if needed, and calls
 *   telemetry_publish(). A Wi-Fi outage only delays this task; the sensor
 *   task keeps sampling and the queue buffers readings (oldest dropped when full).
 *
 * @section build Build
 * `idf.py menuconfig` (WiFi Sensor Hub menu), then `idf.py build flash monitor`.
 */
#include <stdio.h>
#include "bme280.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "telemetry.h"
#include "wifi_station.h"

/** Tag shown in front of every log line from this file. */
static const char *TAG = "hub";

/* Task stack sizes in bytes. The publish task is larger because the HTTP/MQTT
 * clients and JSON formatting need more stack than a simple I2C read. */
#define SENSOR_TASK_STACK 4096
#define PUBLISH_TASK_STACK 6144

/* Both tasks share a priority so FreeRTOS time-slices between them. */
#define PUBLISH_TASK_PRIO 5
#define SENSOR_TASK_PRIO 5

/** Queue carrying telemetry_sample_t items from sensor_task to publish_task. */
static QueueHandle_t s_queue;

/** @brief Enqueue a sample, discarding the oldest one if the queue is full. */
static void queue_push_overwrite(const telemetry_sample_t *sample)
{
    /* Timeout of 0: never block the sensor task, even if the queue is full. */
    if (xQueueSend(s_queue, sample, 0) == pdTRUE)
    {
        return;
    }

    /* Queue is full (Wi-Fi is probably down): make room by removing the oldest
     * sample, then insert the new one so the freshest data is always kept. */
    telemetry_sample_t dropped; 
    xQueueReceive(s_queue, &dropped, 0);
    ESP_LOGW(TAG, "Queue full, dropped oldest sample");
    xQueueSend(s_queue, sample, 0);
}

/**
 * @brief Task A: periodically read the BME280 and queue the result.
 * @param[in] arg bme280_handle_t
 */
static void sensor_task(void *arg)
{
    bme280_handle_t dev = arg;

    /* Reference point for vTaskDelayUntil(), which keeps a fixed period even
     * when a read takes a variable amount of time. */
    TickType_t last_wake = xTaskGetTickCount();

    while (1)
    {
        bme280_data_t d;

        /* Trigger one conversion and read back compensated values. */
        const esp_err_t err = bme280_read(dev, &d);
        if (err == ESP_OK)
        {
            /* Stamp the sample now so the time is correct even if it sits in
             * the queue for a while. esp_timer returns microseconds. */
            const telemetry_sample_t sample =
            {
                .timestamp_ms = esp_timer_get_time() / 1000,
                .temperature_c = d.temperature_c,
                .humidity_pct = d.humidity_pct,
                .pressure_hpa = d.pressure_hpa,
            };

            ESP_LOGI(TAG, "%.2f C  %.2f %%RH  %.2f hPa", sample.temperature_c, sample.humidity_pct, sample.pressure_hpa);

            /* Hand off to the Wi-Fi task; this never touches the network. */
            queue_push_overwrite(&sample);
        }
        else
        {
            /* A failed read is logged and skipped; the next cycle retries. */
            ESP_LOGE(TAG, "BME280 read failed: %s", esp_err_to_name(err));
        }

        /* Puts the task to sleep until the next sample period. It is
           measured from the last wake time to maintain a consistent sampling period. */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONFIG_HUB_SAMPLE_PERIOD_MS));
    }
}

/** @brief Task B: drain the queue and send each sample once Wi-Fi is up. */
static void publish_task(void *arg)
{
    (void)arg;

    while (1)
    {
        /* Retrieve the next sample from the queue. */
        telemetry_sample_t sample;

        /* Sleep (using no CPU) until the sensor task queues something. */
        xQueueReceive(s_queue, &sample, portMAX_DELAY);

        /* If the link is down, wait here. Meanwhile the sensor task keeps
         * running and the queue buffers new readings. */
        wifi_station_wait_connected(portMAX_DELAY);

        const esp_err_t err = telemetry_publish(&sample);
        if (err != ESP_OK)
        {
            /* No retry: a stale reading is less useful than the next one. */
            ESP_LOGW(TAG, "Publish failed (%s), sample dropped", esp_err_to_name(err));
        }
    }
}

/** @brief Initialise NVS, erasing it if the partition layout changed. Needed for Wi-Fi and other subsystems that rely on NVS. */
static void init_nvs(void)
{
    /* The Wi-Fi driver stores calibration data in NVS, so it must be ready. */
    esp_err_t err = nvs_flash_init();

    /* If the NVS partition was truncated or is from a new version, erase and retry. */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        /* Partition is full or from an older IDF version: wipe and retry. */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    /* Step 1: persistent storage (required by Wi-Fi). */
    init_nvs();

    /* Step 2: sensor. Pins, speed and address come from menuconfig (Kconfig). */
    const bme280_config_t sensor_cfg =
    {
        .i2c_port = CONFIG_HUB_I2C_PORT,
        .sda_gpio = CONFIG_HUB_I2C_SDA_GPIO,
        .scl_gpio = CONFIG_HUB_I2C_SCL_GPIO,
        .freq_hz = CONFIG_HUB_I2C_FREQ_HZ,
#ifdef CONFIG_HUB_BME280_ADDR_0x77
        .address = 0x77,
#else
        .address = 0x76,
#endif
    };
    bme280_handle_t sensor;
    /* Aborts with a clear error if the BME280 sensor is missing or miswired. */
    ESP_ERROR_CHECK(bme280_init(&sensor_cfg, &sensor));

    /* Step 3: Wi-Fi. Returns immediately; connecting happens in the background. */
    const wifi_station_params_t wifi =
    {
        .ssid = CONFIG_HUB_WIFI_SSID,
        .password = CONFIG_HUB_WIFI_PASSWORD,
    };
    /* Start the Wi-Fi station; returns immediately. */
    ESP_ERROR_CHECK(wifi_station_start(&wifi));

    /* Step 4: transport (HTTP or MQTT, chosen in menuconfig). */
    const telemetry_config_t tcfg =
    {
        .device_id = CONFIG_HUB_DEVICE_ID,
#ifdef CONFIG_HUB_TRANSPORT_MQTT
        .transport = TELEMETRY_MQTT,
        .mqtt_broker_uri = CONFIG_HUB_MQTT_BROKER_URI,
        .mqtt_topic = CONFIG_HUB_MQTT_TOPIC,
#else
        .transport = TELEMETRY_HTTP,
        .http_url = CONFIG_HUB_HTTP_URL,
#endif
    };
    /* Initialise the telemetry transport (HTTP or MQTT) with the chosen configuration. */
    ESP_ERROR_CHECK(telemetry_init(&tcfg));

    /* Step 5: the queue must exist before either task starts using it. */
    s_queue = xQueueCreate(CONFIG_HUB_QUEUE_LENGTH, sizeof(telemetry_sample_t));
    configASSERT(s_queue);

    /* Step 6: start both tasks. app_main() then returns; the tasks keep running. */
    xTaskCreate(publish_task, "publish", PUBLISH_TASK_STACK, NULL, PUBLISH_TASK_PRIO, NULL);
    xTaskCreate(sensor_task, "sensor", SENSOR_TASK_STACK, sensor, SENSOR_TASK_PRIO, NULL);
}
