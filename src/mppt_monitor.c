#include "mppt_monitor.h"
#include "config.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "mppt";

static atomic_bool     s_present;
static _Atomic uint8_t s_percent;
static atomic_bool     s_charging;

// Line buffer for uart_read_bytes() output. AT+BATTLVL=100,1\r\n is 18
// bytes at most — this is generous headroom for a noisy/partial first read.
#define LINE_BUF_SIZE 64

static void mppt_task(void *arg)
{
    uint8_t  raw[MPPT_UART_BUF_SIZE];
    char     line[LINE_BUF_SIZE];
    size_t   line_len = 0;

    for (;;) {
        int len = uart_read_bytes(MPPT_UART_PORT, raw, sizeof(raw), pdMS_TO_TICKS(1000));
        for (int i = 0; i < len; i++) {
            char c = (char)raw[i];
            if (c == '\n' || c == '\r') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    int pct = -1, chg = -1;
                    if (sscanf(line, "AT+BATTLVL=%d,%d", &pct, &chg) == 2 &&
                        pct >= 0 && pct <= 100 && (chg == 0 || chg == 1)) {
                        atomic_store(&s_percent, (uint8_t)pct);
                        atomic_store(&s_charging, chg != 0);
                        if (!atomic_exchange(&s_present, true))
                            ESP_LOGI(TAG, "MPPT detected: %d%%, %s",
                                     pct, chg ? "charging" : "not charging");
                    }
                    line_len = 0;
                }
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            } else {
                // Line too long to be a valid AT+BATTLVL reply — desynced
                // mid-stream; drop it and resync on the next terminator.
                line_len = 0;
            }
        }
    }
}

esp_err_t mppt_monitor_init(void)
{
    uart_config_t cfg = {
        .baud_rate = MPPT_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(MPPT_UART_PORT, MPPT_UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(MPPT_UART_PORT, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(MPPT_UART_PORT, UART_PIN_NO_CHANGE, MPPT_UART_RX_GPIO,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        mppt_task, "mppt",
        TASK_MPPT_STACK, NULL,
        TASK_MPPT_PRIORITY, NULL,
        TASK_MPPT_CORE);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

bool mppt_monitor_present(void)
{
    return atomic_load(&s_present);
}

uint8_t mppt_monitor_percent(void)
{
    return atomic_load(&s_percent);
}

bool mppt_monitor_charging(void)
{
    return atomic_load(&s_charging);
}
