#include "ota_update.h"
#include "config.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

static const char *TAG = "ota";

const char *ota_board_variant(void)
{
#ifdef CONFIG_SPIRAM_MODE_OCT
    return "oct";
#else
    return "quad";
#endif
}

// ---------------------------------------------------------------------------
// OTA write engine
// ---------------------------------------------------------------------------

// Bytes needed before the image can be sanity-checked: the app descriptor
// sits right after the image header and the first segment header.
#define OTA_DESC_END (sizeof(esp_image_header_t) + \
                      sizeof(esp_image_segment_header_t) + \
                      sizeof(esp_app_desc_t))

static struct {
    atomic_bool  busy;
    bool         flashing;      // esp_ota_begin has run (prefix validated)
    esp_ota_handle_t handle;
    const esp_partition_t *update;
    size_t       total;
    size_t       written;       // includes bytes still parked in prefix[]
    size_t       prefix_have;
    uint8_t      prefix[OTA_DESC_END];
} s_eng;

esp_err_t ota_engine_begin(size_t total_size, const char **err_msg)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_eng.busy, &expected, true)) {
        *err_msg = "Another update is already in progress";
        return ESP_ERR_INVALID_STATE;
    }

    s_eng.update = esp_ota_get_next_update_partition(NULL);
    if (s_eng.update == NULL) {
        atomic_store(&s_eng.busy, false);
        *err_msg = "No OTA update partition found";
        return ESP_FAIL;
    }
    if (total_size < OTA_DESC_END || total_size > s_eng.update->size) {
        atomic_store(&s_eng.busy, false);
        *err_msg = "Bad image size (not a firmware image?)";
        return ESP_ERR_INVALID_SIZE;
    }

    s_eng.flashing    = false;
    s_eng.handle      = 0;
    s_eng.total       = total_size;
    s_eng.written     = 0;
    s_eng.prefix_have = 0;
    return ESP_OK;
}

void ota_engine_abort(void)
{
    if (!atomic_load(&s_eng.busy)) return;
    if (s_eng.flashing) esp_ota_abort(s_eng.handle);
    s_eng.flashing = false;
    atomic_store(&s_eng.busy, false);
}

