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

typedef struct {
    bool   valid;             // last fetch succeeded
    float  temp_f;            // current temp, Fahrenheit
    float  feels_like_f;
    int    humidity_percent;
    char   condition[32];     // e.g. "clear sky", "few clouds"
    char   icon[6];           // OWM icon code, e.g. "01d"
    char   location[32];      // e.g. "Winnsboro"
    long   fetched_unix_ms;   // when this reading was retrieved
} weather_snapshot_t;

weather_snapshot_t weather_state_get(void);

// Start the background poller. Idempotent. No-op if API key is not set.
void weather_state_start(void);

#ifdef __cplusplus
}
#endif
