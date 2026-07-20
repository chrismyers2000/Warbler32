#include "app_config.h"
#include "status_led.h"
#include "boot_button.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "audio_pipeline.h"
#include "rtsp_server.h"

#include "esp_log.h"
#include "nvs_flash.h"

extern "C" void app_main(void)
{
    // NVS required by WiFi driver and config storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load runtime config from NVS (falls back to config.h defaults)
    ESP_ERROR_CHECK(app_config_load());

    // Status LED: start before WiFi so we show "connecting" immediately
    ESP_ERROR_CHECK(status_led_init());

    // Background task: BOOT button gestures (hold = factory reset,
    // double-tap in setup mode = cycle WiFi channel)
    ESP_ERROR_CHECK(boot_button_start());

    // Connect to WiFi (blocks until connected or gives up)
    ESP_ERROR_CHECK(wifi_manager_start());

    // Config web UI on port 80
    ESP_ERROR_CHECK(web_server_start());

    // Skip audio/RTSP while broadcasting the setup AP: nothing can stream
    // yet anyway, so there's no reason to spend the I2S/PSRAM/CPU budget
    // until the device is actually on a real network.
    if (!wifi_manager_is_ap_mode()) {
        // Start I2S capture and ring buffer
        ESP_ERROR_CHECK(audio_pipeline_start());

        // Start RTSP server — clients connect and get PCM L16 audio
        ESP_ERROR_CHECK(rtsp_server_start());
    }

    // All work is done in tasks; app_main can return
}