// Validate the buffered image prefix, then start flashing (erases the
// target region — takes a few seconds).
static esp_err_t engine_start_flash(const char **err_msg)
{
    const esp_image_header_t *hdr = (const esp_image_header_t *)s_eng.prefix;
    const esp_app_desc_t *desc = (const esp_app_desc_t *)
        (s_eng.prefix + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));

    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC ||
        desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        *err_msg = "Not an ESP32 firmware image";
        return ESP_FAIL;
    }
    if (hdr->chip_id != ESP_CHIP_ID_ESP32S3) {
        *err_msg = "Firmware is for a different chip (need ESP32-S3)";
        return ESP_FAIL;
    }
    if (strcmp(desc->project_name, esp_app_get_description()->project_name) != 0) {
        *err_msg = "Not a Warbler32 firmware image";
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "image: %s %s (%s) -> %s", desc->project_name,
             desc->version, desc->date, s_eng.update->label);

    if (esp_ota_begin(s_eng.update, s_eng.total, &s_eng.handle) != ESP_OK) {
        *err_msg = "Failed to start OTA (flash erase)";
        return ESP_FAIL;
    }
    s_eng.flashing = true;

    if (esp_ota_write(s_eng.handle, s_eng.prefix, s_eng.prefix_have) != ESP_OK) {
        *err_msg = "Flash write failed";
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ota_engine_feed(const uint8_t *data, size_t len, const char **err_msg)
{
    if (!atomic_load(&s_eng.busy)) {
        *err_msg = "No update in progress";
        return ESP_ERR_INVALID_STATE;
    }
    if (s_eng.written + len > s_eng.total) {
        *err_msg = "Image larger than announced size";
        ota_engine_abort();
        return ESP_ERR_INVALID_SIZE;
    }
    s_eng.written += len;

    // Park bytes in the prefix buffer until the headers can be validated
    if (!s_eng.flashing) {
        size_t take = OTA_DESC_END - s_eng.prefix_have;
        if (take > len) take = len;
        memcpy(s_eng.prefix + s_eng.prefix_have, data, take);
        s_eng.prefix_have += take;
        data += take;
        len  -= take;

        if (s_eng.prefix_have < OTA_DESC_END) return ESP_OK;

        esp_err_t err = engine_start_flash(err_msg);
        if (err != ESP_OK) {
            ota_engine_abort();
            return err;
        }
    }

    if (len > 0 && esp_ota_write(s_eng.handle, data, len) != ESP_OK) {
        *err_msg = "Flash write failed";
        ota_engine_abort();
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ota_engine_finish(const char **err_msg)
{
    if (!atomic_load(&s_eng.busy) || !s_eng.flashing) {
        *err_msg = "Upload interrupted";
        ota_engine_abort();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_eng.written != s_eng.total) {
        *err_msg = "Image shorter than announced size";
        ota_engine_abort();
        return ESP_FAIL;
    }

    s_eng.flashing = false;   // esp_ota_end consumes the handle either way
    esp_err_t err = esp_ota_end(s_eng.handle);   // full checksum + SHA check
    if (err != ESP_OK) {
        *err_msg = (err == ESP_ERR_OTA_VALIDATE_FAILED)
                       ? "Image verification failed (corrupt download?)"
                       : "Failed to finalize OTA";
        atomic_store(&s_eng.busy, false);
        return err;
    }
    if (esp_ota_set_boot_partition(s_eng.update) != ESP_OK) {
        *err_msg = "Failed to set boot partition";
        atomic_store(&s_eng.busy, false);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA complete, next boot from %s", s_eng.update->label);
    atomic_store(&s_eng.busy, false);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GitHub release check
// ---------------------------------------------------------------------------

#define GH_API_URL   "https://api.github.com/repos/" OTA_GITHUB_REPO "/releases/latest"
#define GH_JSON_MAX  (64 * 1024)

// Cached by ota_github_check() for ota_github_install()
static char   s_asset_url[256];
static size_t s_asset_size;
static char   s_latest_tag[32];
static bool   s_update_available;

esp_err_t ota_github_check(ota_check_result_t *out, const char **err_msg)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->current, esp_app_get_description()->version, sizeof(out->current));
    s_update_available = false;

    esp_http_client_config_t cfg = {
        .url                = GH_API_URL,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .timeout_ms         = 15000,
        .user_agent         = "warbler32",   // GitHub API requires a User-Agent
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) { *err_msg = "HTTP client init failed"; return ESP_FAIL; }

    char *body = NULL;
    esp_err_t ret = ESP_FAIL;

    // The mesh WiFi occasionally drops the first TLS connect; those failures
    // are quick, so a couple of retries are cheap even in the httpd task.
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < OTA_GH_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "GitHub connect failed, retrying (%d/%d)",
                     attempt + 1, OTA_GH_ATTEMPTS);
            esp_http_client_close(client);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        err = esp_http_client_open(client, 0);
        if (err == ESP_OK) break;
    }
    if (err != ESP_OK) {
        *err_msg = "Could not reach GitHub (no internet?)";
        goto out;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status == 404) {
        *err_msg = "No releases published yet";
        goto out;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "GitHub API HTTP %d", status);
        *err_msg = "GitHub API error";
        goto out;
    }

    body = malloc(GH_JSON_MAX);
    if (body == NULL) { *err_msg = "Out of memory"; goto out; }

    int total = 0, n;
    while ((n = esp_http_client_read(client, body + total,
                                     GH_JSON_MAX - 1 - total)) > 0) {
        total += n;
        if (total >= GH_JSON_MAX - 1) { break; }
    }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) { *err_msg = "Bad response from GitHub"; goto out; }

    const cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
    if (!cJSON_IsString(tag)) {
        cJSON_Delete(root);
        *err_msg = "Bad response from GitHub";
        goto out;
    }
    strlcpy(s_latest_tag, tag->valuestring, sizeof(s_latest_tag));
    strlcpy(out->latest, s_latest_tag, sizeof(out->latest));

    char want[48];
    snprintf(want, sizeof(want), "warbler32-%s.bin", ota_board_variant());

    s_asset_url[0] = '\0';
    s_asset_size = 0;
    const cJSON *asset;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItem(root, "assets")) {
        const cJSON *name = cJSON_GetObjectItem(asset, "name");
        if (cJSON_IsString(name) && strcmp(name->valuestring, want) == 0) {
            const cJSON *url  = cJSON_GetObjectItem(asset, "browser_download_url");
            const cJSON *size = cJSON_GetObjectItem(asset, "size");
            if (cJSON_IsString(url) && cJSON_IsNumber(size)) {
                strlcpy(s_asset_url, url->valuestring, sizeof(s_asset_url));
                s_asset_size = (size_t)size->valuedouble;
            }
            break;
        }
    }
    cJSON_Delete(root);

    if (s_asset_url[0] == '\0') {
        ESP_LOGW(TAG, "release %s has no %s", s_latest_tag, want);
        *err_msg = "Latest release has no build for this board";
        goto out;
    }

    out->size = s_asset_size;
    out->available = strcmp(s_latest_tag, out->current) != 0;
    s_update_available = out->available;
    ESP_LOGI(TAG, "latest %s, running %s, asset %s (%u bytes) -> %s",
             out->latest, out->current, want, (unsigned)s_asset_size,
             out->available ? "update available" : "up to date");
    ret = ESP_OK;

