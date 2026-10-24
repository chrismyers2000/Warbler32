#include "usb_mic.h"
#include "app_config.h"
#include "config.h"
#include "audio_dsp.h"
#include "audio_pipeline.h"

#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "usb_mic";

#define MAX_CHANNELS            2
#define MAX_SUBFRAME_BYTES      4   // 24-bit audio may be carried in 3- or 4-byte subframes
#define MAX_FRAME_BYTES         (MAX_CHANNELS * MAX_SUBFRAME_BYTES)
#define USB_MIC_BUFFER_PACKETS  4
#define USB_MIC_DSP_SHIFT       16  // full-scale 32-bit -> 16-bit; USB mics have no gain_shift setting

typedef struct {
    uint8_t addr;
    uint8_t iface_num;
} rx_connected_t;

// volatile: written by the driver's disconnect callback task, read by the
// audio reader task (see snapshot in usb_mic_read)
static uac_host_device_handle_t volatile s_dev = NULL;
static QueueHandle_t            s_connect_q = NULL;
static audio_dsp_state_t        s_dsp;
static uint8_t                  s_channels  = 1;
static uint8_t                  s_subframe  = 2;
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

// Two attempts at auto-recovering here (calling uac_host_device_close()
// synchronously in this callback, then from a separate deferred task
// instead) both crashed the device — an interrupt watchdog panic and a
// spinlock_acquire assertion respectively. This is a real fragility in
// the vendored usb_host_uac driver around closing a device after its own
// disconnect handling, not something safe to work around without a much
// deeper look at that component (or an upstream fix). Left as log-only;
// a reboot is the only way to pick the mic back up after a real unplug.
static void device_event_cb(uac_host_device_handle_t handle,
                             const uac_host_device_event_t event, void *arg)
{
    switch (event) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "USB microphone disconnected");
        s_dev = NULL;
        audio_pipeline_mark_inactive();
        audio_pipeline_retry_usb_mic();
        break;
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "USB microphone transfer error");
        break;
    default:
        break;
    }
}

// Pick the alt setting whose sample rate exactly matches g_config.sample_rate
// (16-bit preferred, 24-bit accepted). A mismatch would desync the RTSP/RTP timing,
// which bakes in g_config.sample_rate, so we require an exact match rather than a nearest one.
static bool find_alt_for_bits(uac_host_dev_info_t *dev_info, uint8_t bits,
                              uac_host_dev_alt_param_t *out)
{
    bool found = false;

    for (uint8_t alt = 1; alt <= dev_info->iface_alt_num; alt++) {
        uac_host_dev_alt_param_t p;
        if (uac_host_get_device_alt_param(s_dev, alt, &p) != ESP_OK) continue;
        if (p.bit_resolution != bits) continue;
        if (p.subframe_size < sizeof(int16_t) || p.subframe_size > MAX_SUBFRAME_BYTES) continue;
        if (p.channels < 1 || p.channels > MAX_CHANNELS) continue;

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

static bool find_alt_setting(uac_host_dev_info_t *dev_info, uac_host_dev_alt_param_t *out)
{
    static const uint8_t bit_prefs[] = { 16, 24 };

    for (size_t b = 0; b < sizeof(bit_prefs); b++) {
        if (find_alt_for_bits(dev_info, bit_prefs[b], out)) return true;
    }
    return false;
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
        // The alt setting isn't chosen until after the device is opened, so
        // size for the worst-case frame.
        .buffer_size      = RTP_SAMPLES_PER_PACKET * MAX_FRAME_BYTES * USB_MIC_BUFFER_PACKETS,
        .buffer_threshold = RTP_SAMPLES_PER_PACKET * MAX_FRAME_BYTES,
        .callback         = device_event_cb,
        .callback_arg     = NULL,
    };
    uac_host_device_handle_t dev = NULL;
    ret = uac_host_device_open(&dev_config, &dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_dev = dev;

    uac_host_dev_info_t dev_info;
    uac_host_get_device_info(s_dev, &dev_info);
    ESP_LOGI(TAG, "USB mic found — VID=0x%04x PID=0x%04x, %d alt setting(s)",
             dev_info.VID, dev_info.PID, dev_info.iface_alt_num);

    uac_host_dev_alt_param_t alt = {0};
    if (!find_alt_setting(&dev_info, &alt)) {
        ESP_LOGE(TAG, "USB mic does not support %lu Hz / 16 or 24-bit",
                 (unsigned long)g_config.sample_rate);
        uac_host_device_close(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_channels = alt.channels;
    s_subframe = alt.subframe_size;

    uac_host_stream_config_t stream_config = {
        .channels       = alt.channels,
        .bit_resolution = alt.bit_resolution,
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
    // Snapshot once: the disconnect callback nulls s_dev from another task
    // at any moment, so re-reading the global after the check could hand
    // the driver a stale handle mid-call.
    uac_host_device_handle_t dev = s_dev;
    if (!dev) return 0;

    // Raw interleaved frames, converted to full-scale 32-bit mono for the DSP,
    // which does gain/filtering at full precision and reduces to 16-bit.
    static uint8_t raw[RTP_SAMPLES_PER_PACKET * MAX_FRAME_BYTES];
    static int32_t mono[RTP_SAMPLES_PER_PACKET];
    const size_t frame_bytes = (size_t)s_channels * s_subframe;
    size_t want = (count < RTP_SAMPLES_PER_PACKET) ? count : RTP_SAMPLES_PER_PACKET;

    uint32_t bytes_read = 0;
    esp_err_t ret = uac_host_device_read(dev, raw, want * frame_bytes,
                                         &bytes_read, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) return 0;

    size_t frames = bytes_read / frame_bytes;
    for (size_t i = 0; i < frames; i++) {
        const uint8_t *f = raw + i * frame_bytes;
        int64_t sum = 0;
        for (uint8_t c = 0; c < s_channels; c++) {
            // Little-endian, left-justified in the subframe: place the bytes
            // at the top of a 32-bit word to get a full-scale sample.
            const uint8_t *smp = f + (size_t)c * s_subframe;
            uint32_t v = 0;
            for (uint8_t b = 0; b < s_subframe; b++)
                v |= (uint32_t)smp[b] << (32 - 8 * (s_subframe - b));
            sum += (int32_t)v;
        }
        mono[i] = (int32_t)(sum / s_channels);
    }

    audio_dsp_process(&s_dsp, mono, buf, frames, USB_MIC_DSP_SHIFT);
    return frames;
}
