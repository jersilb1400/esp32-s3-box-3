#include "jarvis_point_cloud.h"
#include "jarvis_speech_meter.h"

#include <cinttypes>
#include <cmath>
#include <cstring>
#include <algorithm>

#include <esp_heap_caps.h>
#include <esp_log.h>

#define TAG "JarvisPointCloud"

namespace {

struct HeadMeshRawPt {
    float x;
    float y;
    float shade;
};

/** Matches scripts/gen_jarvis_head_mesh.py; uses stack arrays only (no STL alloc) for robust boot on S3/Core boards. */
int BuildGenericMaleHeadMesh(float* out_x, float* out_y, uint8_t* out_shade, int k_target) {
    constexpr int kRawMax = 420;
    constexpr int kChosenMax = 168;

    if (k_target <= 0) {
        return 0;
    }
    int k_pick = k_target;
    if (k_pick > kChosenMax) {
        k_pick = kChosenMax;
    }

    static HeadMeshRawPt s_scratch_raw[kRawMax];
    static bool s_scratch_used[kRawMax];
    static HeadMeshRawPt s_scratch_chosen[kChosenMax];

    HeadMeshRawPt* raw = s_scratch_raw;
    bool* used = s_scratch_used;
    HeadMeshRawPt* chosen = s_scratch_chosen;
    int n_raw = 0;

    const int n_sphere = 520;
    const float golden = (1.f + std::sqrt(5.f)) * 0.5f;
    const float two_pi = 6.28318530718f;

    for (int i = 0; i < n_sphere; ++i) {
        float t = ((float)i + 0.5f) / (float)n_sphere;
        float y = 1.f - 2.f * t;
        float rr = std::sqrt(std::max(0.f, 1.f - y * y));
        float theta = two_pi * (float)i / golden;
        float x = std::cos(theta) * rr;
        float z = std::sin(theta) * rr;

        x *= 0.92f;
        y = y * 1.12f - 0.04f;
        z *= 1.08f;
        float nose = std::exp(-(x * x * 9.f + (y - 0.05f) * (y - 0.05f) * 14.f)) * 0.14f;
        z = z + nose;
        float cheek = std::fabs(y) + 0.12f;
        x *= 1.f + 0.11f * std::exp(-cheek * cheek * 4.f);
        y += 0.03f * std::exp(-(x * x * 5.f + (y - 0.35f) * (y - 0.35f) * 8.f));

        /* Keep a slightly tighter frontal cap so the XY projection reads as a face
         * plate, not a full globe cross-section. */
        if (z < -0.15f) {
            continue;
        }

        float nx = x;
        float ny = y * 1.08f;
        float nz = z + 0.18f;
        float nl = std::sqrt(nx * nx + ny * ny + nz * nz) + 1e-6f;
        nx /= nl;
        ny /= nl;
        nz /= nl;
        float shade = nz;
        if (shade < 0.f) {
            shade = 0.f;
        } else if (shade > 1.f) {
            shade = 1.f;
        }

        if (n_raw >= kRawMax) {
            break;
        }
        raw[n_raw++] = {x, -y, shade};
    }

    if (n_raw == 0) {
        return 0;
    }

    memset(used, 0, sizeof(s_scratch_used));

    int best_si = 0;
    float best_key = 1e9f;
    for (int i = 0; i < n_raw; ++i) {
        float dxn = raw[i].x * raw[i].x + (raw[i].y - 0.08f) * (raw[i].y - 0.08f);
        if (dxn < best_key) {
            best_key = dxn;
            best_si = i;
        }
    }
    chosen[0] = raw[best_si];
    used[best_si] = true;
    int chosen_n = 1;

    while (chosen_n < k_pick) {
        int best_i = -1;
        float best_d2 = -1.f;
        for (int i = 0; i < n_raw; ++i) {
            if (used[i]) {
                continue;
            }
            float px = raw[i].x;
            float py = raw[i].y;
            float mind2 = 1e9f;
            for (int k = 0; k < chosen_n; ++k) {
                float dx = px - chosen[k].x;
                float dy = py - chosen[k].y;
                float d2 = dx * dx + dy * dy;
                if (d2 < mind2) {
                    mind2 = d2;
                }
            }
            if (mind2 > best_d2) {
                best_d2 = mind2;
                best_i = i;
            }
        }
        if (best_i < 0) {
            break;
        }
        chosen[chosen_n++] = raw[best_i];
        used[best_i] = true;
    }

    float minx = chosen[0].x;
    float maxx = chosen[0].x;
    float miny = chosen[0].y;
    float maxy = chosen[0].y;
    for (int i = 0; i < chosen_n; ++i) {
        const HeadMeshRawPt& p = chosen[i];
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }
    float mxw = std::max(std::fabs(minx), std::fabs(maxx));
    float myw = std::max(std::fabs(miny), std::fabs(maxy));
    float mm = std::max(mxw, myw);
    if (mm < 1e-6f) {
        mm = 1e-6f;
    }
    const float norm_scale = 0.78f / mm;

    int n_out = chosen_n;
    if (n_out > k_pick) {
        n_out = k_pick;
    }
    for (int i = 0; i < n_out; ++i) {
        out_x[i] = chosen[i].x * norm_scale;
        out_y[i] = chosen[i].y * norm_scale;
        int su = (int)std::lround(chosen[i].shade * 255.f);
        if (su < 0) {
            su = 0;
        } else if (su > 255) {
            su = 255;
        }
        out_shade[(size_t)i] = (uint8_t)su;
    }
    return n_out;
}

/** Map ~circular frontal projection to a taller, narrower oval + simple jaw/nose read. */
static void ApplyFrontalFaceOval(float& tx, float& ty) {
    float x = tx;
    float y = ty;

    x *= 0.84f;
    y *= 1.10f;

    float nose_w = std::exp(-(x * x * 18.f + (y - 0.06f) * (y - 0.06f) * 14.f));
    y -= 0.030f * nose_w;

    float cheek = std::exp(-((std::fabs(x) - 0.22f) * (std::fabs(x) - 0.22f) * 40.f +
                             (y - 0.02f) * (y - 0.02f) * 10.f));
    const float dir = (x >= 0.f) ? 1.f : -1.f;
    x += dir * 0.020f * cheek;

    if (y > 0.08f) {
        float t = (y - 0.08f) / 0.55f;
        if (t > 1.f) {
            t = 1.f;
        }
        x *= 1.f - 0.24f * t * t;
        y += 0.045f * t;
    }

    tx = x;
    ty = y;
}

}  // namespace

