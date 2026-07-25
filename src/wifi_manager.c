#include "wifi_manager.h"
#include "status_led.h"
#include "app_config.h"
#include "config.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi";

// Set once we're either connected to a real network or broadcasting the
// setup AP — either way, wifi_manager_start() can stop waiting.
#define WIFI_READY_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;
static int  s_retry_count    = 0;
static bool s_ap_mode        = false;
static bool s_wifi_started   = false;
static bool s_want_ap        = false;
// Set on the first IP_EVENT_STA_GOT_IP. Distinguishes the initial boot-time
// connection attempt (bounded retries, falls back to the setup AP) from a
// later runtime drop (retries forever — falling back to the setup AP would
// tear down STA and kill an already-running RTSP stream for no good reason).
static bool s_ever_connected = false;

// Set on every WIFI_EVENT_STA_DISCONNECTED that starts a new outage after a
// successful boot connection (the "keep retrying forever" branch below), to
// the esp_timer_get_time() at which this outage began; cleared back to 0 on
// reconnect. Read by wifi_fallback_task() to decide when to bring up the
// backup AP.
static _Atomic int64_t s_disconnected_since_us = 0;

// True while the backup AP is up specifically because a saved network
// became unreachable (timeout or manual BOOT-button toggle) — as opposed to
// s_ap_mode alone, which is also true for the original "no saved network
// configured yet" setup AP. Distinguishes the two for the web UI banner and
// the BOOT-button toggle logic (see wifi_manager_toggle_fallback_ap()).
static bool s_fallback_mode = false;

// Mirrors whether STA currently has an IP, so exit_fallback_ap_mode() can
// restore the right status LED without re-deriving it.
static bool s_sta_connected = false;

// True for the duration of wifi_manager_scan(). ESP-IDF refuses to scan
// while STA is mid-connect-attempt (esp_wifi_scan_start returns
// ESP_ERR_WIFI_STATE) — during a real outage the disconnect handler below
// calls esp_wifi_connect() again on every single WIFI_EVENT_STA_DISCONNECTED,
// so without this a scan from the backup AP page would almost always lose
// that race. Sat by wifi_manager_scan() to skip that one connect call while
// a scan is in flight; it resumes the retry loop itself once the scan ends.
static atomic_bool s_scan_suppress_connect = false;

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

    // APSTA rather than AP-only: STA stays present (idle, since s_want_ap
    // blocks the auto-connect above) so a WiFi scan is still possible from
    // this page — see wifi_manager_scan(). Harmless with no saved network:
    // there's nothing for STA to do either way.
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    configure_ap(channel);

    s_ap_mode = true;
    status_led_set(LED_SETUP);
    ESP_LOGW(TAG, "setup AP \"%s\" up at 192.168.4.1 (password: %s) — connect and browse there to configure WiFi",
             WIFI_AP_SSID, WIFI_AP_PASSWORD);
}

// Brings up the backup AP without disturbing STA — concurrent AP+STA, so
// whatever might reconnect on its own keeps trying the whole time. Shared
// by wifi_fallback_task() (timeout-triggered) and
// wifi_manager_toggle_fallback_ap() (manual BOOT-button toggle).
static void enter_fallback_ap_mode(void)
{
    uint8_t channel = pick_least_congested_channel();

    for (size_t i = 0; i < sizeof(kManualChannels) / sizeof(kManualChannels[0]); i++) {
        if (kManualChannels[i] == channel) { s_channel_idx = (int)i; break; }
    }

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    configure_ap(channel);

    s_ap_mode       = true;
    s_fallback_mode = true;
    status_led_set(LED_SETUP);
    ESP_LOGW(TAG, "backup AP \"%s\" up at 192.168.4.1 (password: %s) — WiFi unreachable, still retrying in the background",
             WIFI_AP_SSID, WIFI_AP_PASSWORD);
}

// Drops the backup AP, back to STA-only. Safe whether STA just reconnected
// (self-heal) or is still retrying (manual toggle-off).
static void exit_fallback_ap_mode(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_mode       = false;
    s_fallback_mode = false;
    // Restart the outage clock rather than leaving it running: if WiFi is
    // still actually down, a manual toggle-off means "not right now," not
    // "show it again immediately" — this buys another full timeout period.
    atomic_store(&s_disconnected_since_us, s_sta_connected ? 0 : esp_timer_get_time());
    status_led_set(s_sta_connected ? LED_CONNECTED : LED_CONNECTING);
    ESP_LOGW(TAG, "backup AP down");
}

