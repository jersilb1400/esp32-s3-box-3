#pragma once

#include "lcd_display.h"
#include <lvgl.h>
#include <string>

// Iron Man HUD-style face display for ESP-BOX-3 (320x240 ILI9341).
// Replaces the default chat bubble UI with two geometric eye shapes,
// animated per voice state (idle breath, listening scan, speaking pulse).
class HudLcdDisplay : public SpiLcdDisplay {
public:
    HudLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
    ~HudLcdDisplay();

    virtual void SetupUI() override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetStatus(const char* status) override;

private:
    enum class HudState { IDLE, LISTENING, SPEAKING };

    HudState  hud_state_    = HudState::IDLE;
    int       anim_tick_    = 0;
    int       scan_angle_   = 0;   // current scan arc rotation
    bool      pulse_up_     = true; // iris brightness direction

    lv_obj_t* eye_left_frame_  = nullptr;
    lv_obj_t* eye_right_frame_ = nullptr;
    lv_obj_t* iris_left_       = nullptr;
    lv_obj_t* iris_right_      = nullptr;
    lv_obj_t* scan_left_       = nullptr;
    lv_obj_t* scan_right_      = nullptr;
    lv_obj_t* top_label_       = nullptr;
    lv_obj_t* bot_label_       = nullptr;

    lv_timer_t* anim_timer_ = nullptr;

    void CreateEye(lv_obj_t* parent, int cx, int cy,
                   lv_obj_t** frame_out, lv_obj_t** iris_out, lv_obj_t** scan_out);
    void SetHudState(HudState state);
    void StepAnimation();

    static void OnAnimTimer(lv_timer_t* t);
};
