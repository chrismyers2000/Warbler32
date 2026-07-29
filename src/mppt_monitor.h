#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Optional MPPT_18W_A04 solar charge controller monitor. The board has no
// public datasheet; its protocol was reverse-engineered from captured UART
// output over UART1 RX (see MPPT_UART_* in config.h) —
//
//   AT+BATTLVL=<percent 0-100>,<charging flag 0/1>\r\n
//
// The board only transmits a short burst of these right at its own power-on
// (confirmed by testing: data only ever appeared immediately after a power
// cycle, never during any idle window up to 10 minutes) — it does not poll
// or report continuously. Its original companion display was wired RX-only
// too, so there's no query command to request a fresh reading on demand;
// whatever value it broadcast at power-up is all there is until the MPPT
// (or the ESP32, if the MPPT is already running) is power-cycled again.
//
// Gracefully absent, same pattern as battery_monitor.h: if no line has ever
// been parsed (board unpowered/not wired), mppt_monitor_present() reads
// false and nothing else is affected.

// Starts the background UART reader task. Call once from app_main. Only
// fails on a genuine UART driver error — no MPPT board present/powered is
// NOT a failure and is handled internally (present() just stays false).
esp_err_t mppt_monitor_init(void);

// True once at least one valid AT+BATTLVL line has been parsed.
bool mppt_monitor_present(void);

// Last reported battery percentage (0-100). 0 if never read.
uint8_t mppt_monitor_percent(void);

// Last reported charging flag. Assumed polarity (nonzero = charging) —
// confirm against real charge/no-charge conditions before relying on it.
bool mppt_monitor_charging(void);

#ifdef __cplusplus
}
#endif
