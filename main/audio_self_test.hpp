#pragma once

struct audio_mic_stats_t {
    int average;
    int minimum;
    int maximum;
    int range;
    unsigned int sequence;
};

bool audio_self_test_init();
bool audio_self_test_play_audio_clip();
bool audio_self_test_play_custom_file();
bool audio_self_test_record_microphone();
bool audio_self_test_play_recording();
bool audio_self_test_play_speaker();
bool audio_self_test_start_mic_monitor();
void audio_self_test_stop_mic_monitor();
void audio_self_test_set_volume_percent(int volume);
int audio_self_test_get_volume_percent();
bool audio_self_test_is_mic_monitor_running();
bool audio_self_test_is_audio_busy();
bool audio_self_test_is_recording_busy();
bool audio_self_test_is_speaker_busy();
bool audio_self_test_has_recording();
audio_mic_stats_t audio_self_test_get_mic_stats();
