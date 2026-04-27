#include "hud_display.h"
#include "sensor_state.h"
#include "weather_state.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define TAG "HudDisplay"

// ── Layout ──────────────────────────────────────────────────────────────────
#define SCREEN_W 320
#define SCREEN_H 240

#define TOP_PAD  4     // panel top margin to avoid edge clipping
#define TOP_H    24    // top status bar (text + padding)
#define BOT_H    28    // bottom chat label

// Eyes own the upper area, full width. No side columns now.
// Slightly smaller eyes so the bottom info strip fits comfortably with the
// taller top bar and bigger label rows.
#define EYE_W     78
#define EYE_H     46
#define EYE_R     7
#define EYE_BDR   3
#define EYE_GAP   22
#define EYE_CY    (TOP_H + 4 + EYE_H/2)

#define EYE_LEFT_CX  (SCREEN_W/2 - EYE_GAP/2 - EYE_W/2)
#define EYE_RIGHT_CX (SCREEN_W/2 + EYE_GAP/2 + EYE_W/2)

// Iris
#define IRIS_D   28

// Scan arc
#define SCAN_D       46
#define SCAN_ARC_W   4

// Mouth waveform — directly below the eyes (always visible; min when idle)
#define MOUTH_Y       (EYE_CY + EYE_H/2 + 6)
#define MOUTH_BAR_W   10
#define MOUTH_BAR_GAP 5
#define MOUTH_BAR_H_MIN 3      // baseline height when not speaking
#define MOUTH_BAR_H_MAX 22

// Sensor info strip — horizontal row(s) below the mouth, above the chat
#define INFO_ROW_H    20                                  // tall enough for default 14px font + descenders
#define INFO_ROW1_Y   (MOUTH_Y + MOUTH_BAR_H_MAX + 8)
#define INFO_ROW2_Y   (INFO_ROW1_Y + INFO_ROW_H + 2)

// ── Colour palette ──────────────────────────────────────────────────────────
#define C_BG       lv_color_hex(0x000000)
#define C_HUD      lv_color_hex(0x00CCFF)   // Jarvis cyan (primary)
#define C_HUD_DIM  lv_color_hex(0x004C66)   // dimmed cyan
#define C_SCAN     lv_color_hex(0xFF8800)   // orange listening accent
#define C_SPEAK    lv_color_hex(0x00FFCC)   // teal-white speaking pulse
#define C_DIM      lv_color_hex(0x005A70)
#define C_BOOT     lv_color_hex(0x00FF66)   // matrix rain green
#define C_BOOT_HD  lv_color_hex(0xCCFFCC)   // bright leading char

// ── Animation constants ──────────────────────────────────────────────────────
#define ANIM_PERIOD_MS    50
#define IDLE_BREATH_HALF  30
#define IDLE_OPA_LO       60
#define IDLE_OPA_HI       200
#define SCAN_STEP_DEG     15
#define SCAN_SWEEP        90

// Boot
#define BOOT_PERIOD_MS    50
#define BOOT_DURATION_MS  2500
#define BOOT_FONT_W       8       // approx column width for matrix rain
#define BOOT_FONT_H       12

// ── Helpers ─────────────────────────────────────────────────────────────────
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int lerp(int a, int b, int t, int tmax) {
    if (tmax <= 0) return a;
    return a + (b - a) * t / tmax;
}

// ── Construction ────────────────────────────────────────────────────────────
HudLcdDisplay::HudLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy) {}

HudLcdDisplay::~HudLcdDisplay() {
    if (anim_timer_) lv_timer_delete(anim_timer_);
    if (boot_timer_) lv_timer_delete(boot_timer_);
    if (boot_buf_)   heap_caps_free(boot_buf_);
    if (rain_y_)     std::free(rain_y_);
}