JarvisPointCloud::JarvisPointCloud() = default;

JarvisPointCloud::~JarvisPointCloud() {
    Destroy();
}

void JarvisPointCloud::Destroy() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (canvas_ != nullptr) {
        lv_obj_del(canvas_);
        canvas_ = nullptr;
    }
    if (pixel_buffer_ != nullptr) {
        heap_caps_free(pixel_buffer_);
        pixel_buffer_ = nullptr;
    }
}

bool JarvisPointCloud::Create(lv_obj_t* parent, lv_coord_t width, lv_coord_t height) {
    Destroy();

    width_ = width;
    height_ = height;

    const float golden_angle = 2.39996323f;

    /* Fibonacci disk: evenly filled disk without crystalline symmetry. */
    for (int i = 0; i < kParticles; ++i) {
        float r = std::sqrt(((float)i + 0.5f) / (float)kParticles);
        float theta = golden_angle * (float)i;
        particles_[i].nx = std::cos(theta) * r;
        particles_[i].ny = std::sin(theta) * r;
        particles_[i].phase =
            theta * 0.37f + (float)i * 0.11f + std::sin((float)i * 1.7f + 3.1f) * 2.8f;
    }

    InitMorphTargets();

    constexpr uint8_t bpp = LV_COLOR_FORMAT_GET_BPP(LV_COLOR_FORMAT_RGB565);
    uint32_t buf_bytes = lv_canvas_buf_size(width_, height_, bpp, LV_DRAW_BUF_STRIDE_ALIGN);
    pixel_buffer_ =
        heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixel_buffer_ == nullptr) {
        pixel_buffer_ =
            heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (pixel_buffer_ == nullptr) {
        ESP_LOGE(TAG, "canvas buffer alloc failed (%" PRIu32 " bytes)", (uint32_t)buf_bytes);
        return false;
    }

    canvas_ = lv_canvas_create(parent);
    if (canvas_ == nullptr) {
        ESP_LOGE(TAG, "lv_canvas_create failed");
        Destroy();
        return false;
    }

    lv_canvas_set_buffer(canvas_, pixel_buffer_, width_, height_, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(canvas_, width_, height_);
    lv_obj_align(canvas_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_style_all(canvas_);

    /* ~25 FPS budget on BOX3 Jarvis HUD (dense mesh + fluffy aura vs full-frame canvas clear). */
    const uint32_t period_ms = 40;
    timer_ = lv_timer_create(TimerCb, period_ms, this);
    if (timer_ != nullptr) {
        lv_timer_set_repeat_count(timer_, -1);
    }

    Tick();
    return timer_ != nullptr;
}

void JarvisPointCloud::TimerCb(lv_timer_t* timer) {
    JarvisPointCloud* self = static_cast<JarvisPointCloud*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->Tick();
    }
}

void JarvisPointCloud::PlotPixel(int x, int y, lv_color_t color) {
    if (canvas_ == nullptr) {
        return;
    }
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return;
    }
    lv_canvas_set_px(canvas_, x, y, color, LV_OPA_COVER);
}

