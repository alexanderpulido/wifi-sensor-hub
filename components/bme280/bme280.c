/**
 * @file bme280.c
 * @brief BME280 driver implementation. See bme280.h for the public API.
 */
#include "bme280.h"

#include <stdlib.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bme280";

/* Register map (datasheet section 5.3). */
#define REG_CALIB_T_P   0x88  /**< dig_T1..dig_P9 and dig_H1 (0x88..0xA1). */
#define REG_ID          0xD0  /**< Chip ID register. */
#define REG_RESET       0xE0  /**< Writing RESET_CMD triggers a soft reset. */
#define REG_CALIB_H     0xE1  /**< dig_H2..dig_H6 (0xE1..0xE7). */
#define REG_CTRL_HUM    0xF2  /**< Humidity oversampling; applied on the next ctrl_meas write. */
#define REG_STATUS      0xF3  /**< Bit 3 is set while a conversion is running. */
#define REG_CTRL_MEAS   0xF4  /**< Temp/pressure oversampling and power mode. */
#define REG_DATA        0xF7  /**< press[3], temp[3], hum[2]. */

#define CHIP_ID_BME280  0x60  /**< A BMP280 (no humidity) reports 0x58. */
#define RESET_CMD       0xB6
#define STATUS_MEASURING (1 << 3)

/** ctrl_hum: humidity oversampling x1. */
#define CTRL_HUM_X1     0x01
/** ctrl_meas: temp oversampling x1 (bits 7:5), pressure x1 (4:2), forced mode (1:0). */
#define CTRL_MEAS_FORCED_X1 ((1 << 5) | (1 << 2) | 0x01)

#define I2C_TIMEOUT_MS  100
#define CONVERSION_TIMEOUT_MS 50  /**< x1 oversampling needs ~10 ms; this is generous. */

/**
 * @brief Factory trimming parameters (datasheet table 16).
 *
 * Each chip is calibrated at the factory and stores its own coefficients.
 * The compensation maths below needs them to turn raw ADC values into units.
 */
typedef struct
{
    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1; int16_t H2; uint8_t H3; int16_t H4, H5; int8_t H6;
} calib_t;

/** Driver state behind the opaque bme280_handle_t. */
struct bme280_dev
{
    i2c_master_bus_handle_t bus;  /**< I2C bus this driver created. */
    i2c_master_dev_handle_t dev;  /**< The BME280 on that bus. */
    calib_t calib;                /**< Calibration read once at init. */
};

