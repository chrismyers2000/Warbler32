#include "app_config.h"
#include "config.h"

#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config";
#define NVS_NS "warbler32"

app_config_t g_config;

esp_err_t app_config_load(void)
{
    // Apply compile-time defaults first
    strlcpy(g_config.device_name,    DEVICE_NAME_DEFAULT, sizeof(g_config.device_name));
    strlcpy(g_config.wifi_ssid,      WIFI_SSID,           sizeof(g_config.wifi_ssid));
    strlcpy(g_config.wifi_password,  WIFI_PASSWORD,       sizeof(g_config.wifi_password));
    g_config.sample_rate    = AUDIO_SAMPLE_RATE;
    g_config.gain_shift     = AUDIO_GAIN_SHIFT;
    g_config.gain_mult      = AUDIO_GAIN_MULT;
    g_config.led_brightness = NEOPIXEL_BRIGHTNESS;
    g_config.hpf_freq       = AUDIO_HPF_FREQ;
    g_config.hpf_slope      = AUDIO_HPF_SLOPE;
    g_config.hpf_depth      = AUDIO_HPF_DEPTH;
    g_config.audio_source   = AUDIO_SOURCE_DEFAULT;
    g_config.mic_model      = MIC_MODEL_DEFAULT;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        // Namespace not written yet — defaults are fine
        ESP_LOGI(TAG, "no saved config, using defaults");
        return ESP_OK;
    }

    size_t len;

    len = sizeof(g_config.device_name);
    nvs_get_str(h, "devname",     g_config.device_name,   &len);
    len = sizeof(g_config.wifi_ssid);
    nvs_get_str(h, "ssid",        g_config.wifi_ssid,     &len);
    len = sizeof(g_config.wifi_password);
    nvs_get_str(h, "password",    g_config.wifi_password, &len);
    nvs_get_u32(h, "sample_rate", &g_config.sample_rate);
    nvs_get_u8 (h, "gain_shift",  &g_config.gain_shift);
    nvs_get_u8 (h, "gain_mult",   &g_config.gain_mult);
    nvs_get_u8 (h, "led_bright",  &g_config.led_brightness);
    nvs_get_u16(h, "hpf_freq",    &g_config.hpf_freq);
    nvs_get_u8 (h, "hpf_slope",   &g_config.hpf_slope);
    nvs_get_u8 (h, "hpf_depth",   &g_config.hpf_depth);
    nvs_get_u8 (h, "audio_src",   &g_config.audio_source);
    nvs_get_u8 (h, "mic_model",   &g_config.mic_model);

    nvs_close(h);

    ESP_LOGI(TAG, "loaded: name=\"%s\" ssid=\"%s\" rate=%lu shift=%d mult=%d bright=%d",
             g_config.device_name, g_config.wifi_ssid, (unsigned long)g_config.sample_rate,
             g_config.gain_shift, g_config.gain_mult, g_config.led_brightness);
    return ESP_OK;
}

esp_err_t app_config_save(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    nvs_set_str(h, "devname",     g_config.device_name);
    nvs_set_str(h, "ssid",        g_config.wifi_ssid);
    nvs_set_str(h, "password",    g_config.wifi_password);
    nvs_set_u32(h, "sample_rate", g_config.sample_rate);
    nvs_set_u8 (h, "gain_shift",  g_config.gain_shift);
    nvs_set_u8 (h, "gain_mult",   g_config.gain_mult);
    nvs_set_u8 (h, "led_bright",  g_config.led_brightness);
    nvs_set_u16(h, "hpf_freq",    g_config.hpf_freq);
    nvs_set_u8 (h, "hpf_slope",   g_config.hpf_slope);
    nvs_set_u8 (h, "hpf_depth",   g_config.hpf_depth);
    nvs_set_u8 (h, "audio_src",   g_config.audio_source);
    nvs_set_u8 (h, "mic_model",   g_config.mic_model);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "saved");
    return ESP_OK;
}

esp_err_t app_config_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGW(TAG, "factory reset — saved config erased");
    return ESP_OK;
}