void JarvisPointCloud::DrawFluffyDot(int cx, int cy,
                                     lv_color_t core,
                                     lv_color_t mid_halo,
                                     lv_color_t outer_halo,
                                     float speech_fluff) {
    if (speech_fluff < 0.f) {
        speech_fluff = 0.f;
    }
    if (speech_fluff > 1.25f) {
        speech_fluff = 1.25f;
    }
    /* Tighter footprints → fewer canvas pixels/frame (~½ the brush area vs original). */
    float outer_r2 = 9.f + speech_fluff * 6.f;
    if (outer_r2 > 19.f) {
        outer_r2 = 19.f;
    }
    int r2_outer = (int)(outer_r2 + 0.5f);

    int rim = 2 + (int)(speech_fluff * 1.2f + 0.5f);
    if (rim > 5) {
        rim = 5;
    }

    float inner_lim = 1.f + speech_fluff * 0.75f;
    float mid_lim = 4.f + speech_fluff * 2.f;

    for (int dy = -rim; dy <= rim; ++dy) {
        for (int dx = -rim; dx <= rim; ++dx) {
            int r2 = dx * dx + dy * dy;
            if (r2 > r2_outer) {
                continue;
            }
            float fr2 = static_cast<float>(r2);
            lv_color_t col = outer_halo;
            if (fr2 <= inner_lim) {
                col = core;
            } else if (fr2 <= mid_lim) {
                col = mid_halo;
            }
            PlotPixel(cx + dx, cy + dy, col);
        }
    }
}

void JarvisPointCloud::DrawCompactShadedDot(int cx, int cy,
                                            lv_color_t bright_core,
                                            lv_color_t bright_mid,
                                            lv_color_t rim,
                                            uint8_t lambert_u8,
                                            float speech_fluff) {
    if (speech_fluff < 0.f) {
        speech_fluff = 0.f;
    }
    if (speech_fluff > 0.85f) {
        speech_fluff = 0.85f;
    }

    lv_color_t core = rim;
    lv_color_t mid = rim;
    if (lambert_u8 > 210) {
        core = bright_core;
        mid = bright_mid;
    } else if (lambert_u8 > 140) {
        core = bright_mid;
        mid = rim;
    } else if (lambert_u8 > 80) {
        core = rim;
        mid = rim;
    }

    float outer_r2 = 5.f + speech_fluff * 2.f;
    if (outer_r2 > 9.f) {
        outer_r2 = 9.f;
    }
    int r2_outer = (int)(outer_r2 + 0.5f);

    int rim_px = 1 + (int)(speech_fluff * 0.45f + 0.5f);
    if (rim_px > 2) {
        rim_px = 2;
    }

    float inner_lim = 0.55f + speech_fluff * 0.35f;
    float mid_lim = 2.f + speech_fluff * 0.7f;

    for (int dy = -rim_px; dy <= rim_px; ++dy) {
        for (int dx = -rim_px; dx <= rim_px; ++dx) {
            int r2 = dx * dx + dy * dy;
            if (r2 > r2_outer) {
                continue;
            }
            float fr2 = static_cast<float>(r2);
            lv_color_t col = rim;
            if (fr2 <= inner_lim) {
                col = core;
            } else if (fr2 <= mid_lim) {
                col = mid;
            }
            PlotPixel(cx + dx, cy + dy, col);
        }
    }
}