/** @brief Read @p len bytes starting at register @p reg. */
static esp_err_t read_regs(struct bme280_dev *d, uint8_t reg, uint8_t *buf, size_t len)
{
    /* Write the register address, then read the data back in one transaction. */
    return i2c_master_transmit_receive(d->dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

/** @brief Write one byte to register @p reg. */
static esp_err_t write_reg(struct bme280_dev *d, uint8_t reg, uint8_t val)
{
    const uint8_t frame[2] = {reg, val};
    return i2c_master_transmit(d->dev, frame, sizeof(frame), I2C_TIMEOUT_MS);
}

/** @brief Read the factory calibration coefficients from the chip. */
static esp_err_t load_calibration(struct bme280_dev *d)
{
    uint8_t a[26], h[7];

    /* The coefficients live in two separate register blocks. */
    esp_err_t err = read_regs(d, REG_CALIB_T_P, a, sizeof(a));
    if (err != ESP_OK)
    {
        return err;
    }
    err = read_regs(d, REG_CALIB_H, h, sizeof(h));
    if (err != ESP_OK)
    {
        return err;
    }

    /* Most values are little-endian 16-bit: low byte first. */
    calib_t *c = &d->calib;
    c->T1 = (uint16_t)(a[1] << 8 | a[0]);
    c->T2 = (int16_t)(a[3] << 8 | a[2]);
    c->T3 = (int16_t)(a[5] << 8 | a[4]);
    c->P1 = (uint16_t)(a[7] << 8 | a[6]);
    c->P2 = (int16_t)(a[9] << 8 | a[8]);
    c->P3 = (int16_t)(a[11] << 8 | a[10]);
    c->P4 = (int16_t)(a[13] << 8 | a[12]);
    c->P5 = (int16_t)(a[15] << 8 | a[14]);
    c->P6 = (int16_t)(a[17] << 8 | a[16]);
    c->P7 = (int16_t)(a[19] << 8 | a[18]);
    c->P8 = (int16_t)(a[21] << 8 | a[20]);
    c->P9 = (int16_t)(a[23] << 8 | a[22]);
    c->H1 = a[25];
    c->H2 = (int16_t)(h[1] << 8 | h[0]);
    c->H3 = h[2];
    /* H4/H5 are 12-bit signed values packed across three bytes (0xE4..0xE6):
     * H4 = E4[7:0]:E5[3:0], H5 = E6[7:0]:E5[7:4]. */
    c->H4 = (int16_t)(((int8_t)h[3] * 16) | (h[4] & 0x0F));
    c->H5 = (int16_t)(((int8_t)h[5] * 16) | (h[4] >> 4));
    c->H6 = (int8_t)h[6];
    return ESP_OK;
}

/**
 * @brief Temperature in 0.01 °C (e.g. 2341 = 23.41 °C).
 *
 * Also outputs @p t_fine, an intermediate value that the pressure and
 * humidity formulas need, so temperature must be computed first.
 */
static int32_t comp_temperature(const calib_t *c, int32_t adc_T, int32_t *t_fine)
{
    int32_t v1 = (((adc_T >> 3) - ((int32_t)c->T1 << 1)) * (int32_t)c->T2) >> 11;
    int32_t d  = (adc_T >> 4) - (int32_t)c->T1; 
    int32_t v2 = (((d * d) >> 12) * (int32_t)c->T3) >> 14;
    *t_fine = v1 + v2;
    return (*t_fine * 5 + 128) >> 8;
}

/**
 * @brief Pressure in Pa as Q24.8 fixed point (divide by 256 for Pa).
 *
 * 64-bit maths is required to avoid overflow. Returns 0 if the
 * calibration data would cause a divide by zero.
 */
static uint32_t comp_pressure(const calib_t *c, int32_t adc_P, int32_t t_fine)
{
    int64_t v1 = (int64_t)t_fine - 128000;
    int64_t v2 = v1 * v1 * (int64_t)c->P6;
    v2 += (v1 * (int64_t)c->P5) << 17;
    v2 += (int64_t)c->P4 << 35;
    v1 = ((v1 * v1 * (int64_t)c->P3) >> 8) + ((v1 * (int64_t)c->P2) << 12);
    v1 = (((int64_t)1 << 47) + v1) * (int64_t)c->P1 >> 33;

    if (v1 == 0)
    {
        return 0;
    }

    int64_t p = 1048576 - adc_P;    // initial pressure difference from ADC reading
    p = (((p << 31) - v2) * 3125) / v1;
    v1 = ((int64_t)c->P9 * (p >> 13) * (p >> 13)) >> 25;
    v2 = ((int64_t)c->P8 * p) >> 19;
    p = ((p + v1 + v2) >> 8) + ((int64_t)c->P7 << 4);   // final compensated pressure in Q24.8 format
    return (uint32_t)p;
}

/** @brief Humidity in %RH as Q22.10 fixed point (divide by 1024 for %RH). */
static uint32_t comp_humidity(const calib_t *c, int32_t adc_H, int32_t t_fine)
{
    int32_t v = t_fine - 76800;
    v = (((((adc_H << 14) - ((int32_t)c->H4 << 20) - ((int32_t)c->H5 * v)) + 16384) >> 15) *
         (((((((v * (int32_t)c->H6) >> 10) * (((v * (int32_t)c->H3) >> 11) + 32768)) >> 10) +
            2097152) * (int32_t)c->H2 + 8192) >> 14));  
    v -= (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)c->H1) >> 4);

    /* Clamp to 0..100 %RH; 419430400 is 100 %RH before the final >> 12. */
    if (v < 0)
    {
        v = 0;
    }
    if (v > 419430400)
    {
        v = 419430400;
    }
    return (uint32_t)(v >> 12);
}

/** @brief Free whatever @p d holds; safe on a partially initialised driver. */
static void release(struct bme280_dev *d)
{
    if (d->dev)
    {
        i2c_master_bus_rm_device(d->dev);
    }
    if (d->bus)
    {
        i2c_del_master_bus(d->bus);
    }
    free(d);
}

