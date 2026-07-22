#include "app_config.h"
#include "status_led.h"
#include "battery_monitor.h"
#include "boot_button.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "audio_pipeline.h"
#include "rtsp_server.h"
#include "pipeline_watchdog.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
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

    // Battery voltage monitor (INA219, optional — if absent, the Status
    // card just shows it as not present; never blocks boot)
    ESP_ERROR_CHECK(battery_monitor_init());

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

        // Background: reboots the device if the audio reader task ever
        // stops making progress entirely (wedged driver call, etc) — see
        // pipeline_watchdog.h. User-toggleable in the web UI.
        ESP_ERROR_CHECK(pipeline_watchdog_start());
    }

    // First boot after a web OTA update: everything above came up, so commit
    // this image. Skipping this (i.e. crashing before here) makes the
    // bootloader roll back to the previous firmware on the next reset.
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI("main", "OTA update confirmed valid");
    }

    // All work is done in tasks; app_main can return
}
