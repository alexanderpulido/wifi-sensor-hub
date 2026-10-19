/**
 * @file bme280.h
 * @brief Minimal BME280 (temperature / humidity / pressure) driver for ESP-IDF.
 *
 * Uses the ESP-IDF >= 5.2 I2C master driver. The sensor is operated in
 * *forced mode*: each bme280_read() triggers a single conversion, after which
 * the chip returns to sleep. Raw ADC values are converted with the integer
 * compensation algorithms from the Bosch BME280 datasheet (section 4.2.3).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Opaque driver handle (the struct is defined in bme280.c). */
typedef struct bme280_dev *bme280_handle_t;

/** @brief I2C and device configuration. */
typedef struct
{
    int i2c_port;  /**< I2C controller number (0 or 1). */
    int sda_gpio;  /**< SDA pin. */
    int scl_gpio;  /**< SCL pin. */
    uint32_t freq_hz;   /**< I2C clock frequency in Hz. */
    uint8_t address;   /**< 7-bit I2C address (0x76 or 0x77 depending on if the SDO pin is tied to GND or 3.3V). */
} bme280_config_t;

/** @brief Our output compensated measurement in physical units. */
typedef struct
{
    float temperature_c;  /**< Temperature in degrees Celsius. */
    float humidity_pct;   /**< Relative humidity in %RH. */
    float pressure_hpa;   /**< Barometric pressure in hPa. */
} bme280_data_t;

/**
 * @brief Create the I2C bus, probe the chip, and load its calibration data.
 *
 * @param[in]  cfg  Bus and device configuration.
 * @param[out] out  Receives the driver handle on success.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if the chip ID does not match,
 *         or another error code from the I2C driver.
 */
esp_err_t bme280_init(const bme280_config_t *cfg, bme280_handle_t *out);

/**
 * @brief Trigger one forced-mode conversion and return compensated values.
 *
 * @param[in]  dev  Handle from bme280_init().
 * @param[out] out  Receives the measurement.
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if the conversion did not finish,
 *         or an I2C error code.
 */
esp_err_t bme280_read(bme280_handle_t dev, bme280_data_t *out);

/**
 * @brief Release the device and the I2C bus.
 *
 * @param[in] dev Handle from bme280_init(); invalid after this call.
 */
void bme280_deinit(bme280_handle_t dev);

#ifdef __cplusplus
}
#endif
