#pragma once

#include <lvgl.h>
#include <cstdint>

/** Full-screen soft point cloud HUD (floating dots only, Jarvis-inspired). */
class JarvisPointCloud {
public:
    JarvisPointCloud();
    JarvisPointCloud(const JarvisPointCloud&) = delete;
    JarvisPointCloud& operator=(const JarvisPointCloud&) = delete;
    ~JarvisPointCloud();

    bool Create(lv_obj_t* parent, lv_coord_t width, lv_coord_t height);
    void Destroy();
    void SetPaused(bool paused) { paused_ = paused; }

    lv_obj_t* Canvas() const { return canvas_; }

private:
    static void TimerCb(lv_timer_t* timer);
    void Tick();
    /** Frontal head mesh seeds + outer aura targets (speech morph destinations). */
    void InitMorphTargets();
    void PlotPixel(int x, int y, lv_color_t color);
    void DrawFluffyDot(int cx, int cy,
                       lv_color_t core,
                       lv_color_t mid_halo,
                       lv_color_t outer_halo,
                       float speech_fluff);
    /** Tight footprint for dense mesh pts (keeps ≥25 FPS with many surface samples). */
    void DrawCompactShadedDot(int cx, int cy,
                              lv_color_t bright_core,
                              lv_color_t bright_mid,
                              lv_color_t rim,
                              uint8_t lambert_u8,
                              float speech_fluff);

    lv_obj_t* canvas_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    void* pixel_buffer_ = nullptr;
    lv_coord_t width_ = 0;
    lv_coord_t height_ = 0;
    float t_ = 0.f;
    bool paused_ = false;
    float speech_drive_smoothed_ = 0.f;
    /** 0 = free cloud, 1 = silhouette; eased from playback speech level. */
    float morph_smoothed_ = 0.f;

    /**
     * Draw budget tuning (ESP-BOX-ish 320×240 canvas): ~168 mesh pts (compact dots) +
     * ~48 aura particles (full fluff) targets ≥25 FPS with full canvas clear each tick.
     */
    static constexpr int kHeadMeshTarget = 168;
    /** Explicit frontal egg outline samples — dominates visual “head shape” vs interior fill. */
    static constexpr int kFaceOutlinePoints = 84;
    static constexpr int kHeadInteriorPoints = kHeadMeshTarget - kFaceOutlinePoints;
    static_assert(kHeadInteriorPoints > 0, "head interior bucket");
    static constexpr int kAuraParticles = 48;
    static constexpr int kParticles = kHeadMeshTarget + kAuraParticles;

    struct Particle {
        float nx;
        float ny;
        float phase;
    };

    Particle particles_[kParticles];
    /** Normalized XY morph targets (−1…1-ish); mesh + aura slots. */
    float head_tx_[kParticles];
    float head_ty_[kParticles];
    /** Lambert term (0–255) for embedded generic male head mesh samples. */
    uint8_t mesh_shade_[kHeadMeshTarget];

    int scr_x_[kParticles];
    int scr_y_[kParticles];
};
