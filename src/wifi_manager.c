#include "wifi_manager.h"
#include "status_led.h"
#include "app_config.h"
#include "config.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi";

// Set once we're either connected to a real network or broadcasting the
// setup AP — either way, wifi_manager_start() can stop waiting.
#define WIFI_READY_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;
static int  s_retry_count   = 0;
static bool s_ap_mode       = false;
static bool s_wifi_started  = false;
static bool s_want_ap       = false;

// Manual BOOT-button-double-press rotation. Includes channel 1, unlike the
// automatic scan's candidates — the auto-picker avoids it based on evidence
// from this project's own test environment, but that may not generalize to
// every deployment, so a manual override can still reach it.
static const uint8_t kManualChannels[] = { 1, 6, 11 };
static int s_channel_idx = 0;

// Survey nearby 2.4GHz networks and return whichever of the standard
// non-overlapping channels has the fewest networks on it. Channel 1 is
// deliberately excluded: in real-world testing it repeatedly caused WPA2
// handshake failures despite the scan reporting zero competing APs on it —
// this scan only counts other WiFi access points' beacons, so it can't see
// non-WiFi 2.4GHz interference (baby monitors, cordless phones, etc.), which
// better explains the mismatch than coincidence. Requires WiFi already
// started in a STA-capable mode. Always returns a usable channel (falls back
// to 6 if the scan fails) rather than blocking AP startup — this gates the
// device's only config UI.
static uint8_t pick_least_congested_channel(void)
{
    static const uint8_t candidates[] = { 6, 11 };
    int counts[2] = {0, 0};

    wifi_scan_config_t scan_cfg = {0};  // all channels, active scan, default dwell time
    if (esp_wifi_scan_start(&scan_cfg, true /* block */) != ESP_OK) {
        ESP_LOGW(TAG, "channel scan failed, defaulting to channel %d", candidates[0]);
        return candidates[0];
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) return candidates[0];

    wifi_ap_record_t *records = calloc(num, sizeof(*records));
    if (!records) return candidates[0];
    esp_wifi_scan_get_ap_records(&num, records);

    for (int i = 0; i < num; i++) {
        for (int c = 0; c < 2; c++) {
            if (records[i].primary == candidates[c]) { counts[c]++; break; }
        }
    }
    free(records);

    int best = (counts[1] < counts[0]) ? 1 : 0;

    ESP_LOGI(TAG, "channel survey: ch6=%d ch11=%d networks nearby, using ch%d",
             counts[0], counts[1], candidates[best]);
    return candidates[best];
}

// Build and apply the AP config for the given channel. Shared by initial
// setup-AP entry and manual channel cycling so the SSID/password/PMF setup
// isn't duplicated.
static void configure_ap(uint8_t channel)
{
    wifi_config_t ap_cfg = {
        .ap = {
            .channel        = channel,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            // Matches Espressif's current SoftAP reference example. Leaving
            // this unset (ambiguous PMF) while the beacon still advertises
            // MFP-capable was reproducing as a 4-way handshake timeout
            // (802.11 reason 15) for every client, iPhone and laptop alike.
            .pmf_cfg = {
                .required = true,
            },
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid,     WIFI_AP_SSID,     sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, WIFI_AP_PASSWORD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(WIFI_AP_SSID);

    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

// Switch to broadcasting the fallback setup AP. Safe to call whether WiFi
// hasn't started yet (blank saved credentials) or is already running in STA
// mode (retries exhausted) — esp_wifi_set_mode() handles the transition.
static void enter_ap_mode(void)
{
    // Prevents the WIFI_EVENT_STA_START handler below from firing an
    // esp_wifi_connect() in response to the esp_wifi_start() a few lines
    // down — that race made esp_wifi_scan_start() fail immediately (STA
    // busy connecting), silently skipping the channel survey entirely.
    s_want_ap = true;

    esp_wifi_set_mode(WIFI_MODE_STA);
    if (!s_wifi_started) {
        esp_wifi_start();
        s_wifi_started = true;
    }
    uint8_t channel = pick_least_congested_channel();

    // Line up the manual rotation so the first double-press moves to a
    // genuinely different channel rather than re-picking this one.
    for (size_t i = 0; i < sizeof(kManualChannels) / sizeof(kManualChannels[0]); i++) {
        if (kManualChannels[i] == channel) { s_channel_idx = (int)i; break; }
    }

    esp_wifi_set_mode(WIFI_MODE_AP);
    configure_ap(channel);

    s_ap_mode = true;
    status_led_set(LED_SETUP);
    ESP_LOGW(TAG, "setup AP \"%s\" up at 192.168.4.1 (password: %s) — connect and browse there to configure WiFi",
             WIFI_AP_SSID, WIFI_AP_PASSWORD);
}

// Advance to the next channel in the manual rotation. No-op if not currently
// broadcasting the setup AP. Returns the newly active channel, or 0.
uint8_t wifi_manager_cycle_ap_channel(void)
{
    if (!s_ap_mode) return 0;

    size_t n = sizeof(kManualChannels) / sizeof(kManualChannels[0]);
    s_channel_idx = (s_channel_idx + 1) % (int)n;
    uint8_t channel = kManualChannels[s_channel_idx];

    configure_ap(channel);
    ESP_LOGW(TAG, "manually switched setup AP to channel %d", channel);
    return channel;
}

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_want_ap) esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "retrying WiFi (%d/%d)...", s_retry_count, WIFI_MAX_RETRIES);
        } else {
            ESP_LOGE(TAG, "failed to connect to \"%s\" after %d retries", g_config.wifi_ssid, WIFI_MAX_RETRIES);
            enter_ap_mode();
            xEventGroupSetBits(s_wifi_event_group, WIFI_READY_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "connected — IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        status_led_set(LED_CONNECTED);

        mdns_init();
        mdns_hostname_set(g_config.device_name);
        mdns_instance_name_set(g_config.device_name);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        mdns_service_add(NULL, "_rtsp", "_tcp", 554, NULL, 0);
        ESP_LOGI(TAG, "reachable at http://%s.local/", g_config.device_name);

        xEventGroupSetBits(s_wifi_event_group, WIFI_READY_BIT);
    }
}

bool wifi_manager_is_ap_mode(void)
{
    return s_ap_mode;
}

esp_err_t wifi_manager_start(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any, inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &inst_got_ip));

    status_led_set(LED_CONNECTING);

    if (g_config.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "no WiFi configured");
        enter_ap_mode();
    } else {
        wifi_config_t wifi_cfg = {};
        strlcpy((char *)wifi_cfg.sta.ssid,     g_config.wifi_ssid,     sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, g_config.wifi_password, sizeof(wifi_cfg.sta.password));
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_LOGI(TAG, "connecting to \"%s\"...", g_config.wifi_ssid);
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    }

    if (s_ap_mode) xEventGroupSetBits(s_wifi_event_group, WIFI_READY_BIT);

    xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_READY_BIT,
        pdFALSE, pdFALSE,
        portMAX_DELAY);

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inst_got_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inst_any);
    vEventGroupDelete(s_wifi_event_group);

    return ESP_OK;
}
