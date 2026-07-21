#include "battery_monitor.h"
#include "config.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "battery";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static atomic_bool      s_present;
static _Atomic uint32_t s_voltage_mv;

static void battery_task(void *arg)
{
    for (;;) {
        uint8_t reg = INA219_REG_BUS_VOLTAGE;
        uint8_t raw[2];
        esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, raw, 2,
                                                     pdMS_TO_TICKS(100));
        if (ret == ESP_OK) {
            // Bits 15:3 = 13-bit bus voltage, 4 mV/LSB. Bits 2:0 are
            // CNVR/OVF/reserved — ignored; we never touch the calibration
            // register, so OVF is meaningless here, and the INA219's
            // power-on-reset config already runs continuous conversion, so
            // no config register write is needed at all.
            uint16_t v  = ((uint16_t)raw[0] << 8) | raw[1];
            uint16_t mv = (uint16_t)((v >> 3) * 4);
            atomic_store(&s_voltage_mv, mv);
            if (!atomic_exchange(&s_present, true))
                ESP_LOGI(TAG, "INA219 detected, %u mV", mv);
        } else if (atomic_exchange(&s_present, false)) {
            ESP_LOGW(TAG, "INA219 not responding — battery voltage unavailable");
        }
        vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_INTERVAL_MS));
    }
}

esp_err_t battery_monitor_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = BATTERY_I2C_PORT,
        .sda_io_num = BATTERY_I2C_SDA_GPIO,
        .scl_io_num = BATTERY_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = INA219_I2C_ADDR,
        .scl_speed_hz    = BATTERY_I2C_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        battery_task, "battery",
        TASK_BATTERY_STACK, NULL,
        TASK_BATTERY_PRIORITY, NULL,
        TASK_BATTERY_CORE);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

bool battery_monitor_present(void)
{
    return atomic_load(&s_present);
}

uint16_t battery_monitor_voltage_mv(void)
{
    return (uint16_t)atomic_load(&s_voltage_mv);
}
