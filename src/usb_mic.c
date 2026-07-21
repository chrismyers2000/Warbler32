#include "usb_mic.h"
#include "app_config.h"
#include "config.h"
#include "audio_dsp.h"

#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "usb_mic";

typedef struct {
    uint8_t addr;
    uint8_t iface_num;
} rx_connected_t;

static uac_host_device_handle_t s_dev      = NULL;
static QueueHandle_t            s_connect_q = NULL;
static audio_dsp_state_t        s_dsp;
static uint8_t                  s_channels  = 1;
static bool                     s_host_ready = false;

// Pumps USB Host Library events. Required for every USB Host application,
// regardless of which class driver(s) are installed on top.
static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void driver_event_cb(uint8_t addr, uint8_t iface_num,
                             const uac_host_driver_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        rx_connected_t info = { .addr = addr, .iface_num = iface_num };
        xQueueSend(s_connect_q, &info, 0);
    }
}

static void device_event_cb(uac_host_device_handle_t handle,
                             const uac_host_device_event_t event, void *arg)
{
    switch (event) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "USB microphone disconnected");
        s_dev = NULL;
        break;
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "USB microphone transfer error");
        break;
    default:
        break;
    }
}

// Pick the alt setting whose sample rate exactly matches g_config.sample_rate
// (16-bit only). A mismatch would desync the RTSP/RTP timing, which bakes in
// g_config.sample_rate, so we require an exact match rather than a nearest one.
static bool find_alt_setting(uac_host_dev_info_t *dev_info, uac_host_dev_alt_param_t *out)
{
    bool found = false;

    for (uint8_t alt = 1; alt <= dev_info->iface_alt_num; alt++) {
        uac_host_dev_alt_param_t p;
        if (uac_host_get_device_alt_param(s_dev, alt, &p) != ESP_OK) continue;
        if (p.bit_resolution != 16) continue;

        bool rate_ok = false;
        if (p.sample_freq_type == 0) {
            rate_ok = g_config.sample_rate >= p.sample_freq_lower &&
                      g_config.sample_rate <= p.sample_freq_upper;
        } else {
            for (uint8_t i = 0; i < p.sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) {
                if (p.sample_freq[i] == g_config.sample_rate) { rate_ok = true; break; }
            }
        }
        if (!rate_ok) continue;

        // Prefer mono outright; otherwise remember the first usable match.
        if (p.channels == 1) { *out = p; return true; }
        if (!found) { *out = p; found = true; }
    }

    return found;
}

