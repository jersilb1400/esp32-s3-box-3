#include "weather_state.h"
#include "sdkconfig.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

#include "cJSON.h"

namespace {

constexpr const char* TAG = "weather";

// CONFIG_* are provided by Kconfig.projbuild — see the new entries added there.
#ifndef CONFIG_WEATHER_API_KEY
#define CONFIG_WEATHER_API_KEY ""
#endif
#ifndef CONFIG_WEATHER_LOCATION_ZIP
#define CONFIG_WEATHER_LOCATION_ZIP "75494,us"
#endif
#ifndef CONFIG_WEATHER_POLL_INTERVAL_MIN
#define CONFIG_WEATHER_POLL_INTERVAL_MIN 15
#endif

std::atomic<uint32_t> g_version{0};
weather_snapshot_t    g_snapshot{};
std::atomic<bool>     g_started{false};

void publish(const weather_snapshot_t& s) {
    g_version.fetch_add(1, std::memory_order_acq_rel);
    std::memcpy(&g_snapshot, &s, sizeof(s));
    g_version.fetch_add(1, std::memory_order_release);
}

// Tiny string buffer that the HTTP event handler appends into.
struct ResponseBuffer {
    static constexpr size_t kMax = 4096;
    char  data[kMax + 1] = {0};
    size_t len = 0;
    void append(const char* p, size_t n) {
        if (len + n > kMax) n = kMax - len;
        std::memcpy(data + len, p, n);
        len += n;
        data[len] = '\0';
    }
    void reset() { len = 0; data[0] = '\0'; }
};

esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf && evt->data && evt->data_len > 0) {
        buf->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

bool fetch_once(weather_snapshot_t& out) {
    const char* api_key = CONFIG_WEATHER_API_KEY;
    if (!api_key || api_key[0] == '\0') {
        return false;
    }
    char url[256];
    std::snprintf(url, sizeof(url),
        "https://api.openweathermap.org/data/2.5/weather?zip=%s&appid=%s&units=imperial",
        CONFIG_WEATHER_LOCATION_ZIP, api_key);

    ResponseBuffer buf;
    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.event_handler = http_event_handler;
    cfg.user_data = &buf;
    cfg.crt_bundle_attach = nullptr;  // OWM uses Let's Encrypt; default bundle works
    cfg.cert_pem = nullptr;
    cfg.skip_cert_common_name_check = false;
    // Use IDF's bundled root CA store (esp_crt_bundle is on by default in this project).
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "fetch failed: err=%d status=%d", err, status);
        return false;
    }

    cJSON* root = cJSON_Parse(buf.data);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return false;
    }
    bool ok = false;
    do {
        cJSON* main = cJSON_GetObjectItem(root, "main");
        cJSON* weather_arr = cJSON_GetObjectItem(root, "weather");
        cJSON* name = cJSON_GetObjectItem(root, "name");
        if (!main || !cJSON_IsArray(weather_arr)) break;
        cJSON* w0 = cJSON_GetArrayItem(weather_arr, 0);
        if (!w0) break;

        out.valid = true;
        cJSON* t = cJSON_GetObjectItem(main, "temp");
        cJSON* fl = cJSON_GetObjectItem(main, "feels_like");
        cJSON* h = cJSON_GetObjectItem(main, "humidity");
        out.temp_f       = cJSON_IsNumber(t)  ? (float)t->valuedouble  : 0.0f;
        out.feels_like_f = cJSON_IsNumber(fl) ? (float)fl->valuedouble : out.temp_f;
        out.humidity_percent = cJSON_IsNumber(h) ? (int)h->valuedouble : 0;

        cJSON* desc = cJSON_GetObjectItem(w0, "description");
        cJSON* icon = cJSON_GetObjectItem(w0, "icon");
        std::strncpy(out.condition,
            cJSON_IsString(desc) ? desc->valuestring : "",
            sizeof(out.condition) - 1);
        std::strncpy(out.icon,
            cJSON_IsString(icon) ? icon->valuestring : "",
            sizeof(out.icon) - 1);

        std::strncpy(out.location,
            cJSON_IsString(name) ? name->valuestring : "",
            sizeof(out.location) - 1);

        out.fetched_unix_ms = (long)(esp_timer_get_time() / 1000);
        ok = true;
    } while (false);
    cJSON_Delete(root);
    return ok;
}

void poller_task(void*) {
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_WEATHER_POLL_INTERVAL_MIN * 60 * 1000);
    // Initial delay to let WiFi come up
    vTaskDelay(pdMS_TO_TICKS(15000));
    for (;;) {
        weather_snapshot_t s{};
        if (fetch_once(s)) {
            publish(s);
            ESP_LOGI(TAG, "weather: %.1fF %s @ %s",
                (double)s.temp_f, s.condition, s.location);
        }
        vTaskDelay(interval);
    }
}

}  // namespace

extern "C" weather_snapshot_t weather_state_get(void) {
    weather_snapshot_t out{};
    for (;;) {
        uint32_t v_before = g_version.load(std::memory_order_acquire);
        if (v_before & 1u) continue;
        std::memcpy(&out, &g_snapshot, sizeof(out));
        uint32_t v_after = g_version.load(std::memory_order_acquire);
        if (v_after == v_before) return out;
    }
}

extern "C" void weather_state_start(void) {
    if (g_started.exchange(true)) return;
    if (!CONFIG_WEATHER_API_KEY[0]) {
        ESP_LOGI(TAG, "no API key configured; weather disabled");
        return;
    }
    xTaskCreatePinnedToCore(poller_task, "weather", 10 * 1024, nullptr, 3, nullptr, 0);
}
