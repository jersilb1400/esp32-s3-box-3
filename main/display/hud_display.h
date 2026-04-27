#pragma once

#include "lcd_display.h"
#include <lvgl.h>
#include <string>

// Iron Man HUD-style face for ESP-BOX-3 (320x240 ILI9341).
//
// Layout:
//   Top status strip   (time, "JARVIS ONLINE", connection)
//   Left metrics col   (room temp °F, humidity %, weather temp °F, condition)
//   Center eye pair    (animated per voice state)
//   Animated mouth     (waveform during SPEAKING)
//   Right metrics col  (occupancy ✓/✕, weather location)
//   Bottom chat label  (assistant message / status)
//
// Boot sequence:
//   Green Matrix-rain LVGL canvas plays for ~2.5s on first SetupUI() call,
//   then fades out and the HUD assembles in.

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
    int       scan_angle_   = 0;
    bool      pulse_up_     = true;
    int       sensor_tick_  = 0;   // throttle sensor poll (1Hz)
    int       weather_tick_ = 0;   // throttle weather poll (every 5s)
    int       mouth_phase_  = 0;   // running counter for mouth waveform

    // Eyes
    lv_obj_t* eye_left_frame_  = nullptr;
    lv_obj_t* eye_right_frame_ = nullptr;
    lv_obj_t* iris_left_       = nullptr;
    lv_obj_t* iris_right_      = nullptr;
    lv_obj_t* scan_left_       = nullptr;
    lv_obj_t* scan_right_      = nullptr;

    // Status strip
    lv_obj_t* top_label_       = nullptr;
    lv_obj_t* time_label_      = nullptr;
    lv_obj_t* bot_label_       = nullptr;

    // Sensor / weather widgets
    lv_obj_t* temp_label_      = nullptr;   // room temp
    lv_obj_t* hum_label_       = nullptr;
    lv_obj_t* weather_label_   = nullptr;   // outdoor temp + condition
    lv_obj_t* loc_label_       = nullptr;   // weather location
    lv_obj_t* occ_label_       = nullptr;   // occupancy ✓/✕

    // Animated mouth (waveform bars)
    static constexpr int kMouthBars = 9;
    lv_obj_t* mouth_bars_[kMouthBars] = {nullptr};

    // Boot sequence canvas (matrix rain) — present until first eye assembly.
    lv_obj_t*       boot_canvas_   = nullptr;
    lv_color_t*     boot_buf_      = nullptr;
    lv_timer_t*     boot_timer_    = nullptr;
    int             boot_tick_     = 0;
    int*            rain_y_        = nullptr;   // per-column current y offset
    int             rain_cols_     = 0;
    bool            booted_        = false;     // true once we've revealed the HUD

    // Animation timer (50ms / 20fps)
    lv_timer_t* anim_timer_ = nullptr;

    void CreateEye(lv_obj_t* parent, int cx, int cy,
                   lv_obj_t** frame_out, lv_obj_t** iris_out, lv_obj_t** scan_out);
    void SetHudState(HudState state);
    void StepAnimation();
    void UpdateSensorWidgets();
    void UpdateWeatherWidgets();
    void StepMouthAnimation();

    void StartBootSequence();
    void StepBootRain();
    void RevealHud();
    void BuildHudUi();   // creates all the live HUD elements (post-boot)

    static void OnAnimTimer(lv_timer_t* t);
    static void OnBootTimer(lv_timer_t* t);
};