void JarvisPointCloud::Tick() {
    if (canvas_ == nullptr || paused_) {
        return;
    }

    lv_display_t* disp = lv_obj_get_disp(canvas_);
    if (disp == nullptr) {
        return;
    }

    const lv_color_t hud_bg = lv_color_hex(0x061018);
    const lv_color_t dot_core = lv_color_hex(0x62F0FF);
    const lv_color_t dot_mid = lv_color_hex(0x35a8c8);
    const lv_color_t dot_outer = lv_color_hex(0x214a62);

    lv_canvas_fill_bg(canvas_, hud_bg, LV_OPA_COVER);

    lv_display_enable_invalidation(disp, false);

    float mic_in = JarvisSpeechMeterTargetLevel();
    const float atk_coef = (mic_in > speech_drive_smoothed_) ? 0.52f : 0.17f;
    speech_drive_smoothed_ += (mic_in - speech_drive_smoothed_) * atk_coef;
    float drive = speech_drive_smoothed_;
    if (drive > 1.f) {
        drive = 1.f;
    }

    /* Pull dots toward the frontal head when playback level is high.
     * Avoid squaring too early — it kept morph_w low at typical TTS levels (reads as a ring/cloud). */
    float morph_target = 0.f;
    if (drive > 0.048f) {
        morph_target = (drive - 0.048f) / 0.30f;
        if (morph_target > 1.f) {
            morph_target = 1.f;
        }
        /* Smoothstep: soft knees but reaches ~1 during normal speech. */
        morph_target = morph_target * morph_target * (3.f - 2.f * morph_target);
    }
    const float morph_atk =
        (morph_target > morph_smoothed_) ? 0.52f : 0.10f;
    morph_smoothed_ += (morph_target - morph_smoothed_) * morph_atk;
    if (morph_smoothed_ < 0.f) {
        morph_smoothed_ = 0.f;
    } else if (morph_smoothed_ > 1.f) {
        morph_smoothed_ = 1.f;
    }
    float morph_pos = morph_smoothed_;
    /* Ease-out toward the posed head earlier in mid-speech (linear morph undershoot). */
    if (morph_pos > 1.f) {
        morph_pos = 1.f;
    }
    if (morph_pos > 0.001f) {
        morph_pos = 1.f - std::pow(1.f - morph_pos, 2.08f);
    }

    /* Calm base tempo; ramps with speech drive (raised vs v1 — snappier cloud). */
    t_ += 0.092f + drive * 0.038f;

    float cx = (float)width_ * 0.5f;
    float cy = (float)height_ * 0.5f;
    /* Nudge posed head downward so foreheads/top-of-crown dots are not all clamped to y_min (“flat-top vase”). */
    const float cy_head = cy + morph_pos * ((float)height_ * 0.068f);

    float breathe = std::sin(t_ * 0.61f);
    float scale =
        (1.f + breathe * 0.13f + drive * 0.095f + drive * 0.062f * std::sin(t_ * 2.7f + 0.4f)) * 0.98f;

    float jm = 0.86f + drive * 1.12f;

    int margin = 5 + (int)(drive * 2.5f);
    if (margin > 10) {
        margin = 10;
    }

    float inset_base = 6.f + drive * 5.f;
    float rx = (float)width_ * scale * 0.5f - inset_base;
    float ry = (float)height_ * scale * 0.5f - inset_base;
    rx *= (1.f + drive * 0.07f);
    ry *= (1.f + drive * 0.06f);

    float rot = t_ * 0.058f + drive * 0.055f * std::sin(t_ * 10.1f + drive * 4.f);
    float cr = std::cos(rot);
    float sr = std::sin(rot);

    float drift_x = std::sin(t_ * 0.34f + 2.91f) * ((float)width_ * 0.026f *
                                                   (0.93f + 0.07f * std::sin(t_ * 0.21f))) +
                  std::sin(t_ * 0.094f + 8.51f) * ((float)width_ * 0.008f);

    float drift_y = std::cos(t_ * 0.27f + 1.64f) * ((float)height_ * 0.022f) +
                    std::cos(t_ * 0.087f + 4.93f) * ((float)height_ * 0.007f);

    drift_x += drive * (float)width_ * 0.032f * std::sin(t_ * (17.f + drive * 38.f) + 1.03f);
    drift_y += drive * (float)height_ * 0.028f * std::cos(t_ * (20.f + drive * 30.f) + 2.71f);

    int x_min = margin;
    int y_min = margin;
    int x_max = (int)width_ - margin - 1;
    int y_max = (int)height_ - margin - 1;

    for (int i = 0; i < kParticles; ++i) {
        const Particle& p = particles_[i];

        float qx = p.nx * cr - p.ny * sr;
        float qy = p.nx * sr + p.ny * cr;

        float talk = drive * (float)width_ * 0.031f;

        float ox = drift_x +
                   jm * (std::sin(t_ * 1.71f + p.phase * 2.93f + std::sin(t_ * 0.47f)) *
                             ((float)width_ * 0.038f) +
                         std::sin(t_ * 0.71f + p.phase * 1.17f) * ((float)width_ * 0.019f)) +
                   talk * std::sin(t_ * (19.f + drive * 40.f) + p.phase * 4.1f);

        float oy = drift_y +
                   jm * (std::cos(t_ * 1.88f + p.phase * 2.71f + std::cos(t_ * 0.39f)) *
                             ((float)height_ * 0.036f) +
                         std::sin(t_ * 0.82f + p.phase * 1.63f) * ((float)height_ * 0.023f)) +
                   talk * 0.85f *
                       std::cos(t_ * (22.f + drive * 32.f) + p.phase * 3.4f + std::sin(t_ * 2.2f));

        float cpx = cx + qx * rx + ox;
        float cpy = cy + qy * ry + oy;

        float head_rad =
            std::min(rx, ry) * 1.70f +
            drive * (float)((width_ < height_) ? width_ : height_) * 0.017f;

        /* Keep ellipse + halo inside the usable ellipse so edge clamping doesn't erase the hairstyle/crown read. */
        const float radial_room = std::min(rx, ry) - (float)(margin + 8);
        if (radial_room > 12.f && head_rad > radial_room / 1.02f) {
            head_rad = radial_room / 1.02f;
        }

        const float aura_rad = head_rad * 1.13f;

        constexpr float chin_y_cut = 0.10f;
        float mouth_wiggle =
            drive * head_rad * 0.048f *
            std::sin(t_ * (15.5f + drive * 9.f));

        float mouth_open = 0.f;
        if (i < kHeadMeshTarget) {
            mouth_open =
                (head_ty_[i] > chin_y_cut)
                    ? mouth_wiggle * (0.85f + 0.15f * head_ty_[i])
                    : 0.f;
        } else {
            mouth_open =
                mouth_wiggle * (0.12f + 0.10f * std::fabs(head_ty_[i]));
        }

        float jaw_bob = 0.f;
        if (i < kHeadMeshTarget && head_ty_[i] > chin_y_cut * 0.7f) {
            jaw_bob = drive * head_rad * 0.026f *
                      std::sin(t_ * (17.f + drive * 11.f) + head_tx_[i] * 7.f);
        } else if (i >= kHeadMeshTarget) {
            jaw_bob = drive * aura_rad * 0.010f *
                      std::sin(t_ * (13.f + drive * 8.f) + head_tx_[i] * 4.f);
        }

        float rad_i = (i < kHeadMeshTarget) ? head_rad : aura_rad;
        float hpx = cx + head_tx_[i] * rad_i + drift_x * 0.22f + mouth_open;
        float hpy = cy_head + head_ty_[i] * rad_i + drift_y * 0.22f + jaw_bob;

        float px_f = cpx + (hpx - cpx) * morph_pos;
        float py_f = cpy + (hpy - cpy) * morph_pos;

        int px = (int)(px_f + 0.5f);
        int py = (int)(py_f + 0.5f);

        if (px < x_min) {
            px = x_min;
        } else if (px > x_max) {
            px = x_max;
        }
        if (py < y_min) {
            py = y_min;
        } else if (py > y_max) {
            py = y_max;
        }

        scr_x_[i] = px;
        scr_y_[i] = py;
    }

    float fluff_paint = 0.28f + drive * 0.88f;
    const float fluff_interior = 0.26f + drive * 0.58f;
    const float fluff_outline = 0.34f + drive * 0.54f;

    for (int i = 0; i < kFaceOutlinePoints; ++i) {
        DrawFluffyDot(scr_x_[i],
                      scr_y_[i],
                      dot_core,
                      dot_mid,
                      dot_outer,
                      fluff_outline);
    }
    for (int i = kFaceOutlinePoints; i < kHeadMeshTarget; ++i) {
        DrawFluffyDot(scr_x_[i],
                      scr_y_[i],
                      dot_core,
                      dot_mid,
                      dot_outer,
                      fluff_interior);
    }
    for (int i = kHeadMeshTarget; i < kParticles; ++i) {
        DrawFluffyDot(scr_x_[i], scr_y_[i], dot_core, dot_mid, dot_outer, fluff_paint);
    }

    lv_display_enable_invalidation(disp, true);
    lv_obj_invalidate(canvas_);
}

