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
    uint16_t noise_gate;
    uint8_t  audio_source;   // AUDIO_SOURCE_I2S or AUDIO_SOURCE_USB
} app_config_t;

extern app_config_t g_config;

esp_err_t app_config_load(void);            // load NVS → g_config, fall back to config.h defaults
esp_err_t app_config_save(void);            // write g_config → NVS
esp_err_t app_config_factory_reset(void);   // erase saved config; next boot uses config.h defaults

#ifdef __cplusplus
}
#endif
