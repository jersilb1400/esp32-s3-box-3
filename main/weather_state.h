// Weather snapshot facade.
//
// A FreeRTOS task polls OpenWeatherMap every 15 minutes (configurable via
// CONFIG_WEATHER_POLL_INTERVAL_MIN) and publishes the latest snapshot to
// this single global. The HUD display reads it on its LVGL timer.
//
// Returns Fahrenheit (units=imperial in the OWM request).
//
// Set CONFIG_WEATHER_API_KEY in sdkconfig to enable. Without a key, the
// task is a no-op and Get() returns valid=false.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One day of forecast (daily summary derived from the 5-day/3-hour endpoint).
typedef struct {
    bool   valid;             // forecast field populated
    char   day[8];            // "Mon", "Tue", ... ("" if !valid)
    float  high_f;            // daily high, Fahrenheit
    float  low_f;             // daily low, Fahrenheit
    char   condition[24];     // e.g. "clear sky", "rain"
} weather_forecast_day_t;

typedef struct {
    bool   valid;             // last current-conditions fetch succeeded
    float  temp_f;            // current temp, Fahrenheit
    float  feels_like_f;
    int    humidity_percent;
    char   condition[32];     // e.g. "clear sky", "few clouds"
    char   icon[6];           // OWM icon code, e.g. "01d"
    char   location[32];      // e.g. "Winnsboro"
    long   fetched_unix_ms;   // when current conditions were retrieved

    // 3-day forecast (today is excluded; index 0 is tomorrow).
    weather_forecast_day_t forecast[3];
    bool   forecast_valid;    // any of the three days were filled in
} weather_snapshot_t;

weather_snapshot_t weather_state_get(void);

// Start the background poller. Idempotent. No-op if API key is not set.
void weather_state_start(void);

#ifdef __cplusplus
}
#endif