void JarvisPointCloud::InitMorphTargets() {
    constexpr float two_pi = 6.28318530718f;
    constexpr float phi0 = -1.57079632679f;
    constexpr float chin_outline = -0.058f;

    /* Visible “mask” silhouette: frontal egg + jaw taper (pure 2D, reads as human on small LCDs). */
    for (int i = 0; i < kFaceOutlinePoints; ++i) {
        float phi = phi0 + two_pi * (((float)i + 0.5f) / (float)kFaceOutlinePoints);
        float c = std::cos(phi);
        float s = std::sin(phi);

        float r =
            0.47f + 0.104f * c + 0.078f * std::cos(2.f * phi + 0.48f) + 0.048f * std::sin(3.f * phi + 0.15f);
        if (r < 0.38f) {
            r = 0.38f;
        } else if (r > 0.64f) {
            r = 0.64f;
        }

        float tx = r * c;
        float ty = r * s * 1.15f + chin_outline * std::fabs(s) + 0.032f * std::sin(2.f * phi);

        /* Narrow chin compared to cheeks so the silhouette reads closer to a head than to a vase. */
        float s_phi = std::sin(phi);
        if (s_phi > 0.18f) {
            float chin_t = (s_phi - 0.18f) / 0.82f;
            if (chin_t > 1.f) {
                chin_t = 1.f;
            }
            tx *= 1.f - 0.22f * chin_t * chin_t;
        }

        ApplyFrontalFaceOval(tx, ty);
        head_tx_[i] = tx;
        head_ty_[i] = ty;

        float lite = std::sin(phi * 1.05f + 0.85f);
        float shf = 0.55f + 0.40f * (0.5f + 0.5f * lite);
        int su = (int)std::lround(shf * 255.f);
        if (su < 118) {
            su = 118;
        } else if (su > 255) {
            su = 255;
        }
        mesh_shade_[i] = (uint8_t)su;
    }

    float mx[kHeadInteriorPoints];
    float my[kHeadInteriorPoints];
    uint8_t ms[kHeadInteriorPoints];

    int mn = BuildGenericMaleHeadMesh(mx, my, ms, kHeadInteriorPoints);
    if (mn <= 0) {
        mn = 1;
        mx[0] = 0.f;
        my[0] = 0.f;
        ms[0] = 160;
    }
    if (mn < kHeadInteriorPoints) {
        const float lx = mx[(size_t)mn - 1];
        const float ly = my[(size_t)mn - 1];
        const uint8_t ls = ms[(size_t)mn - 1];
        for (int j = mn; j < kHeadInteriorPoints; ++j) {
            mx[(size_t)j] = lx;
            my[(size_t)j] = ly;
            ms[(size_t)j] = ls;
        }
    }

    for (int j = 0; j < kHeadInteriorPoints; ++j) {
        const int i = kFaceOutlinePoints + j;
        float tx = mx[(size_t)j] * 0.67f;
        float ty = my[(size_t)j] * 0.67f;
        ApplyFrontalFaceOval(tx, ty);
        head_tx_[i] = tx;
        head_ty_[i] = ty;
        mesh_shade_[i] = ms[(size_t)j];
    }

    /* Outer aura: halo cloud (not another clean polygon). */
    constexpr float chin_squash = -0.04f;
    const float aura_golden = 2.39996323f;

    for (int j = 0; j < kAuraParticles; ++j) {
        const int i = kHeadMeshTarget + j;
        const int layer = j % 3;
        float phi = aura_golden * (float)j + 0.41f * (float)layer;

        float c = std::cos(phi);
        float s = std::sin(phi);

        float base_r = 0.54f + 0.055f * (float)layer;
        float r = base_r + 0.09f * std::sin(2.35f * phi + 0.52f) + 0.055f * std::sin(5.05f * phi + 1.08f);
        r *= 1.f + 0.11f * std::sin(7.1f * phi + (float)j * 0.19f);
        if (r < 0.43f) {
            r = 0.43f;
        } else if (r > 0.78f) {
            r = 0.78f;
        }

        float tx = r * c + 0.052f * std::sin(4.9f * phi + 0.71f * (float)j);
        float ty =
            r * s * 1.10f + chin_squash * std::fabs(s) + 0.028f * std::sin(2.f * phi) +
            0.042f * std::cos(3.85f * phi + 0.53f * (float)j);

        ApplyFrontalFaceOval(tx, ty);

        float ox = tx;
        float oy = ty;

        float dist = std::sqrt(ox * ox + oy * oy) + 1e-4f;
        const float boost = 1.095f + 0.048f * (float)layer;
        head_tx_[i] = (ox / dist) * (dist * boost);
        head_ty_[i] = (oy / dist) * (dist * boost);
    }
}
