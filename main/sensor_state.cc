#include "sensor_state.h"
#include <atomic>
#include <cstring>

namespace {

// Lock-free single-producer / multi-consumer pattern using a versioned snapshot.
// Producer increments version twice per write (start odd, end even). Consumer
// retries if it sees a torn read. For this use case (small struct, low rate)
// a plain spinlock would also work; this is just zero-cost.
std::atomic<uint32_t> g_version{0};
sensor_snapshot_t     g_snapshot{};

inline float c_to_f(float c) { return c * 9.0f / 5.0f + 32.0f; }

}  // namespace

extern "C" sensor_snapshot_t sensor_state_get(void) {
    sensor_snapshot_t out{};
    for (;;) {
        uint32_t v_before = g_version.load(std::memory_order_acquire);
        if (v_before & 1u) {
            // Write in progress, retry.
            continue;
        }
        std::memcpy(&out, &g_snapshot, sizeof(out));
        uint32_t v_after = g_version.load(std::memory_order_acquire);
        if (v_after == v_before) {
            return out;
        }
    }
}

extern "C" void sensor_state_publish_celsius(
    bool dock_present, bool radar_presence, bool humiture_valid,
    float temperature_c, float humidity_percent,
    bool imu_valid,
    float accel_x, float accel_y, float accel_z,
    float imu_temp_c
) {
    uint32_t v = g_version.fetch_add(1, std::memory_order_acq_rel) + 1;
    (void)v;  // odd now — write in progress

    g_snapshot.dock_present     = dock_present;
    g_snapshot.radar_presence   = radar_presence;
    g_snapshot.humiture_valid   = humiture_valid;
    g_snapshot.temperature_f    = humiture_valid ? c_to_f(temperature_c) : 0.0f;
    g_snapshot.humidity_percent = humiture_valid ? humidity_percent : 0.0f;
    g_snapshot.imu_valid        = imu_valid;
    g_snapshot.accel_x          = accel_x;
    g_snapshot.accel_y          = accel_y;
    g_snapshot.accel_z          = accel_z;
    g_snapshot.imu_temp_f       = imu_valid ? c_to_f(imu_temp_c) : 0.0f;

    g_version.fetch_add(1, std::memory_order_release);  // even — write complete
}