esp_err_t usb_mic_init(void)
{
    audio_dsp_init(&s_dsp);
    esp_err_t ret;

    // The USB/UAC host stack can only be installed once per boot — retrying
    // usb_mic_init() when no mic is found yet (e.g. from the background
    // retry task in audio_pipeline.c) must skip straight to the wait-for-
    // device step below rather than re-running this one-time setup, or
    // usb_host_install() fails with ESP_ERR_INVALID_STATE on every retry.
    if (!s_host_ready) {
        usb_host_config_t host_config = {
            .skip_phy_setup = false,
            .intr_flags     = ESP_INTR_FLAG_LEVEL1,
        };
        ret = usb_host_install(&host_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(ret));
            return ret;
        }

        BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib",
            TASK_USB_STACK, NULL, TASK_USB_PRIORITY, NULL, TASK_USB_CORE);
        if (ok != pdPASS) return ESP_ERR_NO_MEM;

        s_connect_q = xQueueCreate(1, sizeof(rx_connected_t));
        if (!s_connect_q) return ESP_ERR_NO_MEM;

        uac_host_driver_config_t driver_config = {
            .create_background_task = true,
            .task_priority           = TASK_USB_PRIORITY,
            .stack_size              = TASK_USB_STACK,
            .core_id                 = TASK_USB_CORE,
            .callback                = driver_event_cb,
            .callback_arg            = NULL,
        };
        ret = uac_host_install(&driver_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "uac_host_install failed: %s", esp_err_to_name(ret));
            return ret;
        }

        s_host_ready = true;
    }

    ESP_LOGI(TAG, "waiting for USB microphone...");
    rx_connected_t info;
    bool connected = false;
    for (int waited_s = 0; waited_s < 10; waited_s += 2) {
        if (xQueueReceive(s_connect_q, &info, pdMS_TO_TICKS(2000)) == pdTRUE) {
            connected = true;
            break;
        }
        ESP_LOGI(TAG, "still waiting for USB microphone... (%ds)", waited_s + 2);
    }
    if (!connected) {
        ESP_LOGE(TAG, "no USB microphone detected within 10s");
        return ESP_ERR_NOT_FOUND;
    }

    uac_host_device_config_t dev_config = {
        .addr             = info.addr,
        .iface_num        = info.iface_num,
        .buffer_size      = RTP_SAMPLES_PER_PACKET * sizeof(int16_t) * 4,
        .buffer_threshold = RTP_SAMPLES_PER_PACKET * sizeof(int16_t),
        .callback         = device_event_cb,
        .callback_arg     = NULL,
    };
    ret = uac_host_device_open(&dev_config, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uac_host_dev_info_t dev_info;
    uac_host_get_device_info(s_dev, &dev_info);
    ESP_LOGI(TAG, "USB mic found — VID=0x%04x PID=0x%04x, %d alt setting(s)",
             dev_info.VID, dev_info.PID, dev_info.iface_alt_num);

    uac_host_dev_alt_param_t alt = {0};
    if (!find_alt_setting(&dev_info, &alt)) {
        ESP_LOGE(TAG, "USB mic does not support %lu Hz / 16-bit",
                 (unsigned long)g_config.sample_rate);
        uac_host_device_close(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_channels = alt.channels;

    uac_host_stream_config_t stream_config = {
        .channels       = alt.channels,
        .bit_resolution = 16,
        .sample_freq    = g_config.sample_rate,
        .flags          = 0,
    };
    ret = uac_host_device_start(s_dev, &stream_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_start failed: %s", esp_err_to_name(ret));
        uac_host_device_close(s_dev);
        s_dev = NULL;
        return ret;
    }

    // Prefer the device's own hardware gain over digital gain (avoids
    // amplifying quantization noise). Not all UAC mics implement volume
    // control, so a failure here is non-fatal.
    esp_err_t vol_ret = uac_host_device_set_volume(s_dev, 100);
    if (vol_ret == ESP_OK) {
        ESP_LOGI(TAG, "USB mic hardware volume set to max");
    } else if (vol_ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "USB mic has no hardware volume control");
    } else {
        ESP_LOGW(TAG, "uac_host_device_set_volume failed: %s", esp_err_to_name(vol_ret));
    }

    ESP_LOGI(TAG, "USB mic ready — %lu Hz, %d ch%s",
             (unsigned long)g_config.sample_rate, s_channels,
             s_channels == 1 ? " (mono)" : " (stereo, downmixed to mono)");
    return ESP_OK;
}

size_t usb_mic_read(int16_t *buf, size_t count)
{
    if (!s_dev) return 0;

    if (s_channels == 1) {
        uint32_t bytes_read = 0;
        esp_err_t ret = uac_host_device_read(s_dev, (uint8_t *)buf,
                                             count * sizeof(int16_t),
                                             &bytes_read, pdMS_TO_TICKS(200));
        if (ret != ESP_OK) return 0;

        size_t got = bytes_read / sizeof(int16_t);
        audio_dsp_process(&s_dsp, buf, got);
        return got;
    }

    // Stereo device: read interleaved L/R and downmix to mono.
    static int16_t raw[RTP_SAMPLES_PER_PACKET * 2];
    size_t want = (count < RTP_SAMPLES_PER_PACKET) ? count : RTP_SAMPLES_PER_PACKET;

    uint32_t bytes_read = 0;
    esp_err_t ret = uac_host_device_read(s_dev, (uint8_t *)raw,
                                         want * 2 * sizeof(int16_t),
                                         &bytes_read, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) return 0;

    size_t frames = bytes_read / (2 * sizeof(int16_t));
    for (size_t i = 0; i < frames; i++) {
        int32_t l = raw[i * 2];
        int32_t r = raw[i * 2 + 1];
        buf[i] = (int16_t)((l + r) / 2);
    }

    audio_dsp_process(&s_dsp, buf, frames);
    return frames;
}
