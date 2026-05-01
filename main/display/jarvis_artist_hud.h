#pragma once

#include <lvgl.h>

/**
 * Sprite-based Jarvis HUD: centered RGB565 face overlay driven by playback RMS.
 * Replaces procedural JarvisPointCloud for an artist-authored silhouette/mask workflow.
 */
class JarvisArtistHud {
public:
    JarvisArtistHud();
    JarvisArtistHud(const JarvisArtistHud&) = delete;
    JarvisArtistHud& operator=(const JarvisArtistHud&) = delete;
    ~JarvisArtistHud();

    bool Create(lv_obj_t* parent, lv_coord_t width, lv_coord_t height);
    void Destroy();
    void SetPaused(bool paused);

    lv_obj_t* Overlay() const { return root_; }

private:
    static void TimerCb(lv_timer_t* timer);
    void Tick();

    lv_obj_t* root_ = nullptr;
    lv_obj_t* face_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    lv_img_dsc_t face_dsc_{};
    bool face_dsc_ready_ = false;

    bool paused_ = false;

    float speech_smoothed_ = 0.f;

    void EnsureFaceDescriptor();
};