// ── Eye creation ────────────────────────────────────────────────────────────
void HudLcdDisplay::CreateEye(lv_obj_t* parent, int cx, int cy,
                              lv_obj_t** frame_out, lv_obj_t** iris_out, lv_obj_t** scan_out) {
    lv_obj_t* frame = lv_obj_create(parent);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, EYE_W, EYE_H);
    lv_obj_set_pos(frame, cx - EYE_W/2, cy - EYE_H/2);
    lv_obj_set_style_radius(frame, EYE_R, 0);
    lv_obj_set_style_border_color(frame, C_HUD, 0);
    lv_obj_set_style_border_width(frame, EYE_BDR, 0);
    lv_obj_set_style_bg_color(frame, C_BG, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    *frame_out = frame;

    lv_obj_t* iris = lv_obj_create(frame);
    lv_obj_remove_style_all(iris);
    lv_obj_set_size(iris, IRIS_D, IRIS_D);
    lv_obj_align(iris, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(iris, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(iris, C_HUD, 0);
    lv_obj_set_style_bg_opa(iris, IDLE_OPA_HI, 0);
    lv_obj_set_style_border_width(iris, 0, 0);
    lv_obj_clear_flag(iris, LV_OBJ_FLAG_SCROLLABLE);
    *iris_out = iris;

    lv_obj_t* arc = lv_arc_create(frame);
    lv_obj_set_size(arc, SCAN_D, SCAN_D);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(arc, 0);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, 0, SCAN_SWEEP);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, C_SCAN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, SCAN_ARC_W, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);  // shown only in LISTENING
    *scan_out = arc;
}