/** @brief Bring up the bus, probe the chip and load calibration. */
static esp_err_t setup(struct bme280_dev *d, const bme280_config_t *cfg)
{
    /* Step 1: create the I2C bus on the requested pins. */
    const i2c_master_bus_config_t bus_cfg =
    {
        .i2c_port = cfg->i2c_port,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,  /* Filter short noise spikes on the lines. */
        .flags.enable_internal_pullup = true,  /* Most breakouts also have their own. */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &d->bus);
    if (err != ESP_OK)
    {
        return err;
    }

    /* Step 2: register the sensor as a device on that bus. */
    const i2c_device_config_t dev_cfg =
    {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->address,
        .scl_speed_hz = cfg->freq_hz,
    };
    err = i2c_master_bus_add_device(d->bus, &dev_cfg, &d->dev);
    if (err != ESP_OK)
    {
        return err;
    }

    /* Step 3: confirm a BME280 is really there before trusting it. */
    uint8_t id = 0;
    err = read_regs(d, REG_ID, &id, 1);
    if (err != ESP_OK)
    {
        return err;
    }
    if (id != CHIP_ID_BME280)
    {
        ESP_LOGE(TAG, "Unexpected chip ID 0x%02X (BMP280 is 0x58, BME280 is 0x60)", id);
        return ESP_ERR_NOT_FOUND;
    }

    /* Step 4: soft reset to a known state, then load the calibration data. */
    err = write_reg(d, REG_RESET, RESET_CMD);
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(5));  /* Start-up time after reset: 2 ms. */

    return load_calibration(d);
}

esp_err_t bme280_init(const bme280_config_t *cfg, bme280_handle_t *out)
{
    if (!cfg || !out)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct bme280_dev *d = calloc(1, sizeof(*d));
    if (!d)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = setup(d, cfg);
    if (err != ESP_OK)
    {
        release(d);
        return err;
    }

    *out = d;
    return ESP_OK;
}

esp_err_t bme280_read(bme280_handle_t d, bme280_data_t *out)
{
    if (!d || !out)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Forced mode: one conversion per request, then the chip sleeps again.
     * ctrl_hum only takes effect after a write to ctrl_meas, so write it first. */
    esp_err_t err = write_reg(d, REG_CTRL_HUM, CTRL_HUM_X1);
    if (err != ESP_OK)
    {
        return err;
    }
    err = write_reg(d, REG_CTRL_MEAS, CTRL_MEAS_FORCED_X1);
    if (err != ESP_OK)
    {
        return err;
    }

    /* Poll the status register until the "measuring" bit clears. */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONVERSION_TIMEOUT_MS);
    uint8_t status = STATUS_MEASURING;

    while (xTaskGetTickCount() < deadline)
    {
        vTaskDelay(pdMS_TO_TICKS(2));

        err = read_regs(d, REG_STATUS, &status, 1);
        if (err != ESP_OK)
        {
            return err;
        }

        if (!(status & STATUS_MEASURING))
        {
            break;
        }
    }

    if (status & STATUS_MEASURING)
    {
        return ESP_ERR_TIMEOUT;
    }

    /* Burst-read all 8 data bytes in one transaction so they come from the
     * same conversion. */
    uint8_t raw[8];
    err = read_regs(d, REG_DATA, raw, sizeof(raw));
    if (err != ESP_OK)
    {
        return err;
    }

    /* Pressure and temperature are 20-bit (the last nibble is unused);
     * humidity is 16-bit. */
    const int32_t adc_P = (int32_t)((raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4));
    const int32_t adc_T = (int32_t)((raw[3] << 12) | (raw[4] << 4) | (raw[5] >> 4));
    const int32_t adc_H = (int32_t)((raw[6] << 8) | raw[7]);

    /* Temperature first: it produces t_fine, which the other two need. */
    int32_t t_fine;
    out->temperature_c = comp_temperature(&d->calib, adc_T, &t_fine) / 100.0f;
    out->pressure_hpa  = comp_pressure(&d->calib, adc_P, t_fine) / 256.0f / 100.0f;  /* Q24.8 Pa -> hPa */
    out->humidity_pct  = comp_humidity(&d->calib, adc_H, t_fine) / 1024.0f;          /* Q22.10 -> %RH */
    return ESP_OK;
}

void bme280_deinit(bme280_handle_t d)
{
    if (!d)
    {
        return;
    }
    release(d);
}
