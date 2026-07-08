#ifndef CUSTOM_DISPLAY_H
#define CUSTOM_DISPLAY_H

#include "display/lcd_display.h"

#include <atomic>

class CustomDisplay : public SpiLcdDisplay {
private:
    lv_obj_t* main_screen_ = nullptr;
    lv_obj_t* info_screen_ = nullptr;

    // Main screen
    lv_obj_t* status_icon_ = nullptr;
    lv_obj_t* status_text_ = nullptr;

    // Info screen (temperature + humidity + air quality, rotated 90°)
    lv_obj_t* temp_label_ = nullptr;
    lv_obj_t* hum_label_ = nullptr;
    lv_obj_t* air_label_ = nullptr;
    lv_obj_t* air_quality_label_ = nullptr;

    int current_screen_ = 0;

    void CreateMainScreen();
    void CreateInfoScreen();
    void ProcessUiRequests();
    void ApplyPowerSave();
    void SwitchToNextScreen();

public:
    CustomDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);

    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetPowerSaveMode(bool on) override;

    void RequestSwitchToNextScreen();
    void UpdateTemperature(float temp, float humidity);
    void UpdateAirQuality(float ppm, const char* quality);

private:
    // UI changes requested by other tasks are applied in LVGL timer context.
    std::atomic<bool> power_save_requested_ = false;
    std::atomic<bool> power_save_target_ = false;
    std::atomic<bool> switch_screen_requested_ = false;
    lv_timer_t* power_save_check_timer_ = nullptr;
};

#endif
