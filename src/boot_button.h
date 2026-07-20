#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts a background task that watches the BOOT button (FACTORY_RESET_GPIO)
// while the device is running. Two gestures, both checked only after the app
// is already up — never at power-on, since GPIO0 is a strapping pin and
// holding it low through a reset/power-on puts the chip into the ROM
// bootloader's UART download mode instead of running the app:
//   - Held for FACTORY_RESET_HOLD_MS: erases saved config and reboots.
//   - Double-tapped (two quick presses within DOUBLE_TAP_WINDOW_MS) while
//     broadcasting the setup AP: cycles its WiFi channel. Ignored otherwise.
esp_err_t boot_button_start(void);

#ifdef __cplusplus
}
#endif
