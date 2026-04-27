#include "weather_state.h"
#include "sdkconfig.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

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

// Tiny string buffer that the HTTP event handler appends into. The forecast
// endpoint can return ~30KB so the forecast variant uses a larger pool.
template <size_t MAX>
struct ResponseBufferT {
    static constexpr size_t kMax = MAX;
    char  data[MAX + 1] = {0};
    size_t len = 0;
    void append(const char* p, size_t n) {
        if (len + n > MAX) n = MAX - len;
        std::memcpy(data + len, p, n);
        len += n;
        data[len] = '\0';
    }
    void reset() { len = 0; data[0] = '\0'; }
};
using ResponseBuffer = ResponseBufferT<4096>;
using ForecastResponseBuffer = ResponseBufferT<32768>;

esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf && evt->data && evt->data_len > 0) {
        buf->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

esp_err_t forecast_event_handler(esp_http_client_event_t* evt) {
    auto* buf = static_cast<ForecastResponseBuffer*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf && evt->data && evt->data_len > 0) {
        buf->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

// Map a unix timestamp to a 3-letter local weekday name, e.g. "Mon".
void weekday_short(time_t t, char out[8]) {
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    static const char* kNames[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    int wday = tm_local.tm_wday;
    if (wday < 0 || wday > 6) wday = 0;
    std::strncpy(out, kNames[wday], 7);
    out[7] = '\0';
}

bool fetch_forecast(weather_forecast_day_t out[3]) {
    const char* api_key = CONFIG_WEATHER_API_KEY;
    if (!api_key || api_key[0] == '\0') return false;

    char url[256];
    // 5-day / 3-hour forecast. Free tier; same key as current weather.
    // We'll bucket entries by local day and pick the 12:00-local entry as
    // the day's representative summary.
    std::snprintf(url, sizeof(url),
        "https://api.openweathermap.org/data/2.5/forecast?zip=%s&appid=%s&units=imperial",
        CONFIG_WEATHER_LOCATION_ZIP, api_key);

    // Forecast payload is large — heap-allocate to avoid a 32KB stack hit
    // (this task only has 10KB stack).
    auto* buf = new ForecastResponseBuffer();
    if (!buf) return false;

    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 15000;
    cfg.event_handler = forecast_event_handler;
    cfg.user_data = buf;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { delete buf; return false; }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "forecast fetch failed: err=%d status=%d", err, status);
        delete buf;
        return false;
    }

    cJSON* root = cJSON_Parse(buf->data);
    delete buf;
    if (!root) {
        ESP_LOGW(TAG, "forecast JSON parse failed");
        return false;
    }

    // We want the 3 calendar days FOLLOWING today, in local time. Walk the
    // 40 entries and reduce to per-day high/low + the entry closest to 12pm
    // for the condition string.
    struct DayBucket {
        bool  filled;
        long  date_key;   // YYYYMMDD in local tz
        char  day[8];
        float hi;
        float lo;
        char  condition[24];
        int   best_diff_to_noon;  // for picking representative condition
    };
    DayBucket days[5] = {};  // up to 5 distinct local days observed
    int day_count = 0;

    time_t now;
    time(&now);
    struct tm now_local;
    localtime_r(&now, &now_local);
    long today_key = (now_local.tm_year + 1900) * 10000
                   + (now_local.tm_mon + 1) * 100
                   + now_local.tm_mday;

    cJSON* list = cJSON_GetObjectItem(root, "list");
    if (cJSON_IsArray(list)) {
        int n = cJSON_GetArraySize(list);
        for (int i = 0; i < n; ++i) {
            cJSON* item = cJSON_GetArrayItem(list, i);
            if (!item) continue;
            cJSON* dt = cJSON_GetObjectItem(item, "dt");
            cJSON* main_o = cJSON_GetObjectItem(item, "main");
            cJSON* w_arr = cJSON_GetObjectItem(item, "weather");
            if (!cJSON_IsNumber(dt) || !main_o || !cJSON_IsArray(w_arr)) continue;

            time_t ts = (time_t)dt->valuedouble;
            struct tm ts_local;
            localtime_r(&ts, &ts_local);
            long key = (ts_local.tm_year + 1900) * 10000
                     + (ts_local.tm_mon + 1) * 100
                     + ts_local.tm_mday;
            if (key <= today_key) continue;  // skip today

            // Find or create bucket for this date.
            int idx = -1;
            for (int b = 0; b < day_count; ++b) {
                if (days[b].date_key == key) { idx = b; break; }
            }
            if (idx < 0) {
                if (day_count >= 5) continue;
                idx = day_count++;
                days[idx].filled = true;
                days[idx].date_key = key;
                weekday_short(ts, days[idx].day);
                days[idx].hi = -1000.0f;
                days[idx].lo =  1000.0f;
                days[idx].best_diff_to_noon = 99;
                days[idx].condition[0] = '\0';
            }

            cJSON* tmax = cJSON_GetObjectItem(main_o, "temp_max");
            cJSON* tmin = cJSON_GetObjectItem(main_o, "temp_min");
            float hi = cJSON_IsNumber(tmax) ? (float)tmax->valuedouble : 0.0f;
            float lo = cJSON_IsNumber(tmin) ? (float)tmin->valuedouble : 0.0f;
            if (hi > days[idx].hi) days[idx].hi = hi;
            if (lo < days[idx].lo) days[idx].lo = lo;

            int diff = std::abs(ts_local.tm_hour - 12);
            if (diff < days[idx].best_diff_to_noon) {
                days[idx].best_diff_to_noon = diff;
                cJSON* w0 = cJSON_GetArrayItem(w_arr, 0);
                if (w0) {
                    cJSON* desc = cJSON_GetObjectItem(w0, "main");  // "Clear","Rain", short word
                    if (cJSON_IsString(desc)) {
                        std::strncpy(days[idx].condition, desc->valuestring,
                                     sizeof(days[idx].condition) - 1);
                        days[idx].condition[sizeof(days[idx].condition) - 1] = '\0';
                    }
                }
            }
        }
    }
    cJSON_Delete(root);

    // Take first 3 buckets (already in chronological order).
    for (int i = 0; i < 3; ++i) {
        if (i < day_count && days[i].filled) {
            out[i].valid = true;
            std::strncpy(out[i].day, days[i].day, sizeof(out[i].day) - 1);
            out[i].day[sizeof(out[i].day) - 1] = '\0';
            out[i].high_f = days[i].hi;
            out[i].low_f  = days[i].lo;
            std::strncpy(out[i].condition, days[i].condition, sizeof(out[i].condition) - 1);
            out[i].condition[sizeof(out[i].condition) - 1] = '\0';
        } else {
            out[i].valid = false;
            out[i].day[0] = '\0';
        }
    }
    return true;
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
            // Try to enrich with forecast — non-fatal if it fails.
            weather_forecast_day_t fc[3] = {};
            if (fetch_forecast(fc)) {
                s.forecast_valid = false;
                for (int i = 0; i < 3; ++i) {
                    s.forecast[i] = fc[i];
                    if (fc[i].valid) s.forecast_valid = true;
                }
                if (s.forecast_valid) {
                    ESP_LOGI(TAG, "forecast: %s %.0f/%.0f %s, %s %.0f/%.0f %s, %s %.0f/%.0f %s",
                        fc[0].day, (double)fc[0].high_f, (double)fc[0].low_f, fc[0].condition,
                        fc[1].day, (double)fc[1].high_f, (double)fc[1].low_f, fc[1].condition,
                        fc[2].day, (double)fc[2].high_f, (double)fc[2].low_f, fc[2].condition);
                }
            }
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
