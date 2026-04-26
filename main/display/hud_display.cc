#include "hud_display.h"
#include <esp_log.h>
#include <cstring>

#define TAG "HudDisplay"

// ── Layout ──────────────────────────────────────────────────────────────────
// Display: 320 × 240, landscape
#define SCREEN_W  320
#define SCREEN_H  240

#define TOP_H     22   // top status bar height
#define BOT_H     32   // bottom label height

// Eye geometry (both eyes same size)
#define EYE_W     88
#define EYE_H     54
#define EYE_R     7    // corner radius (hex-ish look)
#define EYE_BDR   3    // border width px
#define EYE_CY    (TOP_H + (SCREEN_H - TOP_H - BOT_H) / 2 - 4)  // ~105

#define EYE_LEFT_CX   92
#define EYE_RIGHT_CX  228

// Iris (inner glow circle)
#define IRIS_D    28   // diameter
#define IRIS_OFS_X 0   // centre offset from eye centre
#define IRIS_OFS_Y 0

// Scan arc (rotating ring during LISTENING)
#define SCAN_D    46   // diameter of arc bounding box
#define SCAN_ARC_W  4  // arc stroke width

// ── Colour palette ──────────────────────────────────────────────────────────
#define C_BG      lv_color_hex(0x000000)  // black background
#define C_HUD     lv_color_hex(0x00CCFF)  // Jarvis cyan (primary)
#define C_SCAN    lv_color_hex(0xFF8800)  // orange accent for scan arcs
#define C_SPEAK   lv_color_hex(0x00FFCC)  // teal-white pulse when speaking
#define C_DIM     lv_color_hex(0x005A70)  // dimmed eye colour (idle)

// ── Animation constants ──────────────────────────────────────────────────────
#define ANIM_PERIOD_MS  50   // timer period → 20 fps
#define IDLE_BREATH_HALF 30  // ticks for half a breath cycle (1.5 s up, 1.5 s down)
#define IDLE_OPA_LO  60     // LV_OPA range 0-255
#define IDLE_OPA_HI  200
#define SCAN_STEP_DEG 15    // degrees the scan arc advances per tick
#define SCAN_SWEEP    90    // arc sweep in degrees

// ── Helpers ─────────────────────────────────────────────────────────────────
static inline int lerp(int a, int b, int t, int tmax) {
    return a + (b - a) * t / tmax;
}

// ────────────────────────────────────────────────────────────────────────────
HudLcdDisplay::HudLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_panel_handle_t panel,
                             int width, int height,
                             int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy)
{
    ESP_LOGI(TAG, "HUD display created (%dx%d)", width, height);
}

HudLcdDisplay::~HudLcdDisplay() {
    if (anim_timer_) {
        DisplayLockGuard lock(this);
        lv_timer_delete(anim_timer_);
        anim_timer_ = nullptr;
    }
}

// ── CreateEye ───────────────────────────────────────────────────────────────
// Builds one eye: outer frame + inner iris + scan arc (hidden until LISTENING).
void HudLcdDisplay::CreateEye(lv_obj_t* parent,
                               int cx, int cy,
                               lv_obj_t** frame_out,
                               lv_obj_t** iris_out,
                               lv_obj_t** scan_out)
{
    // Outer frame: rounded rectangle, border only (transparent fill)
    lv_obj_t* frame = lv_obj_create(parent);
    lv_obj_set_size(frame, EYE_W, EYE_H);
    lv_obj_set_pos(frame, cx - EYE_W / 2, cy - EYE_H / 2);
    lv_obj_set_style_radius(frame, EYE_R, 0);
    lv_obj_set_style_border_width(frame, EYE_BDR, 0);
    lv_obj_set_style_border_color(frame, C_HUD, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_set_scrollbar_mode(frame, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    *frame_out = frame;

    // Inner iris: filled circle centred in the eye frame
    lv_obj_t* iris = lv_obj_create(parent);
    lv_obj_set_size(iris, IRIS_D, IRIS_D);
    lv_obj_set_pos(iris, cx - IRIS_D / 2 + IRIS_OFS_X, cy - IRIS_D / 2 + IRIS_OFS_Y);
    lv_obj_set_style_radius(iris, IRIS_D / 2, 0);
    lv_obj_set_style_bg_color(iris, C_HUD, 0);
    lv_obj_set_style_bg_opa(iris, IDLE_OPA_LO, 0);
    lv_obj_set_style_border_width(iris, 0, 0);
    lv_obj_clear_flag(iris, LV_OBJ_FLAG_SCROLLABLE);
    *iris_out = iris;

    // Scan arc: centred on the iris, hidden until LISTENING
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, SCAN_D, SCAN_D);
    lv_obj_set_pos(arc, cx - SCAN_D / 2, cy - SCAN_D / 2);
    // Remove knob and background arc
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);  // hide value indicator
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_set_style_arc_color(arc, C_BG, LV_PART_MAIN);  // bg arc invisible
    lv_obj_set_style_arc_width(arc, SCAN_ARC_W, LV_PART_MAIN);
    // Use indicator for the visible sweep
    lv_arc_set_angles(arc, 0, SCAN_SWEEP);
    lv_obj_set_style_arc_color(arc, C_SCAN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, SCAN_ARC_W, LV_PART_INDICATOR);
    lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    *scan_out = arc;
}