// Polls for a WiFi outage that's outlasted the configured timeout and, if
// so, brings up the backup AP — see WIFI_FALLBACK_* in config.h for why.
static void wifi_fallback_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_FALLBACK_CHECK_INTERVAL_MS));

        if (!g_config.wifi_fallback_enabled || s_fallback_mode) continue;

        int64_t since = atomic_load(&s_disconnected_since_us);
        if (since == 0) continue;

        int64_t elapsed_min = (esp_timer_get_time() - since) / 60000000;
        if (elapsed_min >= g_config.wifi_fallback_timeout_min) {
            ESP_LOGW(TAG, "WiFi unreachable for %d+ min — bringing up backup AP",
                     g_config.wifi_fallback_timeout_min);
            enter_fallback_ap_mode();
        }
    }
}

// BOOT-button double-tap handler — see wifi_manager.h for the exact
// semantics of each branch.
void wifi_manager_toggle_fallback_ap(void)
{
    if (s_fallback_mode) {
        exit_fallback_ap_mode();
    } else if (!s_ap_mode) {
        enter_fallback_ap_mode();
    } else {
        ESP_LOGI(TAG, "double-press ignored (no known network to fall back to)");
    }
}

// See wifi_manager.h for the contract.
int wifi_manager_scan(wifi_scan_result_t *out, int max_results)
{
    atomic_store(&s_scan_suppress_connect, true);

    wifi_scan_config_t scan_cfg = {0};  // all channels, active scan, default dwell time
    esp_err_t ret = ESP_FAIL;
    // A reconnect attempt already in flight when this call started (there's
    // a gap between suppressing future ones above and this actually
    // running) still collides — ESP_ERR_WIFI_STATE means "busy, try again
    // shortly," not a real failure, so retry a few times rather than
    // surfacing it to the user immediately.
    for (int attempt = 0; attempt < 5; attempt++) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_wifi_scan_start(&scan_cfg, true /* block */);
        if (ret == ESP_OK || ret != ESP_ERR_WIFI_STATE) break;
    }

    atomic_store(&s_scan_suppress_connect, false);
    // Resume the reconnect loop ourselves: suppressing the disconnect
    // handler's esp_wifi_connect() call above means nothing else will kick
    // it again on its own if we're still mid-outage.
    if (s_ever_connected && !s_sta_connected) esp_wifi_connect();

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(ret));
        return -1;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) return 0;

    wifi_ap_record_t *records = calloc(num, sizeof(*records));
    if (!records) return -1;
    esp_wifi_scan_get_ap_records(&num, records);

    int count = 0;
    for (int i = 0; i < num && count < max_results; i++) {
        if (records[i].ssid[0] == '\0') continue;  // hidden network — nothing to show/pick

        int existing = -1;
        for (int j = 0; j < count; j++) {
            if (strcmp((char *)records[i].ssid, out[j].ssid) == 0) { existing = j; break; }
        }
        if (existing >= 0) {
            // Same SSID seen on another channel/BSSID — keep the stronger one.
            if (records[i].rssi > out[existing].rssi) out[existing].rssi = records[i].rssi;
            continue;
        }

        strlcpy(out[count].ssid, (char *)records[i].ssid, sizeof(out[count].ssid));
        out[count].rssi   = records[i].rssi;
        out[count].secure = records[i].authmode != WIFI_AUTH_OPEN;
        count++;
    }
    free(records);

    // Sort strongest-first (insertion sort — result sets are small).
    for (int i = 1; i < count; i++) {
        wifi_scan_result_t tmp = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < tmp.rssi) { out[j + 1] = out[j]; j--; }
        out[j + 1] = tmp;
    }

    return count;
}

