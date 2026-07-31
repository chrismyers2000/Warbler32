#include "cpu_temp.h"

#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char *TAG = "cputemp";

static temperature_sensor_handle_t s_handle;

esp_err_t cpu_temp_init(void)
{
    // -10..80C covers outdoor deployment conditions (this device is often
    // tree-mounted, not in a climate-controlled enclosure) at the cost of
    // some accuracy vs. a narrower indoor-only range — the driver logs the
    // error margin it picked for this range at boot.
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

    esp_err_t ret = temperature_sensor_install(&cfg, &s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "temperature_sensor_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return temperature_sensor_enable(s_handle);
}

esp_err_t cpu_temp_read_celsius(float *out_celsius)
{
    return temperature_sensor_get_celsius(s_handle, out_celsius);
}
