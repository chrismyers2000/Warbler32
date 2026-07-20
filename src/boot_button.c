#include "boot_button.h"
#include "app_config.h"
#include "status_led.h"
#include "wifi_manager.h"
#include "config.h"

#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "boot_button";

// Waits up to DOUBLE_TAP_WINDOW_MS for a second press to begin. If one does,
// waits for its release too before returning, so the outer loop doesn't
// immediately re-trigger on the still-held second press.
static bool wait_for_second_tap(void)
{
    TickType_t wait_start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - wait_start) < pdMS_TO_TICKS(DOUBLE_TAP_WINDOW_MS)) {
        if (gpio_get_level(FACTORY_RESET_GPIO) == 0) {
            while (gpio_get_level(FACTORY_RESET_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static void monitor_task(void *arg)
{
    for (;;) {
        if (gpio_get_level(FACTORY_RESET_GPIO) == 0) {
            led_state_t prev_led = status_led_get();
            status_led_set(LED_ERROR);  // orange: "press registered"

            TickType_t press_start = xTaskGetTickCount();
            bool held_to_reset = false;
            while (gpio_get_level(FACTORY_RESET_GPIO) == 0) {
                if ((xTaskGetTickCount() - press_start) >= pdMS_TO_TICKS(FACTORY_RESET_HOLD_MS)) {
                    held_to_reset = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            if (held_to_reset) {
                ESP_LOGW(TAG, "factory reset triggered");
                app_config_factory_reset();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }

            status_led_set(prev_led);

            // Released before the hold threshold — a quick tap. A second one
            // within the window makes this a double-press.
            if (wait_for_second_tap()) {
                if (wifi_manager_is_ap_mode()) {
                    uint8_t ch = wifi_manager_cycle_ap_channel();
                    ESP_LOGI(TAG, "double-press: setup AP switched to channel %d", ch);
                    status_led_flash();
                } else {
                    ESP_LOGI(TAG, "double-press ignored (not broadcasting setup AP)");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t boot_button_start(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << FACTORY_RESET_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    esp_err_t ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) return ret;

    BaseType_t ok = xTaskCreate(monitor_task, "boot_button", 3072, NULL, 1, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