// Advance to the next channel in the manual rotation. No-op if not currently
// broadcasting the setup AP. Returns the newly active channel, or 0.
// esp_wifi_set_max_tx_power() takes int8_t units of 0.25 dBm; the config
// field is plain dBm, so convert at the call site rather than storing the
// odd unit in NVS/the web UI.
void wifi_manager_apply_tx_power(void)
{
    int8_t units = (int8_t)(g_config.wifi_tx_power_dbm * 4);
    esp_err_t ret = esp_wifi_set_max_tx_power(units);
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "WiFi TX power set to %d dBm", g_config.wifi_tx_power_dbm);
    else
        ESP_LOGW(TAG, "esp_wifi_set_max_tx_power failed: %s", esp_err_to_name(ret));
}

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
        if (s_ever_connected) {
            // Runtime drop after a successful boot connection — keep
            // retrying indefinitely rather than falling back to the setup
            // AP, which would tear down STA and kill the running stream.
            // wifi_fallback_task() brings up a *concurrent* backup AP on
            // its own timeout if this drags on, without touching STA here.
            ESP_LOGW(TAG, "WiFi dropped, reconnecting...");
            s_sta_connected = false;
            if (!s_fallback_mode) status_led_set(LED_CONNECTING);
            if (atomic_load(&s_disconnected_since_us) == 0)
                atomic_store(&s_disconnected_since_us, esp_timer_get_time());
            // Skip this one reconnect attempt if a scan is in flight — see
            // s_scan_suppress_connect. wifi_manager_scan() itself kicks the
            // retry loop again once the scan ends, so this isn't lost, just
            // deferred.
            if (!atomic_load(&s_scan_suppress_connect)) esp_wifi_connect();
        } else if (s_retry_count < WIFI_MAX_RETRIES) {
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
        s_ever_connected = true;
        s_sta_connected  = true;
        atomic_store(&s_disconnected_since_us, 0);
        // Self-heal: the saved network is back, so the temporary backup AP
        // (if the outage ran long enough to trigger it) is no longer
        // needed. The original "no saved network yet" setup AP never
        // reaches this branch in the first place — reconnecting to nothing
        // isn't possible — so this can't misfire on that case.
        if (s_fallback_mode) exit_fallback_ap_mode();
        status_led_set(LED_CONNECTED);

        // WiFi modem-sleep power save (IDF default: WIFI_PS_MIN_MODEM) puts
        // the radio to sleep between beacons, waking only on the DTIM
        // interval (~100ms here) — fine for idle traffic, but it injects a
        // periodic ~100ms delivery stall into anything latency-sensitive.
        // For continuous RTP/HTTP audio streaming that shows up as a
        // rhythmic glitch roughly at the DTIM rate. This device is always
        // mains/battery powered for streaming, not battery-idle, so the
        // power saved isn't worth the audio artifact.
        esp_wifi_set_ps(WIFI_PS_NONE);

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

bool wifi_manager_is_fallback_mode(void)
{
    return s_fallback_mode;
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

    // Left registered for the life of the device (never unregistered below)
    // so a post-boot disconnect is still handled — see WIFI_EVENT_STA_DISCONNECTED.
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    status_led_set(LED_CONNECTING);

    if (g_config.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "no WiFi configured");
        enter_ap_mode();
    } else {
        wifi_config_t wifi_cfg = {};
        strlcpy((char *)wifi_cfg.sta.ssid,     g_config.wifi_ssid,     sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, g_config.wifi_password, sizeof(wifi_cfg.sta.password));
        // A WPA2 threshold would refuse to associate with an open AP, and
        // the web UI explicitly supports blank-password open networks.
        wifi_cfg.sta.threshold.authmode =
            g_config.wifi_password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_LOGI(TAG, "connecting to \"%s\"...", g_config.wifi_ssid);
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    }

    // Radio is up in either branch above (STA or setup-AP) — apply the
    // configured TX power ceiling now rather than waiting for STA to fully
    // associate, since the setup AP's own beacon/handshake benefits too.
    wifi_manager_apply_tx_power();

    if (s_ap_mode) xEventGroupSetBits(s_wifi_event_group, WIFI_READY_BIT);

    xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_READY_BIT,
        pdFALSE, pdFALSE,
        portMAX_DELAY);

    // s_wifi_event_group is intentionally not deleted here: IP_EVENT_STA_GOT_IP
    // sets WIFI_READY_BIT on every reconnect, not just the first one, so the
    // handle must stay valid for the device's lifetime. The extra SetBits
    // calls after boot are harmless no-ops since nothing waits on it anymore.

    xTaskCreatePinnedToCore(
        wifi_fallback_task, "wifi_fallback",
        TASK_WIFI_FALLBACK_STACK, NULL,
        TASK_WIFI_FALLBACK_PRIORITY, NULL,
        TASK_WIFI_FALLBACK_CORE);

    return ESP_OK;
}
