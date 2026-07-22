#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     device_name[32];
    char     wifi_ssid[64];
    char     wifi_password[64];
    uint32_t sample_rate;
    uint8_t  gain_shift;
    uint8_t  gain_mult;
    uint8_t  led_brightness;
    uint16_t hpf_freq;
    uint8_t  hpf_slope;      // 1-4 cascaded stages (6/12/18/24 dB per octave)
    uint8_t  hpf_depth;      // shelf attenuation in dB; 60 = full cut, 0 = bypass
    uint8_t  audio_source;   // AUDIO_SOURCE_I2S or AUDIO_SOURCE_USB
    uint8_t  mic_model;      // MIC_MODEL_INMP441 or MIC_MODEL_SPH0645
    uint8_t  batt_chemistry; // 0=Li-ion/LiPo, 1=LiFePO4, 2=Custom
    uint8_t  batt_cells;     // 1-4 (1S-4S); ignored when batt_chemistry=Custom
    uint16_t batt_low_mv;    // pack-level threshold in mV — always the value
    uint16_t batt_nom_mv;    // actually used, whether preset-derived or custom
    uint16_t batt_full_mv;
    uint8_t  wifi_tx_power_dbm; // max WiFi TX power, 8-20 dBm (see WIFI_TX_POWER_*)
    uint8_t  watchdog_enabled;  // reboot if the audio pipeline stalls (see pipeline_watchdog.h)
} app_config_t;

extern app_config_t g_config;

esp_err_t app_config_load(void);            // load NVS → g_config, fall back to config.h defaults
esp_err_t app_config_save(void);            // write g_config → NVS
esp_err_t app_config_factory_reset(void);   // erase saved config; next boot uses config.h defaults

#ifdef __cplusplus
}
#endif
