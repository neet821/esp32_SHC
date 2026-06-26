#pragma once

#include "brookesia/system_phone/app.hpp"
#include "lvgl.h"

namespace esp_brookesia::apps {

class AudioControl: public systems::phone::App {
public:
    AudioControl();
    ~AudioControl() override;

private:
    bool run() override;
    bool back() override;
    bool close() override;

    static void onRecordClicked(lv_event_t *event);
    static void onPlayRecordingClicked(lv_event_t *event);
    static void onFileClicked(lv_event_t *event);
    static void onToneClicked(lv_event_t *event);
    static void onVolumeChanged(lv_event_t *event);
    static void onRefreshTimer(lv_timer_t *timer);

    void refreshLabels();

    lv_obj_t *_record_label = nullptr;
    lv_obj_t *_play_recording_label = nullptr;
    lv_obj_t *_file_label = nullptr;
    lv_obj_t *_tone_label = nullptr;
    lv_obj_t *_volume_label = nullptr;
    lv_obj_t *_volume_slider = nullptr;
    lv_obj_t *_mic_status_label = nullptr;
    lv_obj_t *_mic_level_bar = nullptr;
};

} // namespace esp_brookesia::apps
