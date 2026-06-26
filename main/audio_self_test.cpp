/*
 * Audio hardware helpers for ESP-SensairShuttle.
 *
 * MIC is routed to GPIO6 / ADC channel 5. SPK uses GPIO7 and GPIO8 as a
 * differential PDM signal, with GPIO1 enabling the speaker amplifier.
 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <math.h>
#include <stdlib.h>

#include "audio_self_test.hpp"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_sig_map.h"

extern const uint8_t custom_audio_wav_start[] asm("_binary_custom_audio_wav_start");
extern const uint8_t custom_audio_wav_end[] asm("_binary_custom_audio_wav_end");

namespace {

constexpr char TAG[] = "audio";

constexpr gpio_num_t PA_CTL_GPIO = GPIO_NUM_1;
constexpr gpio_num_t SPK_P_GPIO = GPIO_NUM_7;
constexpr gpio_num_t SPK_N_GPIO = GPIO_NUM_8;
constexpr adc_channel_t MIC_ADC_CHANNEL = ADC_CHANNEL_5;

constexpr int AUDIO_SAMPLE_RATE = 16000;
constexpr int RECORD_SECONDS = 3;
constexpr int RECORD_SAMPLE_RATE = 16000;
constexpr int RECORD_SAMPLE_COUNT = RECORD_SECONDS * RECORD_SAMPLE_RATE;
constexpr float PI = 3.14159265358979323846f;
constexpr float AUDIO_TEST_MELODY[] = {523.25f, 659.25f, 783.99f, 1046.50f, 783.99f, 659.25f, 523.25f};

enum class PlaybackMode : intptr_t {
    Melody,
    CustomFile,
    Recording,
};

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_audio_busy{false};
std::atomic<bool> g_recording_busy{false};
std::atomic<bool> g_speaker_busy{false};
std::atomic<bool> g_mic_running{false};
std::atomic<unsigned int> g_mic_sequence{0};
std::atomic<int> g_mic_average{0};
std::atomic<int> g_mic_minimum{0};
std::atomic<int> g_mic_maximum{0};
std::atomic<int> g_mic_range{0};
std::atomic<int> g_volume_percent{70};

adc_oneshot_unit_handle_t g_adc_handle = nullptr;
TaskHandle_t g_mic_task_handle = nullptr;
int16_t *g_recording = nullptr;
size_t g_recording_samples = 0;

uint16_t read_le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int16_t apply_volume(int16_t sample)
{
    const int volume = std::clamp(g_volume_percent.load(), 0, 100);
    const int32_t scaled = static_cast<int32_t>(sample) * volume / 100;
    return static_cast<int16_t>(std::clamp<int32_t>(scaled, INT16_MIN, INT16_MAX));
}

void configure_speaker_gpio_for_bitbang()
{
    gpio_config_t speaker_cfg = {};
    speaker_cfg.pin_bit_mask = (1ULL << SPK_P_GPIO) | (1ULL << SPK_N_GPIO);
    speaker_cfg.mode = GPIO_MODE_OUTPUT;
    speaker_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    speaker_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    speaker_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&speaker_cfg));
    gpio_set_level(SPK_P_GPIO, 0);
    gpio_set_level(SPK_N_GPIO, 0);
}

void speaker_tone(int frequency_hz, int duration_ms)
{
    const int half_period_us = 1000000 / (frequency_hz * 2);
    const int cycles = frequency_hz * duration_ms / 1000;

    gpio_set_level(PA_CTL_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    for (int i = 0; i < cycles; ++i) {
        gpio_set_level(SPK_P_GPIO, 1);
        gpio_set_level(SPK_N_GPIO, 0);
        esp_rom_delay_us(half_period_us);
        gpio_set_level(SPK_P_GPIO, 0);
        gpio_set_level(SPK_N_GPIO, 1);
        esp_rom_delay_us(half_period_us);
    }

    gpio_set_level(SPK_P_GPIO, 0);
    gpio_set_level(SPK_N_GPIO, 0);
}

void speaker_task(void *)
{
    g_speaker_busy.store(true);
    configure_speaker_gpio_for_bitbang();
    speaker_tone(1200, 250);
    vTaskDelay(pdMS_TO_TICKS(120));
    speaker_tone(1600, 250);
    gpio_set_level(PA_CTL_GPIO, 0);
    g_speaker_busy.store(false);
    vTaskDelete(nullptr);
}

esp_err_t start_pdm_output(int sample_rate, i2s_chan_handle_t *tx_handle)
{
    *tx_handle = nullptr;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, tx_handle, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_pdm_tx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(sample_rate)),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = GPIO_NUM_NC,
            .dout = SPK_P_GPIO,
#if SOC_I2S_PDM_MAX_TX_LINES > 1
            .dout2 = GPIO_NUM_NC,
#endif
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    pdm_cfg.clk_cfg.up_sample_fs = 480;
    pdm_cfg.slot_cfg.sd_scale = I2S_PDM_SIG_SCALING_MUL_4;
    pdm_cfg.slot_cfg.hp_scale = I2S_PDM_SIG_SCALING_MUL_4;
    pdm_cfg.slot_cfg.lp_scale = I2S_PDM_SIG_SCALING_MUL_4;
    pdm_cfg.slot_cfg.sinc_scale = I2S_PDM_SIG_SCALING_MUL_4;

    ret = i2s_channel_init_pdm_tx_mode(*tx_handle, &pdm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S PDM init failed: %s", esp_err_to_name(ret));
        i2s_del_channel(*tx_handle);
        *tx_handle = nullptr;
        return ret;
    }

    gpio_set_direction(SPK_N_GPIO, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(SPK_N_GPIO, I2SO_SD_OUT_IDX, true, false);
    gpio_set_drive_capability(SPK_P_GPIO, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(SPK_N_GPIO, GPIO_DRIVE_CAP_0);

    ret = i2s_channel_enable(*tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(*tx_handle);
        *tx_handle = nullptr;
        return ret;
    }

    gpio_set_level(PA_CTL_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

void stop_pdm_output(i2s_chan_handle_t tx_handle)
{
    gpio_set_level(PA_CTL_GPIO, 0);
    if (tx_handle != nullptr) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
    }
    gpio_set_level(SPK_P_GPIO, 0);
    gpio_set_level(SPK_N_GPIO, 0);
}

esp_err_t write_pcm_scaled(i2s_chan_handle_t tx_handle, const int16_t *samples, size_t sample_count)
{
    constexpr size_t chunk_samples = 256;
    int16_t chunk[chunk_samples];
    size_t offset = 0;

    while (offset < sample_count) {
        const size_t count = std::min(chunk_samples, sample_count - offset);
        for (size_t i = 0; i < count; ++i) {
            chunk[i] = apply_volume(samples[offset + i]);
        }

        size_t bytes_written = 0;
        const esp_err_t ret = i2s_channel_write(tx_handle, chunk, count * sizeof(chunk[0]), &bytes_written, 1000);
        if (ret != ESP_OK) {
            return ret;
        }
        offset += bytes_written / sizeof(chunk[0]);
    }

    return ESP_OK;
}

esp_err_t play_note(i2s_chan_handle_t tx_handle, float frequency_hz, int duration_ms)
{
    constexpr int buffer_samples = 256;
    int16_t buffer[buffer_samples];
    const int total_samples = AUDIO_SAMPLE_RATE * duration_ms / 1000;
    float phase = 0.0f;
    const float phase_step = 2.0f * PI * frequency_hz / AUDIO_SAMPLE_RATE;
    int written_samples = 0;

    while (written_samples < total_samples) {
        const int samples_this_round = std::min(buffer_samples, total_samples - written_samples);
        for (int i = 0; i < samples_this_round; ++i) {
            const int sample_index = written_samples + i;
            const int attack = std::min(sample_index, AUDIO_SAMPLE_RATE / 100);
            const int release = std::min(total_samples - sample_index, AUDIO_SAMPLE_RATE / 100);
            const float envelope = std::min(1.0f, std::min(attack, release) / static_cast<float>(AUDIO_SAMPLE_RATE / 100));
            buffer[i] = static_cast<int16_t>(sinf(phase) * 10000.0f * envelope);
            phase += phase_step;
            if (phase >= 2.0f * PI) {
                phase -= 2.0f * PI;
            }
        }

        ESP_RETURN_ON_ERROR(write_pcm_scaled(tx_handle, buffer, samples_this_round), TAG, "Write note failed");
        written_samples += samples_this_round;
    }

    return ESP_OK;
}

esp_err_t play_melody()
{
    i2s_chan_handle_t tx_handle = nullptr;
    ESP_RETURN_ON_ERROR(start_pdm_output(AUDIO_SAMPLE_RATE, &tx_handle), TAG, "Start PDM failed");

    esp_err_t ret = ESP_OK;
    for (float note : AUDIO_TEST_MELODY) {
        ret = play_note(tx_handle, note, 180);
        if (ret != ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    int16_t silence[256] = {};
    write_pcm_scaled(tx_handle, silence, 256);
    stop_pdm_output(tx_handle);
    return ret;
}

bool find_wav_data(const uint8_t *wav, size_t wav_size, uint16_t &channels, uint32_t &sample_rate,
                   const uint8_t *&data, size_t &data_size)
{
    if (wav_size < 44 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool found_fmt = false;
    size_t offset = 12;
    while (offset + 8 <= wav_size) {
        const uint8_t *chunk = wav + offset;
        const uint32_t chunk_size = read_le32(chunk + 4);
        const size_t payload = offset + 8;
        if (payload + chunk_size > wav_size) {
            return false;
        }

        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            const uint16_t audio_format = read_le16(wav + payload);
            channels = read_le16(wav + payload + 2);
            sample_rate = read_le32(wav + payload + 4);
            const uint16_t bits_per_sample = read_le16(wav + payload + 14);
            found_fmt = audio_format == 1 && (channels == 1 || channels == 2) && bits_per_sample == 16;
        } else if (memcmp(chunk, "data", 4) == 0) {
            data = wav + payload;
            data_size = chunk_size;
        }

        offset = payload + chunk_size + (chunk_size & 1);
    }

    return found_fmt && data != nullptr && data_size > 0 && sample_rate >= 8000 && sample_rate <= 48000;
}

esp_err_t play_wav(const uint8_t *wav, size_t wav_size)
{
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    const uint8_t *data = nullptr;
    size_t data_size = 0;

    if (!find_wav_data(wav, wav_size, channels, sample_rate, data, data_size)) {
        ESP_LOGE(TAG, "Custom audio must be 8-48 kHz, 16-bit PCM, mono or stereo WAV");
        return ESP_ERR_INVALID_ARG;
    }

    i2s_chan_handle_t tx_handle = nullptr;
    ESP_RETURN_ON_ERROR(start_pdm_output(sample_rate, &tx_handle), TAG, "Start PDM failed");

    esp_err_t ret = ESP_OK;
    constexpr size_t frames_per_chunk = 256;
    int16_t mono[frames_per_chunk];
    const size_t frame_size = channels * sizeof(int16_t);
    size_t offset = 0;

    while (offset + frame_size <= data_size) {
        const size_t frames = std::min(frames_per_chunk, (data_size - offset) / frame_size);
        for (size_t i = 0; i < frames; ++i) {
            const int16_t left = static_cast<int16_t>(read_le16(data + offset + i * frame_size));
            if (channels == 1) {
                mono[i] = left;
            } else {
                const int16_t right = static_cast<int16_t>(read_le16(data + offset + i * frame_size + 2));
                mono[i] = static_cast<int16_t>((static_cast<int32_t>(left) + right) / 2);
            }
        }
        ret = write_pcm_scaled(tx_handle, mono, frames);
        if (ret != ESP_OK) {
            break;
        }
        offset += frames * frame_size;
    }

    stop_pdm_output(tx_handle);
    return ret;
}

esp_err_t play_recording()
{
    if (g_recording == nullptr || g_recording_samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_handle_t tx_handle = nullptr;
    ESP_RETURN_ON_ERROR(start_pdm_output(RECORD_SAMPLE_RATE, &tx_handle), TAG, "Start PDM failed");
    const esp_err_t ret = write_pcm_scaled(tx_handle, g_recording, g_recording_samples);
    stop_pdm_output(tx_handle);
    return ret;
}

void playback_task(void *arg)
{
    const PlaybackMode mode = static_cast<PlaybackMode>(reinterpret_cast<intptr_t>(arg));
    g_audio_busy.store(true);

    esp_err_t ret = ESP_OK;
    if (mode == PlaybackMode::Melody) {
        ret = play_melody();
    } else if (mode == PlaybackMode::CustomFile) {
        ret = play_wav(custom_audio_wav_start, custom_audio_wav_end - custom_audio_wav_start);
    } else {
        ret = play_recording();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Playback failed: %s", esp_err_to_name(ret));
    }

    g_audio_busy.store(false);
    vTaskDelete(nullptr);
}

void record_task(void *)
{
    g_recording_busy.store(true);
    g_mic_running.store(false);

    int16_t *new_recording = static_cast<int16_t *>(heap_caps_malloc(RECORD_SAMPLE_COUNT * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (new_recording == nullptr) {
        new_recording = static_cast<int16_t *>(heap_caps_malloc(RECORD_SAMPLE_COUNT * sizeof(int16_t), MALLOC_CAP_8BIT));
    }
    if (new_recording == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate recording buffer");
        g_recording_busy.store(false);
        vTaskDelete(nullptr);
        return;
    }

    int64_t baseline_sum = 0;
    for (int i = 0; i < 256; ++i) {
        int raw = 0;
        if (adc_oneshot_read(g_adc_handle, MIC_ADC_CHANNEL, &raw) == ESP_OK) {
            baseline_sum += raw;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    const int baseline = static_cast<int>(baseline_sum / 256);

    const int64_t start_us = esp_timer_get_time();
    int min_raw = 4095;
    int max_raw = 0;
    int64_t sum = 0;

    for (int i = 0; i < RECORD_SAMPLE_COUNT; ++i) {
        const int64_t target_us = start_us + (static_cast<int64_t>(i) * 1000000 / RECORD_SAMPLE_RATE);
        while (esp_timer_get_time() < target_us) {
            esp_rom_delay_us(10);
        }

        int raw = baseline;
        adc_oneshot_read(g_adc_handle, MIC_ADC_CHANNEL, &raw);
        min_raw = std::min(min_raw, raw);
        max_raw = std::max(max_raw, raw);
        sum += raw;

        const int32_t centered = (raw - baseline) * 48;
        new_recording[i] = static_cast<int16_t>(std::clamp<int32_t>(centered, INT16_MIN, INT16_MAX));

        if ((i % 512) == 511) {
            g_mic_average.store(static_cast<int>(sum / (i + 1)));
            g_mic_minimum.store(min_raw);
            g_mic_maximum.store(max_raw);
            g_mic_range.store(max_raw - min_raw);
            g_mic_sequence.fetch_add(1);
        }
    }

    if (g_recording != nullptr) {
        free(g_recording);
    }
    g_recording = new_recording;
    g_recording_samples = RECORD_SAMPLE_COUNT;

    g_recording_busy.store(false);
    vTaskDelete(nullptr);
}

void mic_task(void *)
{
    while (g_mic_running.load()) {
        int min_raw = 4095;
        int max_raw = 0;
        int64_t sum = 0;
        int valid_count = 0;

        for (int i = 0; i < 128 && g_mic_running.load(); ++i) {
            int raw = 0;
            esp_err_t ret = adc_oneshot_read(g_adc_handle, MIC_ADC_CHANNEL, &raw);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Microphone read failed: %s", esp_err_to_name(ret));
                break;
            }
            min_raw = std::min(min_raw, raw);
            max_raw = std::max(max_raw, raw);
            sum += raw;
            ++valid_count;
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        if (valid_count > 0) {
            g_mic_average.store(static_cast<int>(sum / valid_count));
            g_mic_minimum.store(min_raw);
            g_mic_maximum.store(max_raw);
            g_mic_range.store(max_raw - min_raw);
            g_mic_sequence.fetch_add(1);
        }
    }

    g_mic_task_handle = nullptr;
    vTaskDelete(nullptr);
}

bool start_playback_task(PlaybackMode mode)
{
    if (!audio_self_test_init() || g_audio_busy.load() || g_speaker_busy.load() || g_recording_busy.load()) {
        return false;
    }

    return xTaskCreate(
        playback_task,
        "audio_playback",
        6144,
        reinterpret_cast<void *>(static_cast<intptr_t>(mode)),
        5,
        nullptr
    ) == pdPASS;
}

} // namespace

bool audio_self_test_init()
{
    if (g_initialized.load()) {
        return true;
    }

    gpio_config_t pa_cfg = {};
    pa_cfg.pin_bit_mask = (1ULL << PA_CTL_GPIO);
    pa_cfg.mode = GPIO_MODE_OUTPUT;
    pa_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    pa_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pa_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&pa_cfg));
    gpio_set_level(PA_CTL_GPIO, 0);

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config, &g_adc_handle);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Microphone ADC init failed: %s", esp_err_to_name(ret));
        return false;
    }

    adc_oneshot_chan_cfg_t mic_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(g_adc_handle, MIC_ADC_CHANNEL, &mic_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Microphone ADC channel config failed: %s", esp_err_to_name(ret));
        return false;
    }

    g_initialized.store(true);
    return true;
}

bool audio_self_test_play_audio_clip()
{
    return start_playback_task(PlaybackMode::Melody);
}

bool audio_self_test_play_custom_file()
{
    return start_playback_task(PlaybackMode::CustomFile);
}

bool audio_self_test_record_microphone()
{
    if (!audio_self_test_init() || g_recording_busy.load() || g_audio_busy.load()) {
        return false;
    }

    return xTaskCreate(record_task, "mic_record", 4096, nullptr, 5, nullptr) == pdPASS;
}

bool audio_self_test_play_recording()
{
    if (!audio_self_test_has_recording()) {
        return false;
    }
    return start_playback_task(PlaybackMode::Recording);
}

bool audio_self_test_play_speaker()
{
    if (!audio_self_test_init() || g_speaker_busy.load() || g_audio_busy.load() || g_recording_busy.load()) {
        return false;
    }

    return xTaskCreate(speaker_task, "speaker_test", 3072, nullptr, 5, nullptr) == pdPASS;
}

bool audio_self_test_start_mic_monitor()
{
    if (!audio_self_test_init() || g_recording_busy.load()) {
        return false;
    }
    if (g_mic_running.load()) {
        return true;
    }

    g_mic_running.store(true);
    if (xTaskCreate(mic_task, "mic_monitor", 4096, nullptr, 4, &g_mic_task_handle) != pdPASS) {
        g_mic_running.store(false);
        g_mic_task_handle = nullptr;
        return false;
    }
    return true;
}

void audio_self_test_stop_mic_monitor()
{
    g_mic_running.store(false);
}

void audio_self_test_set_volume_percent(int volume)
{
    g_volume_percent.store(std::clamp(volume, 0, 100));
}

int audio_self_test_get_volume_percent()
{
    return g_volume_percent.load();
}

bool audio_self_test_is_mic_monitor_running()
{
    return g_mic_running.load();
}

bool audio_self_test_is_audio_busy()
{
    return g_audio_busy.load();
}

bool audio_self_test_is_recording_busy()
{
    return g_recording_busy.load();
}

bool audio_self_test_is_speaker_busy()
{
    return g_speaker_busy.load();
}

bool audio_self_test_has_recording()
{
    return g_recording != nullptr && g_recording_samples > 0;
}

audio_mic_stats_t audio_self_test_get_mic_stats()
{
    return {
        .average = g_mic_average.load(),
        .minimum = g_mic_minimum.load(),
        .maximum = g_mic_maximum.load(),
        .range = g_mic_range.load(),
        .sequence = g_mic_sequence.load(),
    };
}
