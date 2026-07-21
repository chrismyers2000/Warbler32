#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Optional INA219 battery-voltage monitor. Gracefully absent: if no INA219
// answers on the I2C bus, battery_monitor_present() just reads false and
// nothing else in the firmware is affected (no blocking boot, no error
// spam — one log line on each present/absent transition only).

// Sets up the I2C bus + INA219 device handle and starts the background
// polling task. Call once from app_main, before WiFi. Only fails on a
// genuine driver error (e.g. bus alloc failure) — a missing/absent INA219
// chip is NOT a failure and is handled internally by the poller.
esp_err_t battery_monitor_init(void);

// True if the last poll got a valid reading from an INA219 at the
// configured address. False before the first poll completes and whenever
// the chip is absent/not responding.
bool battery_monitor_present(void);

// Last known bus voltage in millivolts. 0 if never read / not present.
uint16_t battery_monitor_voltage_mv(void);

#ifdef __cplusplus
}
#endif