out:
    free(body);
    esp_http_client_cleanup(client);
    return ret;
}

// ---------------------------------------------------------------------------
// GitHub release install (background task)
// ---------------------------------------------------------------------------

static struct {
    const char * volatile state;
    volatile int pct;
    char msg[96];
} s_prog = { .state = "idle" };

void ota_github_progress(ota_progress_t *out)
{
    out->state = s_prog.state;
    out->pct   = s_prog.pct;
    strlcpy(out->msg, s_prog.msg, sizeof(out->msg));
}

static void gh_fail(const char *msg)
{
    ESP_LOGE(TAG, "install failed: %s", msg);
    strlcpy(s_prog.msg, msg, sizeof(s_prog.msg));
    s_prog.state = "error";
}

// One complete download + flash attempt. Returns ESP_OK on success,
// ESP_ERR_INVALID_STATE for permanent failures (bad image, engine busy),
// and ESP_FAIL for transient network failures worth retrying.
static esp_err_t gh_attempt(uint8_t *buf, const char **emsg)
{
    esp_http_client_config_t cfg = {
        .url               = s_asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
        .user_agent        = "warbler32",
        // The redirect to GitHub's CDN uses a signed URL (~1 KB) with
        // S3-style headers — both the RX header buffer and the TX buffer
        // (which must hold the redirected request line) far exceed the
        // 512-byte defaults.
        .buffer_size       = 4096,
        .buffer_size_tx    = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        *emsg = "Out of memory";
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_FAIL;        // default: transient, retry
    bool engine_active = false;

    // browser_download_url redirects to GitHub's CDN; follow manually since
    // the streaming open/fetch_headers path doesn't auto-redirect.
    int status = 0;
    for (int hop = 0; hop < 5; hop++) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            *emsg = "Could not reach GitHub (no internet?)";
            goto out;
        }
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        break;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "asset download HTTP %d", status);
        *emsg = "Download failed (GitHub error)";
        goto out;
    }

    int64_t clen = esp_http_client_get_content_length(client);
    size_t total = (clen > 0) ? (size_t)clen : s_asset_size;

    if (ota_engine_begin(total, emsg) != ESP_OK) {
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }
    engine_active = true;

    ESP_LOGI(TAG, "downloading %s (%u bytes)", s_latest_tag, (unsigned)total);
    size_t received = 0;
    int n;
    while ((n = esp_http_client_read(client, (char *)buf, 4096)) > 0) {
        if (ota_engine_feed(buf, (size_t)n, emsg) != ESP_OK) {
            engine_active = false;   // feed aborts the engine on error
            ret = ESP_ERR_INVALID_STATE;
            goto out;
        }
        received += (size_t)n;
        s_prog.pct = (int)(received * 100 / total);
    }
    if (n < 0 || !esp_http_client_is_complete_data_received(client)) {
        *emsg = "Download interrupted";
        goto out;
    }

    s_prog.state = "verifying";
    engine_active = false;           // finish consumes the engine either way
    if (ota_engine_finish(emsg) != ESP_OK) {
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }
    ret = ESP_OK;

out:
    if (engine_active) ota_engine_abort();
    esp_http_client_cleanup(client);
    return ret;
}

static void gh_install_task(void *arg)
{
    uint8_t *buf = malloc(4096);
    const char *emsg = "Download failed";

    if (buf == NULL) {
        gh_fail("Out of memory");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= OTA_GH_ATTEMPTS; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "download failed (%s), retrying (%d/%d)",
                     emsg, attempt, OTA_GH_ATTEMPTS);
            s_prog.state = "downloading";
            s_prog.pct   = 0;
            vTaskDelay(pdMS_TO_TICKS(OTA_GH_RETRY_DELAY_MS));
        }
        err = gh_attempt(buf, &emsg);
        if (err != ESP_FAIL) break;  // success or permanent failure
    }
    free(buf);

    if (err != ESP_OK) {
        gh_fail(emsg);
        vTaskDelete(NULL);
        return;
    }

    s_prog.state = "rebooting";
    ESP_LOGI(TAG, "update installed, rebooting");
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

esp_err_t ota_github_install(const char **err_msg)
{
    if (strcmp(s_prog.state, "downloading") == 0 ||
        strcmp(s_prog.state, "verifying") == 0 ||
        strcmp(s_prog.state, "rebooting") == 0) {
        *err_msg = "Install already in progress";
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_update_available || s_asset_url[0] == '\0') {
        *err_msg = "No update found — run a check first";
        return ESP_ERR_INVALID_STATE;
    }

    s_prog.pct = 0;
    s_prog.msg[0] = '\0';
    s_prog.state = "downloading";
    if (xTaskCreate(gh_install_task, "ota_gh", 12288, NULL, 5, NULL) != pdPASS) {
        s_prog.state = "error";
        *err_msg = "Out of memory";
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
