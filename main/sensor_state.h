// Cross-component sensor snapshot facade.
//
// The board class (esp_box3_board.cc) publishes the latest sensor readings
// to this single global on its sensor task; the HUD display reads from it
// on its LVGL timer. Keeps display code free of board-specific includes.
//
// All temperature accessors return Fahrenheit. Conversion happens in
// Publish() so consumers never have to think about units.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool  dock_present;       // sensor dock physically attached
    bool  radar_presence;     // radar sees motion within last 2 min
    bool  humiture_valid;     // temp/humidity reading is fresh
    float temperature_f;      // ambient temp, Fahrenheit
    float humidity_percent;   // 0-100
    bool  imu_valid;
    float accel_x;            // g
    float accel_y;
    float accel_z;
    float imu_temp_f;         // chip die temp, Fahrenheit (kept for parity)
} sensor_snapshot_t;

// Atomic-ish snapshot (single producer, multiple consumers).
sensor_snapshot_t sensor_state_get(void);

// Publish a fresh snapshot. Call from a single producer task only.
// Pass temperature in Celsius; the facade converts to Fahrenheit before storing.
void sensor_state_publish_celsius(
    bool dock_present, bool radar_presence, bool humiture_valid,
    float temperature_c, float humidity_percent,
    bool imu_valid,
    float accel_x, float accel_y, float accel_z,
    float imu_temp_c
);

#ifdef __cplusplus
}
#endif
