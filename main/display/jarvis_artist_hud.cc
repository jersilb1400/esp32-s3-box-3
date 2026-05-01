#include "jarvis_artist_hud.h"
#include "jarvis_speech_meter.h"

#include <cmath>
#include <cstring>

#include <esp_log.h>

#define TAG "JarvisArtist"

extern "C" {
extern const unsigned kJarvisArtistFace_w;
extern const unsigned kJarvisArtistFace_h;
extern const unsigned kJarvisArtistFace_stride;
extern const unsigned kJarvisArtistFace_data_size;
extern const uint8_t kJarvisArtistFace_pixels[];
}

namespace {

inline float Clamp01(float x) {
    if (x < 0.f) {
        return 0.f;
    }
    if (x > 1.f) {
        return 1.f;
    }
    return x;
}

inline float Smoothstep(float edge0, float edge1, float x) {
    if (edge1 <= edge0) {
        return x >= edge1 ? 1.f : 0.f;
    }
    float t = (x - edge0) / (edge1 - edge0);
    t = Clamp01(t);
    return t * t * (3.f - 2.f * t);
}

}  // namespace

JarvisArtistHud::JarvisArtistHud() = default;

JarvisArtistHud::~JarvisArtistHud() {
    Destroy();
}

void JarvisArtistHud::EnsureFaceDescriptor() {
    if (face_dsc_ready_) {
        return;
    }
    std::memset(&face_dsc_, 0, sizeof(face_dsc_));
    face_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    face_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    face_dsc_.header.w = static_cast<uint32_t>(kJarvisArtistFace_w);
    face_dsc_.header.h = static_cast<uint32_t>(kJarvisArtistFace_h);
    face_dsc_.header.stride = static_cast<uint32_t>(kJarvisArtistFace_stride);
    face_dsc_.data = kJarvisArtistFace_pixels;
    face_dsc_.data_size = kJarvisArtistFace_data_size;
    face_dsc_ready_ = true;
}

void JarvisArtistHud::Destroy() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (root_ != nullptr) {
        lv_obj_del(root_);
        root_ = nullptr;
        face_ = nullptr;
    }
}

bool JarvisArtistHud::Create(lv_obj_t* parent, lv_coord_t width, lv_coord_t height) {
    Destroy();
    if (parent == nullptr) {
        return false;
    }

    EnsureFaceDescriptor();
    if (face_dsc_.data == nullptr || face_dsc_.data_size == 0 ||
        kJarvisArtistFace_stride < (unsigned)face_dsc_.header.w * 2u) {
        ESP_LOGE(TAG, "Face asset invalid");
        return false;
    }

    root_ = lv_obj_create(parent);
    if (root_ == nullptr) {
        ESP_LOGE(TAG, "root create failed");
        return false;
    }
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, width, height);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root_, 0, 0);

    face_ = lv_image_create(root_);
    if (face_ == nullptr) {
        ESP_LOGE(TAG, "lv_image_create failed");
        Destroy();
        return false;
    }
    lv_image_set_src(face_, &face_dsc_);
    lv_image_set_inner_align(face_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(face_);

    /* Match ~25 FPS budget used by former point-cloud HUD */
    constexpr uint32_t period_ms = 40;
    timer_ = lv_timer_create(TimerCb, period_ms, this);
    if (timer_ == nullptr) {
        ESP_LOGE(TAG, "lv_timer_create failed");
        Destroy();
        return false;
    }
    lv_timer_set_repeat_count(timer_, -1);

    SetPaused(false);
    return true;
}

void JarvisArtistHud::SetPaused(bool paused) {
    paused_ = paused;
    if (root_ != nullptr) {
        if (paused) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void JarvisArtistHud::TimerCb(lv_timer_t* timer) {
    auto* self = static_cast<JarvisArtistHud*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->Tick();
    }
}

void JarvisArtistHud::Tick() {
    if (paused_ || face_ == nullptr) {
        return;
    }

    float target = JarvisSpeechMeterTargetLevel();
    speech_smoothed_ += (target - speech_smoothed_) * 0.28f;
    float s = Smoothstep(0.02f, 0.98f, speech_smoothed_);

    int32_t opa = 38 + static_cast<int32_t>(s * 217.f);
    if (opa < 0) {
        opa = 0;
    } else if (opa > 255) {
        opa = 255;
    }
    lv_obj_set_style_opa(face_, static_cast<lv_opa_t>(opa), LV_PART_MAIN);

    int32_t scale = 256 + static_cast<int32_t>(s * 80.f);
    if (scale < 256) {
        scale = 256;
    } else if (scale > 430) {
        scale = 430;
    }
    lv_image_set_scale(face_, scale);
}
