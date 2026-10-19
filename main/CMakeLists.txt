idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES bme280 wifi_station telemetry
                       PRIV_REQUIRES nvs_flash esp_timer)