// ── State transitions ───────────────────────────────────────────────────────
void HudLcdDisplay::SetHudState(HudState state) {
    if (state == hud_state_) return;
    hud_state_ = state;

    if (state == HudState::LISTENING) {
        if (scan_left_)  lv_obj_clear_flag(scan_left_,  LV_OBJ_FLAG_HIDDEN);
        if (scan_right_) lv_obj_clear_flag(scan_right_, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (scan_left_)  lv_obj_add_flag(scan_left_,  LV_OBJ_FLAG_HIDDEN);
        if (scan_right_) lv_obj_add_flag(scan_right_, LV_OBJ_FLAG_HIDDEN);
    }

    // Mouth bars stay visible in all states — animated during SPEAKING,
    // static minimum height (and dimmer colour) when idle/listening.
    lv_color_t bar_color = (state == HudState::SPEAKING) ? C_SPEAK : C_HUD_DIM;
    for (int i = 0; i < kMouthBars; ++i) {
        if (!mouth_bars_[i]) continue;
        lv_obj_clear_flag(mouth_bars_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(mouth_bars_[i], bar_color, 0);
        if (state != HudState::SPEAKING) {
            lv_obj_set_height(mouth_bars_[i], MOUTH_BAR_H_MIN);
            lv_obj_set_y(mouth_bars_[i], MOUTH_Y + (MOUTH_BAR_H_MAX - MOUTH_BAR_H_MIN) / 2);
        }
    }
}

// ── SetupUI: shows boot sequence on first call, then assembles HUD ──────────
void HudLcdDisplay::SetupUI() {
    if (setup_ui_called_) return;
    setup_ui_called_ = true;

    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Hidden parent-class stubs first — prevents crash in theme/refresh code.
    container_          = lv_obj_create(screen);
    status_bar_         = lv_obj_create(container_);
    content_            = lv_obj_create(container_);
    side_bar_           = lv_obj_create(container_);
    top_bar_            = lv_obj_create(container_);
    preview_image_      = lv_obj_create(content_);
    mute_label_         = lv_label_create(top_bar_);
    battery_label_      = lv_label_create(top_bar_);
    network_label_      = lv_label_create(top_bar_);
    notification_label_ = lv_label_create(status_bar_);
    status_label_       = lv_label_create(status_bar_);
    chat_message_label_ = lv_label_create(content_);
    emoji_label_        = lv_label_create(content_);
    low_battery_popup_  = lv_obj_create(screen);

    lv_obj_t* hidden_objs[] = {
        container_, status_bar_, content_, side_bar_, top_bar_,
        preview_image_, mute_label_, battery_label_, network_label_,
        notification_label_, status_label_, chat_message_label_,
        emoji_label_, low_battery_popup_,
    };
    for (auto o : hidden_objs) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(o, 1, 1);
        lv_obj_set_pos(o, -1000, -1000);
    }
    lv_label_set_text(mute_label_,         "");
    lv_label_set_text(battery_label_,      "");
    lv_label_set_text(network_label_,      "");
    lv_label_set_text(notification_label_, "");
    lv_label_set_text(status_label_,       "");
    lv_label_set_text(chat_message_label_, "");
    lv_label_set_text(emoji_label_,        "");

    StartBootSequence();
}

// ── Matrix-rain boot canvas ─────────────────────────────────────────────────
void HudLcdDisplay::StartBootSequence() {
    lv_obj_t* screen = lv_screen_active();

    // Allocate a full-screen RGB565 canvas in PSRAM.
    const size_t buf_size = SCREEN_W * SCREEN_H * sizeof(lv_color_t);
    boot_buf_ = (lv_color_t*)heap_caps_malloc(buf_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!boot_buf_) {
        // PSRAM unavailable — skip the rain, go straight to HUD.
        ESP_LOGW(TAG, "boot canvas alloc failed; skipping rain");
        BuildHudUi();
        booted_ = true;
        anim_timer_ = lv_timer_create(OnAnimTimer, ANIM_PERIOD_MS, this);
        return;
    }

    boot_canvas_ = lv_canvas_create(screen);
    lv_canvas_set_buffer(boot_canvas_, boot_buf_, SCREEN_W, SCREEN_H, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(boot_canvas_, C_BG, LV_OPA_COVER);

    // Per-column current y position (random staggered start).
    rain_cols_ = SCREEN_W / BOOT_FONT_W;
    rain_y_ = (int*)std::malloc(rain_cols_ * sizeof(int));
    for (int i = 0; i < rain_cols_; ++i) {
        rain_y_[i] = -((std::rand() % SCREEN_H));
    }

    boot_tick_ = 0;
    boot_timer_ = lv_timer_create(OnBootTimer, BOOT_PERIOD_MS, this);
}

void HudLcdDisplay::OnBootTimer(lv_timer_t* t) {
    auto* self = static_cast<HudLcdDisplay*>(lv_timer_get_user_data(t));
    if (self) self->StepBootRain();
}

void HudLcdDisplay::StepBootRain() {
    if (!boot_canvas_ || !boot_buf_) return;

    // Soft fade — multiply every pixel down by ~7/8 each frame for trail decay.
    {
        lv_color_t* p = boot_buf_;
        const int n = SCREEN_W * SCREEN_H;
        for (int i = 0; i < n; ++i) {
            uint16_t v = lv_color_to_u16(p[i]);
            uint16_t r = (v >> 11) & 0x1F;
            uint16_t g = (v >> 5) & 0x3F;
            uint16_t b = v & 0x1F;
            r = (r * 7) >> 3;
            g = (g * 7) >> 3;
            b = (b * 7) >> 3;
            p[i] = lv_color_make((r << 3), (g << 2), (b << 3));
        }
    }

    // Advance each column. Draw a small rectangle as "char" — leading row
    // bright, trail dim.
    for (int c = 0; c < rain_cols_; ++c) {
        rain_y_[c] += BOOT_FONT_H;
        if (rain_y_[c] >= SCREEN_H) {
            rain_y_[c] = -((std::rand() % 80) + 16);
        }
        const int x = c * BOOT_FONT_W;
        const int y = rain_y_[c];
        if (y < 0 || y >= SCREEN_H) continue;
        // bright leading "char"
        lv_layer_t layer;
        lv_canvas_init_layer(boot_canvas_, &layer);
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = C_BOOT_HD;
        dsc.bg_opa = LV_OPA_COVER;
        lv_area_t a = { x, y, x + BOOT_FONT_W - 2, y + BOOT_FONT_H - 2 };
        lv_draw_rect(&layer, &dsc, &a);
        // dim trailing block above
        dsc.bg_color = C_BOOT;
        dsc.bg_opa = 180;
        if (y - BOOT_FONT_H >= 0) {
            lv_area_t a2 = { x, y - BOOT_FONT_H, x + BOOT_FONT_W - 2, y - 2 };
            lv_draw_rect(&layer, &dsc, &a2);
        }
        lv_canvas_finish_layer(boot_canvas_, &layer);
    }
    lv_obj_invalidate(boot_canvas_);

    boot_tick_ += BOOT_PERIOD_MS;
    if (boot_tick_ >= BOOT_DURATION_MS) {
        // Reveal the HUD.
        if (boot_timer_) { lv_timer_delete(boot_timer_); boot_timer_ = nullptr; }
        RevealHud();
    }
}

void HudLcdDisplay::RevealHud() {
    BuildHudUi();
    booted_ = true;
    if (boot_canvas_) {
        lv_obj_add_flag(boot_canvas_, LV_OBJ_FLAG_HIDDEN);
        // Keep boot_buf_ allocated for now — it's PSRAM and freeing safely
        // would require lvgl synchronization. Cost: ~150KB PSRAM. Acceptable.
    }
    if (rain_y_) { std::free(rain_y_); rain_y_ = nullptr; }
    anim_timer_ = lv_timer_create(OnAnimTimer, ANIM_PERIOD_MS, this);
}

// ── Build all live HUD elements (eyes, mouth, sensor widgets) ───────────────
void HudLcdDisplay::BuildHudUi() {
    lv_obj_t* screen = lv_screen_active();

    // ── Top status strip ───────────────────────────────────────────────────
    top_label_ = lv_label_create(screen);
    lv_obj_set_size(top_label_, SCREEN_W, TOP_H);
    lv_obj_set_pos(top_label_, 0, TOP_PAD);
    lv_obj_set_style_text_color(top_label_, C_HUD, 0);
    lv_obj_set_style_bg_opa(top_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_align(top_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(top_label_, "JARVIS ONLINE");

    time_label_ = lv_label_create(screen);
    lv_obj_set_pos(time_label_, SCREEN_W - 60, TOP_PAD);
    lv_obj_set_style_text_color(time_label_, C_HUD_DIM, 0);
    lv_label_set_text(time_label_, "--:--");

    // Horizontal rule under top
    lv_obj_t* hline = lv_obj_create(screen);
    lv_obj_remove_style_all(hline);
    lv_obj_set_size(hline, SCREEN_W, 1);
    lv_obj_set_pos(hline, 0, TOP_H);
    lv_obj_set_style_bg_color(hline, C_HUD, 0);
    lv_obj_set_style_bg_opa(hline, LV_OPA_40, 0);

    // ── Eyes ───────────────────────────────────────────────────────────────
    CreateEye(screen, EYE_LEFT_CX,  EYE_CY, &eye_left_frame_,  &iris_left_,  &scan_left_);
    CreateEye(screen, EYE_RIGHT_CX, EYE_CY, &eye_right_frame_, &iris_right_, &scan_right_);

    // ── Info strip below the mouth ─────────────────────────────────────────
    // Row 1: ROOM 77F   HUM 49%   OCC *   OUT 65F
    // Row 2: weather condition + location (centered)
    //
    // Layout uses fixed column centres so the labels stay nicely aligned.
    auto make_label = [&](int x, int y, int width, lv_color_t color, lv_text_align_t align) {
        lv_obj_t* l = lv_label_create(screen);
        lv_obj_set_pos(l, x, y);
        lv_obj_set_size(l, width, INFO_ROW_H);
        lv_obj_set_style_text_color(l, color, 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_align(l, align, 0);
        // Default LV_LABEL_LONG_WRAP — will overflow horizontally rather than
        // truncate, so we'd rather see the full text than "…"
        return l;
    };
    // 4 equal columns spanning the full width. Each ~80 px wide.
    const int col_w = SCREEN_W / 4;
    temp_label_    = make_label(0 * col_w, INFO_ROW1_Y, col_w, C_HUD,     LV_TEXT_ALIGN_CENTER);
    hum_label_     = make_label(1 * col_w, INFO_ROW1_Y, col_w, C_HUD_DIM, LV_TEXT_ALIGN_CENTER);
    occ_label_     = make_label(2 * col_w, INFO_ROW1_Y, col_w, C_HUD,     LV_TEXT_ALIGN_CENTER);
    weather_label_ = make_label(3 * col_w, INFO_ROW1_Y, col_w, C_SPEAK,   LV_TEXT_ALIGN_CENTER);

    // Row 2: weather condition spans full width, centered, dim cyan.
    loc_label_ = make_label(4, INFO_ROW2_Y, SCREEN_W - 8, C_HUD_DIM, LV_TEXT_ALIGN_CENTER);

    lv_label_set_text(temp_label_,    "RM --F");
    lv_label_set_text(hum_label_,     "H --%");
    lv_label_set_text(occ_label_,     "OCC ?");
    lv_label_set_text(weather_label_, "OUT --F");
    lv_label_set_text(loc_label_,     "");

    // ── Animated mouth (waveform bars) ─────────────────────────────────────
    const int total_w = kMouthBars * MOUTH_BAR_W + (kMouthBars - 1) * MOUTH_BAR_GAP;
    int x0 = SCREEN_W/2 - total_w/2;
    for (int i = 0; i < kMouthBars; ++i) {
        lv_obj_t* b = lv_obj_create(screen);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, MOUTH_BAR_W, MOUTH_BAR_H_MIN);
        // Vertically centre the minimum-height bar within the mouth slot
        lv_obj_set_pos(b, x0 + i * (MOUTH_BAR_W + MOUTH_BAR_GAP),
                       MOUTH_Y + (MOUTH_BAR_H_MAX - MOUTH_BAR_H_MIN) / 2);
        lv_obj_set_style_bg_color(b, C_HUD_DIM, 0);  // dim baseline when idle
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 2, 0);
        // Always visible — height + colour change when SPEAKING
        mouth_bars_[i] = b;
    }

    // ── Bottom rule ────────────────────────────────────────────────────────
    lv_obj_t* bline = lv_obj_create(screen);
    lv_obj_remove_style_all(bline);
    lv_obj_set_size(bline, SCREEN_W, 1);
    lv_obj_set_pos(bline, 0, SCREEN_H - BOT_H - 1);
    lv_obj_set_style_bg_color(bline, C_HUD, 0);
    lv_obj_set_style_bg_opa(bline, LV_OPA_40, 0);

    // ── Bottom chat label ──────────────────────────────────────────────────
    bot_label_ = lv_label_create(screen);
    lv_obj_set_size(bot_label_, SCREEN_W - 8, BOT_H);
    lv_obj_set_pos(bot_label_, 4, SCREEN_H - BOT_H);
    lv_obj_set_style_text_color(bot_label_, C_HUD, 0);
    lv_obj_set_style_bg_opa(bot_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_align(bot_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(bot_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(bot_label_, "Ready");

    // Kick off the weather poller (no-op if no API key configured).
    weather_state_start();
}

// ── Animation tick ──────────────────────────────────────────────────────────
void HudLcdDisplay::OnAnimTimer(lv_timer_t* t) {
    auto* self = static_cast<HudLcdDisplay*>(lv_timer_get_user_data(t));
    if (self) self->StepAnimation();
}

void HudLcdDisplay::StepAnimation() {
    if (!booted_) return;

    ++anim_tick_;

    // Eye animation per state ────────────────────────────────────────────
    if (hud_state_ == HudState::IDLE) {
        int half = anim_tick_ / IDLE_BREATH_HALF;
        bool up = (half & 1) ? false : true;
        int t = anim_tick_ % IDLE_BREATH_HALF;
        int opa = up ? lerp(IDLE_OPA_LO, IDLE_OPA_HI, t, IDLE_BREATH_HALF)
                     : lerp(IDLE_OPA_HI, IDLE_OPA_LO, t, IDLE_BREATH_HALF);
        if (iris_left_)  lv_obj_set_style_bg_opa(iris_left_,  opa, 0);
        if (iris_right_) lv_obj_set_style_bg_opa(iris_right_, opa, 0);
    } else if (hud_state_ == HudState::LISTENING) {
        scan_angle_ = (scan_angle_ + SCAN_STEP_DEG) % 360;
        if (scan_left_)  lv_arc_set_angles(scan_left_,  scan_angle_, scan_angle_ + SCAN_SWEEP);
        if (scan_right_) lv_arc_set_angles(scan_right_, (360 - scan_angle_) % 360, (360 - scan_angle_ + SCAN_SWEEP) % 360);
        if (iris_left_)  lv_obj_set_style_bg_opa(iris_left_,  IDLE_OPA_HI, 0);
        if (iris_right_) lv_obj_set_style_bg_opa(iris_right_, IDLE_OPA_HI, 0);
    } else { // SPEAKING
        if ((anim_tick_ % 10) == 0) {
            pulse_up_ = !pulse_up_;
        }
        int opa = pulse_up_ ? 255 : 180;
        if (iris_left_)  lv_obj_set_style_bg_opa(iris_left_,  opa, 0);
        if (iris_right_) lv_obj_set_style_bg_opa(iris_right_, opa, 0);
        StepMouthAnimation();
    }

    // Sensor widgets — refresh at 1 Hz
    if (++sensor_tick_ >= 20) {
        sensor_tick_ = 0;
        UpdateSensorWidgets();
    }

    // Weather widgets — refresh at 0.2 Hz (poller updates every 15 min anyway)
    if (++weather_tick_ >= 100) {
        weather_tick_ = 0;
        UpdateWeatherWidgets();
    }
}

// ── Mouth waveform ──────────────────────────────────────────────────────────
void HudLcdDisplay::StepMouthAnimation() {
    ++mouth_phase_;
    for (int i = 0; i < kMouthBars; ++i) {
        if (!mouth_bars_[i]) continue;
        // Centre bar gets biggest swing; phase-shift each so it looks like
        // a travelling wave. Use a cheap integer wave (not real audio amp;
        // requires routing TTS opus frames here, which we'll wire later).
        float phase = (mouth_phase_ * 0.6f) + i * 0.7f;
        float center_bias = 1.0f - 0.65f * std::fabs((float)i - (kMouthBars - 1) / 2.0f) / ((kMouthBars - 1) / 2.0f);
        float v = (std::sin(phase) * 0.5f + 0.5f) * center_bias;  // 0..1
        int h = MOUTH_BAR_H_MIN + (int)(v * (MOUTH_BAR_H_MAX - MOUTH_BAR_H_MIN));
        h = clampi(h, MOUTH_BAR_H_MIN, MOUTH_BAR_H_MAX);
        lv_obj_set_height(mouth_bars_[i], h);
        lv_obj_set_y(mouth_bars_[i], MOUTH_Y + (MOUTH_BAR_H_MAX - h) / 2);
    }
}

// ── Sensor widgets ──────────────────────────────────────────────────────────
void HudLcdDisplay::UpdateSensorWidgets() {
    if (!temp_label_) return;
    sensor_snapshot_t s = sensor_state_get();
    char buf[24];

    if (s.dock_present && s.humiture_valid) {
        std::snprintf(buf, sizeof(buf), "RM %.0fF", (double)s.temperature_f);
        lv_label_set_text(temp_label_, buf);
        std::snprintf(buf, sizeof(buf), "H %.0f%%", (double)s.humidity_percent);
        lv_label_set_text(hum_label_, buf);
    } else {
        lv_label_set_text(temp_label_, "RM --F");
        lv_label_set_text(hum_label_,  "H --%");
    }

    if (s.dock_present) {
        if (s.radar_presence) {
            lv_label_set_text(occ_label_, "OCC *");
            lv_obj_set_style_text_color(occ_label_, C_SPEAK, 0);
        } else {
            lv_label_set_text(occ_label_, "OCC -");
            lv_obj_set_style_text_color(occ_label_, C_HUD_DIM, 0);
        }
    } else {
        lv_label_set_text(occ_label_, "OCC ?");
        lv_obj_set_style_text_color(occ_label_, C_SCAN, 0);
    }

    // Update clock too — uses local time configured by SNTP elsewhere.
    if (time_label_) {
        time_t now;
        time(&now);
        if (now > 1700000000) {  // sane epoch
            struct tm tm_local;
            localtime_r(&now, &tm_local);
            char tbuf[16];
            std::strftime(tbuf, sizeof(tbuf), "%H:%M", &tm_local);
            lv_label_set_text(time_label_, tbuf);
        }
    }
}

// ── Weather widgets ─────────────────────────────────────────────────────────
void HudLcdDisplay::UpdateWeatherWidgets() {
    if (!weather_label_) return;
    weather_snapshot_t w = weather_state_get();
    char buf[40];
    if (w.valid) {
        std::snprintf(buf, sizeof(buf), "OUT %.0fF", (double)w.temp_f);
        lv_label_set_text(weather_label_, buf);
        // Capitalised condition + location, e.g. "Clear sky · Winnsboro"
        char cond[40];
        std::strncpy(cond, w.condition, sizeof(cond) - 1);
        cond[sizeof(cond) - 1] = '\0';
        if (cond[0] >= 'a' && cond[0] <= 'z') cond[0] -= 32;
        char combined[96];
        if (w.location[0]) {
            std::snprintf(combined, sizeof(combined), "%.31s  -  %.31s", cond, w.location);
        } else {
            std::snprintf(combined, sizeof(combined), "%.63s", cond);
        }
        lv_label_set_text(loc_label_, combined);
    } else {
        lv_label_set_text(weather_label_, "OUT --F");
        lv_label_set_text(loc_label_,     "");
    }
}

// ── Public emotion / status / chat hooks ───────────────────────────────────
// These run on the application/main task; LVGL callbacks run on the LVGL task.
// Take DisplayLockGuard before any lv_* API or LVGL state corrupts and lv_inv_area spins.
void HudLcdDisplay::SetEmotion(const char* emotion) {
    if (!emotion) return;
    DisplayLockGuard lock(this);
    if (!std::strcmp(emotion, "listening")) {
        SetHudState(HudState::LISTENING);
        if (top_label_) lv_label_set_text(top_label_, "LISTENING");
    } else if (!std::strcmp(emotion, "speaking") || !std::strcmp(emotion, "thinking")) {
        SetHudState(HudState::SPEAKING);
        if (top_label_) lv_label_set_text(top_label_, "SPEAKING");
    } else {
        SetHudState(HudState::IDLE);
        if (top_label_) lv_label_set_text(top_label_, "JARVIS ONLINE");
    }
}

void HudLcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!content || !bot_label_) return;
    DisplayLockGuard lock(this);
    lv_label_set_text(bot_label_, content);
    if (role && !std::strcmp(role, "assistant")) {
        SetHudState(HudState::SPEAKING);
    } else if (role && !std::strcmp(role, "user")) {
        SetHudState(HudState::LISTENING);
    }
}

void HudLcdDisplay::SetStatus(const char* status) {
    if (!status || !top_label_) return;
    DisplayLockGuard lock(this);
    lv_label_set_text(top_label_, status);
}
