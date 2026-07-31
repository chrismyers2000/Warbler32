#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ESP32-S3's built-in die temperature sensor (driver/temperature_sensor.h).
// This is chip temperature, not ambient — it rises under CPU/WiFi load, so
// treat it as a thermal/diagnostic reading, not a weather sensor. Configured
// for a wide range (see cpu_temp.c) since this device is often deployed
// outdoors rather than in a climate-controlled enclosure.

// Installs and enables the sensor. Call once from app_main. Only fails on
// a genuine driver error — this sensor is always physically present on
// every ESP32-S3, unlike the optional I2C/UART peripherals elsewhere.
esp_err_t cpu_temp_init(void);

// Reads the current die temperature in Celsius.
esp_err_t cpu_temp_read_celsius(float *out_celsius);

#ifdef __cplusplus
}
#endif
