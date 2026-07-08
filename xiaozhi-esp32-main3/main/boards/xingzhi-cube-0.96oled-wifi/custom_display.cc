#include "custom_display.h"
#include "settings.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "assets/lang_config.h"

#include <font_awesome.h>
#include <esp_log.h>
#include <cstring>

#define TAG "CustomDisplay"

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
extern "C" {
    LV_IMAGE_DECLARE(bg_image);
}

// Festive color palette
#define COLOR_BG       lv_color_hex(0xFF0000)  // 全红背景
#define COLOR_GOLD     lv_color_hex(0xFFD700)  // gold
#define COLOR_LIGHT    lv_color_hex(0xFFE4B5)  // moccasin
#define COLOR_WHITE    lv_color_hex(0xFFFFFF)
#define COLOR_BLACK    lv_color_hex(0x000000)

static lv_obj_t* create_rotated_info_label(lv_obj_t* parent, const char* text, int center_x) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_size(box, 76, 92);
    lv_obj_set_pos(box, center_x - 38, -8);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_transform_pivot_x(box, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(box, LV_PCT(50), 0);
    lv_obj_set_style_transform_width(box, 12, 0);
    lv_obj_set_style_transform_height(box, 12, 0);
    lv_obj_set_style_transform_rotation(box, -900, 0);

    lv_obj_t* label = lv_label_create(box);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(label, 76);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(label, COLOR_WHITE, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(label, 1, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    return label;
}

// Status icon mappings
static const char* status_to_icon(const char* status) {
    if (strstr(status, Lang::Strings::LISTENING)) return FONT_AWESOME_MICROPHONE;
    if (strstr(status, Lang::Strings::SPEAKING)) return FONT_AWESOME_VOLUME_HIGH;
    if (strstr(status, Lang::Strings::CONNECTING)) return FONT_AWESOME_WIFI;
    if (strstr(status, Lang::Strings::STANDBY)) return FONT_AWESOME_CHECK;
    return FONT_AWESOME_MICROCHIP_AI;
}

CustomDisplay::CustomDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy) {
}

void CustomDisplay::CreateMainScreen() {
    main_screen_ = lv_obj_create(NULL);
    lv_obj_clear_flag(main_screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(main_screen_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_color(main_screen_, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(main_screen_, LV_OPA_COVER, 0);

    lv_obj_t* bg_img = lv_image_create(main_screen_);
    lv_image_set_src(bg_img, &bg_image);
    lv_image_set_pivot(bg_img, LV_PCT(50), LV_PCT(50));
    lv_image_set_rotation(bg_img, 1800);
    lv_image_set_antialias(bg_img, false);
    lv_obj_center(bg_img);

    // 状态文字
    status_icon_ = nullptr;
    status_text_ = lv_label_create(main_screen_);
    lv_obj_set_style_text_color(status_text_, COLOR_LIGHT, 0);
    lv_label_set_text(status_text_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_text_, LV_ALIGN_BOTTOM_MID, 0, -2);

    // LVGL timer: apply queued UI changes in LVGL context.
    power_save_check_timer_ = lv_timer_create([](lv_timer_t* timer) {
        auto* self = (CustomDisplay*)lv_timer_get_user_data(timer);
        self->ProcessUiRequests();
    }, 50, this);
}

void CustomDisplay::CreateInfoScreen() {
    info_screen_ = lv_obj_create(NULL);
    lv_obj_clear_flag(info_screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(info_screen_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_color(info_screen_, COLOR_BLACK, 0);
    lv_obj_set_style_bg_opa(info_screen_, LV_OPA_COVER, 0);

    temp_label_ = create_rotated_info_label(info_screen_, "温度\n--.-°C", 47);
    hum_label_ = create_rotated_info_label(info_screen_, "湿度\n--.-%", 142);
    air_label_ = create_rotated_info_label(info_screen_, "空气\n---PPM\n等待", 237);

    air_quality_label_ = nullptr;
}

void CustomDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping");
        return;
    }
    setup_ui_called_ = true;

    CreateMainScreen();
    CreateInfoScreen();

    // Show main screen by default
    lv_screen_load(main_screen_);
    current_screen_ = 0;

    ESP_LOGI(TAG, "Custom UI setup complete");
}

void CustomDisplay::SetTheme(Theme* theme) {
    // Custom UI: skip base class SetTheme which accesses UI objects we don't create
    Display::SetTheme(theme);  // Only save theme to settings
}

void CustomDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    if (status_icon_) {
        lv_label_set_text(status_icon_, status_to_icon(status));
    }
    if (status_text_) {
        lv_label_set_text(status_text_, status);
    }
}

void CustomDisplay::SetEmotion(const char* emotion) {
    // Not using emoji in this custom UI
}

void CustomDisplay::SetChatMessage(const char* role, const char* content) {
    if (content && strlen(content) > 0 &&
        (strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0)) {
        SetStatus(content);
    }
}

void CustomDisplay::ClearChatMessages() {
    // No chat messages in custom UI
}

void CustomDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    // No preview image in custom UI
}

void CustomDisplay::ShowNotification(const char* notification, int duration_ms) {
    // Route volume/network notifications to status text
    if (notification && strlen(notification) > 0) {
        SetStatus(notification);
    }
}

void CustomDisplay::UpdateStatusBar(bool update_all) {
    // No status bar icons in custom UI
}

void CustomDisplay::SetPowerSaveMode(bool on) {
    // Defer to LVGL task context (safe to call from any task)
    power_save_target_.store(on);
    power_save_requested_.store(true);
}

void CustomDisplay::RequestSwitchToNextScreen() {
    switch_screen_requested_.store(true);
}

void CustomDisplay::ProcessUiRequests() {
    ApplyPowerSave();

    if (switch_screen_requested_.exchange(false)) {
        SwitchToNextScreen();
    }
}

void CustomDisplay::ApplyPowerSave() {
    // Runs in LVGL task context — safe to call LVGL APIs
    if (!power_save_requested_.exchange(false)) return;

    // Keep the active screen object stable; board-level backlight handles dimming.
    if (!power_save_target_.load()) {
        if (main_screen_) lv_obj_remove_flag(main_screen_, LV_OBJ_FLAG_HIDDEN);
        if (info_screen_) lv_obj_remove_flag(info_screen_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CustomDisplay::SwitchToNextScreen() {
    current_screen_ = (current_screen_ + 1) % 2;
    switch (current_screen_) {
        case 0: lv_screen_load(main_screen_); break;
        case 1: lv_screen_load(info_screen_); break;
    }
    ESP_LOGI(TAG, "Switched to screen %d", current_screen_);
}

void CustomDisplay::UpdateTemperature(float temp, float humidity) {
    DisplayLockGuard lock(this);
    if (temp_label_) {
        char buf[32];
        snprintf(buf, sizeof(buf), "温度\n%.1f°C", temp);
        lv_label_set_text(temp_label_, buf);
        lv_obj_center(temp_label_);
    }
    if (hum_label_) {
        char buf[32];
        snprintf(buf, sizeof(buf), "湿度\n%.1f%%", humidity);
        lv_label_set_text(hum_label_, buf);
        lv_obj_center(hum_label_);
    }
}

void CustomDisplay::UpdateAirQuality(float ppm, const char* quality) {
    DisplayLockGuard lock(this);
    if (air_label_) {
        char buf[48];
        snprintf(buf, sizeof(buf), "空气\n%.0fPPM\n%s", ppm, quality);
        lv_label_set_text(air_label_, buf);
        lv_obj_center(air_label_);
    }
}
