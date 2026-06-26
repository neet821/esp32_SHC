#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "App:AudioControl"

#include <algorithm>

#include "audio_control_app.hpp"
#include "audio_self_test.hpp"
#include "esp_lib_utils.h"

namespace esp_brookesia::apps {

namespace {

constexpr char APP_NAME[] = "Audio Test";

constexpr systems::base::App::Config CORE_DATA = {
    .name = APP_NAME,
    .launcher_icon = {},
    .screen_size = gui::StyleSize::RECT_PERCENT(100, 100),
    .flags = {
        .enable_default_screen = 1,
        .enable_recycle_resource = 1,
        .enable_resize_visual_area = 1,
    },
};

constexpr systems::phone::App::Config APP_DATA = systems::phone::App::Config::SIMPLE_CONSTRUCTOR(nullptr, true, true);

lv_obj_t *create_button(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                        lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 104, 34);
    lv_obj_align(button, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_radius(button, 7, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2563eb), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1d4ed8), LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);
    return label;
}

} // namespace

AudioControl::AudioControl()
    : App(CORE_DATA, APP_DATA)
{
}

AudioControl::~AudioControl() = default;

bool AudioControl::run()
{
    audio_self_test_init();

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x111827), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Audio Test");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    _record_label = create_button(screen, "Record 3s", -55, 36, onRecordClicked, this);
    _play_recording_label = create_button(screen, "Play Rec", 55, 36, onPlayRecordingClicked, this);
    _file_label = create_button(screen, "Play File", -55, 78, onFileClicked, this);
    _tone_label = create_button(screen, "Play Tone", 55, 78, onToneClicked, this);

    _volume_label = lv_label_create(screen);
    lv_obj_set_width(_volume_label, 218);
    lv_obj_align(_volume_label, LV_ALIGN_TOP_MID, 0, 124);
    lv_obj_set_style_text_font(_volume_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_volume_label, lv_color_hex(0xe5e7eb), 0);
    lv_obj_set_style_text_align(_volume_label, LV_TEXT_ALIGN_CENTER, 0);

    _volume_slider = lv_slider_create(screen);
    lv_obj_set_size(_volume_slider, 200, 18);
    lv_obj_align(_volume_slider, LV_ALIGN_TOP_MID, 0, 148);
    lv_slider_set_range(_volume_slider, 0, 100);
    lv_slider_set_value(_volume_slider, audio_self_test_get_volume_percent(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_volume_slider, lv_color_hex(0x374151), 0);
    lv_obj_set_style_bg_color(_volume_slider, lv_color_hex(0x38bdf8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_volume_slider, lv_color_hex(0xf9fafb), LV_PART_KNOB);
    lv_obj_add_event_cb(_volume_slider, onVolumeChanged, LV_EVENT_VALUE_CHANGED, this);

    _mic_status_label = lv_label_create(screen);
    lv_obj_set_width(_mic_status_label, 218);
    lv_obj_align(_mic_status_label, LV_ALIGN_TOP_MID, 0, 184);
    lv_obj_set_style_text_font(_mic_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_mic_status_label, lv_color_hex(0xd1d5db), 0);
    lv_obj_set_style_text_align(_mic_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_mic_status_label, LV_LABEL_LONG_WRAP);

    _mic_level_bar = lv_bar_create(screen);
    lv_obj_set_size(_mic_level_bar, 200, 18);
    lv_obj_align(_mic_level_bar, LV_ALIGN_TOP_MID, 0, 252);
    lv_bar_set_range(_mic_level_bar, 0, 900);
    lv_bar_set_value(_mic_level_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(_mic_level_bar, 4, 0);
    lv_obj_set_style_bg_color(_mic_level_bar, lv_color_hex(0x374151), 0);
    lv_obj_set_style_bg_color(_mic_level_bar, lv_color_hex(0x22c55e), LV_PART_INDICATOR);

    lv_timer_create(onRefreshTimer, 300, this);
    refreshLabels();

    return true;
}

bool AudioControl::back()
{
    audio_self_test_stop_mic_monitor();
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool AudioControl::close()
{
    audio_self_test_stop_mic_monitor();
    return true;
}

void AudioControl::onRecordClicked(lv_event_t *event)
{
    auto *self = static_cast<AudioControl *>(lv_event_get_user_data(event));
    if (self == nullptr) {
        return;
    }
    audio_self_test_record_microphone();
    self->refreshLabels();
}

void AudioControl::onPlayRecordingClicked(lv_event_t *event)
{
    auto *self = static_cast<AudioControl *>(lv_event_get_user_data(event));
    if (self == nullptr) {
        return;
    }
    audio_self_test_play_recording();
    self->refreshLabels();
}

void AudioControl::onFileClicked(lv_event_t *event)
{
    auto *self = static_cast<AudioControl *>(lv_event_get_user_data(event));
    if (self == nullptr) {
        return;
    }
    audio_self_test_play_custom_file();
    self->refreshLabels();
}

void AudioControl::onToneClicked(lv_event_t *event)
{
    auto *self = static_cast<AudioControl *>(lv_event_get_user_data(event));
    if (self == nullptr) {
        return;
    }
    audio_self_test_play_audio_clip();
    self->refreshLabels();
}

void AudioControl::onVolumeChanged(lv_event_t *event)
{
    auto *self = static_cast<AudioControl *>(lv_event_get_user_data(event));
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(event));
    if (self == nullptr || slider == nullptr) {
        return;
    }
    audio_self_test_set_volume_percent(static_cast<int>(lv_slider_get_value(slider)));
    self->refreshLabels();
}

void AudioControl::onRefreshTimer(lv_timer_t *timer)
{
    auto *self = static_cast<AudioControl *>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->refreshLabels();
    }
}

void AudioControl::refreshLabels()
{
    const bool audio_busy = audio_self_test_is_audio_busy();
    const bool recording_busy = audio_self_test_is_recording_busy();
    const bool has_recording = audio_self_test_has_recording();
    const int volume = audio_self_test_get_volume_percent();

    if (_record_label != nullptr) {
        lv_label_set_text(_record_label, recording_busy ? "Recording" : "Record 3s");
    }

    if (_play_recording_label != nullptr) {
        lv_label_set_text(_play_recording_label, has_recording ? "Play Rec" : "No Rec");
    }

    if (_file_label != nullptr) {
        lv_label_set_text(_file_label, audio_busy ? "Playing" : "Play File");
    }

    if (_tone_label != nullptr) {
        lv_label_set_text(_tone_label, audio_busy ? "Playing" : "Play Tone");
    }

    if (_volume_label != nullptr) {
        lv_label_set_text_fmt(_volume_label, "Speaker Volume  %d%%", volume);
    }

    if (_volume_slider != nullptr && lv_slider_get_value(_volume_slider) != volume) {
        lv_slider_set_value(_volume_slider, volume, LV_ANIM_OFF);
    }

    const audio_mic_stats_t stats = audio_self_test_get_mic_stats();
    if (_mic_status_label != nullptr) {
        if (recording_busy) {
            lv_label_set_text_fmt(
                _mic_status_label,
                "Recording 3 seconds\navg %d  range %d",
                stats.average,
                stats.range
            );
        } else if (audio_busy) {
            lv_label_set_text(_mic_status_label, "Playing through speaker");
        } else if (has_recording) {
            lv_label_set_text_fmt(
                _mic_status_label,
                "Recording ready\navg %d  range %d\nmin %d  max %d",
                stats.average,
                stats.range,
                stats.minimum,
                stats.maximum
            );
        } else {
            lv_label_set_text(_mic_status_label, "No recording yet\npress Record 3s and speak");
        }
    }

    if (_mic_level_bar != nullptr) {
        lv_bar_set_value(_mic_level_bar, std::clamp(stats.range, 0, 900), LV_ANIM_ON);
    }
}

ESP_UTILS_REGISTER_PLUGIN(systems::base::App, AudioControl, APP_NAME)

} // namespace esp_brookesia::apps