// ── SetupUI ──────────────────────────────────────────────────────────────────
void HudLcdDisplay::SetupUI() {
    if (setup_ui_called_) return;
    setup_ui_called_ = true;

    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();

    // Black background
    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // ── Top label (status) ──────────────────────────────────────────────────
    top_label_ = lv_label_create(screen);
    lv_obj_set_size(top_label_, SCREEN_W, TOP_H);
    lv_obj_set_pos(top_label_, 0, 0);
    lv_obj_set_style_text_color(top_label_, C_HUD, 0);
    lv_obj_set_style_bg_opa(top_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_align(top_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(top_label_, "JARVIS ONLINE");

    // ── Horizontal rule under top bar ──────────────────────────────────────
    lv_obj_t* hline = lv_obj_create(screen);
    lv_obj_set_size(hline, SCREEN_W, 1);
    lv_obj_set_pos(hline, 0, TOP_H);
    lv_obj_set_style_bg_color(hline, C_HUD, 0);
    lv_obj_set_style_bg_opa(hline, LV_OPA_40, 0);
    lv_obj_set_style_border_width(hline, 0, 0);
    lv_obj_clear_flag(hline, LV_OBJ_FLAG_SCROLLABLE);

    // ── Eyes ──────────────────────────────────────────────────────────────
    CreateEye(screen, EYE_LEFT_CX,  EYE_CY, &eye_left_frame_,  &iris_left_,  &scan_left_);
    CreateEye(screen, EYE_RIGHT_CX, EYE_CY, &eye_right_frame_, &iris_right_, &scan_right_);

    // ── Horizontal rule above bottom bar ───────────────────────────────────
    lv_obj_t* bline = lv_obj_create(screen);
    lv_obj_set_size(bline, SCREEN_W, 1);
    lv_obj_set_pos(bline, 0, SCREEN_H - BOT_H - 1);
    lv_obj_set_style_bg_color(bline, C_HUD, 0);
    lv_obj_set_style_bg_opa(bline, LV_OPA_40, 0);
    lv_obj_set_style_border_width(bline, 0, 0);
    lv_obj_clear_flag(bline, LV_OBJ_FLAG_SCROLLABLE);

    // ── Bottom label (chat message / status) ───────────────────────────────
    bot_label_ = lv_label_create(screen);
    lv_obj_set_size(bot_label_, SCREEN_W - 8, BOT_H);
    lv_obj_set_pos(bot_label_, 4, SCREEN_H - BOT_H);
    lv_obj_set_style_text_color(bot_label_, C_HUD, 0);
    lv_obj_set_style_bg_opa(bot_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_align(bot_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(bot_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(bot_label_, "Ready");

    // ── Hidden parent-class labels (prevent crash in theme refresh) ────────
    // The base LcdDisplay class assumes these objects exist. Create them
    // hidden so parent code that iterates them does not crash.
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
    emoji_label_      = lv_label_create(content_);
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
    lv_label_set_text(emoji_label_,      "");

    // ── Animation timer ────────────────────────────────────────────────────
    anim_timer_ = lv_timer_create(OnAnimTimer, ANIM_PERIOD_MS, this);
}

// ── Animation timer callback ─────────────────────────────────────────────────
void HudLcdDisplay::OnAnimTimer(lv_timer_t* t) {
    auto* self = static_cast<HudLcdDisplay*>(lv_timer_get_user_data(t));
    self->StepAnimation();
}

void HudLcdDisplay::StepAnimation() {
    anim_tick_++;

    switch (hud_state_) {

    case HudState::IDLE: {
        // Breathing: iris opacity oscillates IDLE_OPA_LO ↔ IDLE_OPA_HI
        int phase = anim_tick_ % (IDLE_BREATH_HALF * 2);
        lv_opa_t opa;
        if (phase < IDLE_BREATH_HALF) {
            opa = lerp(IDLE_OPA_LO, IDLE_OPA_HI, phase, IDLE_BREATH_HALF);
        } else {
            opa = lerp(IDLE_OPA_HI, IDLE_OPA_LO, phase - IDLE_BREATH_HALF, IDLE_BREATH_HALF);
        }
        lv_obj_set_style_bg_opa(iris_left_,  opa, 0);
        lv_obj_set_style_bg_opa(iris_right_, opa, 0);
        break;
    }

    case HudState::LISTENING: {
        // Rotating scan arc
        scan_angle_ = (scan_angle_ + SCAN_STEP_DEG) % 360;
        int start = scan_angle_;
        int end   = (scan_angle_ + SCAN_SWEEP) % 360;
        lv_arc_set_angles(scan_left_,  start, end);
        lv_arc_set_angles(scan_right_, start, end);
        // Keep iris at full brightness
        lv_obj_set_style_bg_opa(iris_left_,  LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(iris_right_, LV_OPA_COVER, 0);
        break;
    }

    case HudState::SPEAKING: {
        // Fast pulse between cyan and teal-white
        int phase = anim_tick_ % 10;
        lv_opa_t opa = (phase < 5)
            ? lerp(180, 255, phase, 5)
            : lerp(255, 180, phase - 5, 5);
        lv_obj_set_style_bg_opa(iris_left_,  opa, 0);
        lv_obj_set_style_bg_opa(iris_right_, opa, 0);
        break;
    }
    }
}

// ── State transitions ─────────────────────────────────────────────────────────
void HudLcdDisplay::SetHudState(HudState state) {
    if (hud_state_ == state) return;
    hud_state_ = state;
    anim_tick_ = 0;

    const bool show_scan = (state == HudState::LISTENING);
    lv_obj_set_style_border_color(eye_left_frame_,  state == HudState::SPEAKING ? C_SPEAK : C_HUD, 0);
    lv_obj_set_style_border_color(eye_right_frame_, state == HudState::SPEAKING ? C_SPEAK : C_HUD, 0);
    lv_obj_set_style_bg_color(iris_left_,  state == HudState::SPEAKING ? C_SPEAK : C_HUD, 0);
    lv_obj_set_style_bg_color(iris_right_, state == HudState::SPEAKING ? C_SPEAK : C_HUD, 0);

    if (show_scan) {
        lv_obj_clear_flag(scan_left_,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(scan_right_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(scan_left_,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scan_right_, LV_OBJ_FLAG_HIDDEN);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void HudLcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_ || !iris_left_) return;
    DisplayLockGuard lock(this);

    if (strcmp(emotion, "listening") == 0) {
        SetHudState(HudState::LISTENING);
        if (bot_label_) lv_label_set_text(bot_label_, "Listening...");
    } else if (strcmp(emotion, "speaking") == 0 ||
               strcmp(emotion, "thinking") == 0) {
        SetHudState(HudState::SPEAKING);
    } else {
        SetHudState(HudState::IDLE);
    }
}

void HudLcdDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_ || !top_label_) return;
    DisplayLockGuard lock(this);
    lv_label_set_text(top_label_, status ? status : "JARVIS ONLINE");
}

void HudLcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_ || !bot_label_) return;
    DisplayLockGuard lock(this);

    if (content && *content) {
        lv_label_set_text(bot_label_, content);
    }

    // Transition to speaking on assistant message
    if (role && strcmp(role, "assistant") == 0) {
        SetHudState(HudState::SPEAKING);
    } else if (role && strcmp(role, "user") == 0) {
        SetHudState(HudState::LISTENING);
    } else {
        SetHudState(HudState::IDLE);
    }
}
