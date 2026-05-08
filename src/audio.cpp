//
// Created by awalol on 2026/3/5.
//

#include "audio.h"
#include "bt.h"
#include "resample.h"
#include "tusb.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "opus.h"
#include "utils.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "pico/util/queue.h"
#include "config.h"
#include "usb.h"

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2
#define SAMPLE_SIZE       64
#define REPORT_SIZE       398
#define REPORT_ID         0x36
// #define VOLUME_GAIN       2
// #define BUFFER_LENGTH     48

using std::clamp;
using std::max;

static WDL_Resampler resampler;
static uint8_t reportSeqCounter = 0;
static uint8_t packetCounter = 0;
static bool plug_headset = false;
alignas(8) static uint32_t audio_core1_stack[8192];
queue_t audio_fifo;
static queue_t audio_free_fifo;
queue_t opus_fifo;

constexpr uint32_t AUDIO_WARNING_INTERVAL_US = 1000 * 1000;
constexpr int USB_AUDIO_RAW_SAMPLES = 192;
constexpr int HAPTIC_INPUT_FRAMES = USB_AUDIO_RAW_SAMPLES / INPUT_CHANNELS;
constexpr int HAPTIC_OUTPUT_FRAMES = SAMPLE_SIZE / OUTPUT_CHANNELS;
constexpr size_t AUDIO_READY_QUEUE_DEPTH = 2;
constexpr size_t AUDIO_BUFFER_POOL_SIZE = AUDIO_READY_QUEUE_DEPTH + 2; // ready queue + fill + process

struct audio_raw_element {
    float data[512 * 2];
};
struct opus_element {
    uint8_t data[200];
};

static audio_raw_element audio_buffer_pool[AUDIO_BUFFER_POOL_SIZE];

void set_headset(bool state) {
    plug_headset = state;
}

static bool should_log_warning(uint32_t &last_log_us) {
    const uint32_t now = time_us_32();
    if (last_log_us != 0 && now - last_log_us < AUDIO_WARNING_INTERVAL_US) {
        return false;
    }
    last_log_us = now;
    return true;
}

static void release_audio_buffer(audio_raw_element *buffer) {
    if (buffer == nullptr) {
        return;
    }
    if (!queue_try_add(&audio_free_fifo, &buffer)) {
        static uint32_t last_audio_free_warning_us = 0;
        if (should_log_warning(last_audio_free_warning_us)) {
            printf("[Audio] Warning: audio_free_fifo add failed\n");
        }
    }
}

static audio_raw_element *acquire_audio_buffer() {
    audio_raw_element *buffer = nullptr;
    if (queue_try_remove(&audio_free_fifo, &buffer)) {
        return buffer;
    }

    if (queue_try_remove(&audio_fifo, &buffer)) {
        return buffer;
    }

    static uint32_t last_audio_acquire_warning_us = 0;
    if (should_log_warning(last_audio_acquire_warning_us)) {
        printf("[Audio] Warning: audio buffer unavailable\n");
    }
    return nullptr;
}

static void queue_audio_buffer(audio_raw_element *buffer) {
    if (buffer == nullptr) {
        return;
    }
    if (queue_is_full(&audio_fifo)) {
        audio_raw_element *oldest = nullptr;
        if (queue_try_remove(&audio_fifo, &oldest)) {
            release_audio_buffer(oldest);
        }
    }
    if (!queue_try_add(&audio_fifo, &buffer)) {
        release_audio_buffer(buffer);
        static uint32_t last_audio_fifo_warning_us = 0;
        if (should_log_warning(last_audio_fifo_warning_us)) {
            printf("[Audio] Warning: audio_fifo add failed\n");
        }
    }
}

void audio_loop() {
    // 1. 读取 USB 音频数据
    if (!tud_audio_available()) return;

    int16_t raw[USB_AUDIO_RAW_SAMPLES];
    uint32_t bytes_read = tud_audio_read(raw, sizeof(raw)); // 每次读入 384 bytes
    int frames = bytes_read / (INPUT_CHANNELS * sizeof(int16_t));
    if (frames == 0) {
        return;
    }

    const auto &config = get_config();
    const float speaker_gain = mute[0] ? 0.0f : powf(10.0f, config.speaker_volume / 20.0f);
    const float haptics_gain = max(config.haptics_gain, 1.0f);
    const uint8_t haptics_buffer_length = config.haptics_buffer_length;

    static audio_raw_element *audio_buf = nullptr;
    static uint audio_buf_pos = 0;
    // 2. 从4ch中提取ch3/ch4，转换为float输入重采样器
    WDL_ResampleSample *in_buf;
    int nframes = resampler.ResamplePrepare(frames, OUTPUT_CHANNELS, &in_buf);

    for (int i = 0; i < nframes; i++) {
        if (audio_buf == nullptr) {
            audio_buf = acquire_audio_buffer();
        }
        if (audio_buf != nullptr) {
            audio_buf->data[audio_buf_pos++] = raw[i * INPUT_CHANNELS] / 32768.0f * speaker_gain;
            audio_buf->data[audio_buf_pos++] = raw[i * INPUT_CHANNELS + 1] / 32768.0f * speaker_gain;
        }
        if (audio_buf_pos == 512 * 2) {
            queue_audio_buffer(audio_buf);
            audio_buf = nullptr;
            audio_buf_pos = 0;
        }

        in_buf[i * 2] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 2] / 32768.0f;
        in_buf[i * 2 + 1] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 3] / 32768.0f;
    }

    // 3. 48kHz -> 3kHz 重采样
    static WDL_ResampleSample out_buf[SAMPLE_SIZE]; // 64 floats = 32帧 × 2ch
    int out_frames = resampler.ResampleOut(out_buf, nframes, SAMPLE_SIZE / OUTPUT_CHANNELS, OUTPUT_CHANNELS);

    static int8_t haptic_buf[SAMPLE_SIZE];
    static int haptic_buf_pos = 0;

    // 4. 转换为int8并缓冲，满64字节即组包发送
    for (int i = 0; i < out_frames; i++) {
        int val_l = (int) (out_buf[i * 2] * 127.0f * haptics_gain);
        int val_r = (int) (out_buf[i * 2 + 1] * 127.0f * haptics_gain);
        haptic_buf[haptic_buf_pos++] = (int8_t) clamp(val_l, -128, 127); // 似乎clamp有点多余？还是以防万一吧
        haptic_buf[haptic_buf_pos++] = (int8_t) clamp(val_r, -128, 127);

        if (haptic_buf_pos != SAMPLE_SIZE) {
            continue;
        }
        uint8_t pkt[REPORT_SIZE]{};
        pkt[0] = REPORT_ID;
        pkt[1] = reportSeqCounter << 4;
        reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
        pkt[2] = 0x11 | (1 << 7);
        pkt[3] = 7;
        pkt[4] = 0b11111110;
        const auto buf_len = haptics_buffer_length;
        pkt[5] = buf_len;
        pkt[6] = buf_len;
        pkt[7] = buf_len;
        pkt[8] = buf_len;
        pkt[9] = buf_len; // buffer length
        pkt[10] = packetCounter++;
        pkt[11] = 0x12 | (1 << 7);
        pkt[12] = SAMPLE_SIZE;
        memcpy(pkt + 13, haptic_buf, SAMPLE_SIZE);
        static opus_element opus_packet{};
        if (queue_try_remove(&opus_fifo,&opus_packet)) {
            pkt[77] = (plug_headset ? 0x16 : 0x13) | 0 << 6 | 1 << 7; // Speaker: 0x13
            // L Headset Mono: 0x14
            // L Headset R Speaker: 0x15
            // Headset: 0x16
            pkt[78] = 200;
            memcpy(pkt + 79,opus_packet.data,200);
        }else {
            static uint32_t last_opus_fifo_remove_warning_us = 0;
            if (should_log_warning(last_opus_fifo_remove_warning_us)) {
                printf("[Audio] Warning: opus_fifo try remove failed\n");
            }
        }

        bt_write(pkt, sizeof(pkt));
        haptic_buf_pos = 0;
    }
}

void audio_init() {
    resampler.SetMode(true, 0, false);
    resampler.SetRates(48000, 3000);
    resampler.SetFeedMode(true);
    resampler.Prealloc(OUTPUT_CHANNELS, HAPTIC_INPUT_FRAMES, HAPTIC_OUTPUT_FRAMES);
    queue_init(&audio_fifo,sizeof(audio_raw_element *),AUDIO_READY_QUEUE_DEPTH);
    queue_init(&audio_free_fifo,sizeof(audio_raw_element *),AUDIO_BUFFER_POOL_SIZE);
    for (auto &buffer : audio_buffer_pool) {
        audio_raw_element *buffer_ptr = &buffer;
        queue_try_add(&audio_free_fifo, &buffer_ptr);
    }
    queue_init(&opus_fifo,sizeof(opus_element),2);
    multicore_launch_core1_with_stack(core1_entry,audio_core1_stack,sizeof(audio_core1_stack));
}

static OpusEncoder *encoder;
static WDL_Resampler resampler_audio;

void core1_entry() {
    int error = 0;
    encoder = opus_encoder_create(48000,2,OPUS_APPLICATION_AUDIO,&error);
    if (error != 0) {
        printf("[Audio] OpusEncoder create failed\n");
        return;
    }
    opus_encoder_ctl(encoder,OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder,OPUS_SET_BITRATE(200 * 8 * 100));
    opus_encoder_ctl(encoder,OPUS_SET_VBR(false));
    opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(0)); // max 4
    resampler_audio.SetMode(true,0,false);
    resampler_audio.SetRates(51200,48000);
    resampler_audio.SetFeedMode(true);
    resampler_audio.Prealloc(2,512,480);

    while (true) {
        audio_raw_element *audio_element = nullptr;
        queue_remove_blocking(&audio_fifo,&audio_element);
        // 将 512 frames 重采样成 480 frames 以解决噪音问题。感谢 @Junhoo
        WDL_ResampleSample *in_buf;
        int nframes = resampler_audio.ResamplePrepare(512, 2, &in_buf);
        for (int i = 0; i < nframes * 2;i++) {
            in_buf[i] = audio_element->data[i];
        }
        static WDL_ResampleSample out_buf[480 * 2];
        resampler_audio.ResampleOut(out_buf,nframes,480,2);
        release_audio_buffer(audio_element);
        static opus_element opus_packet{};
        const int encoded_size = opus_encode_float(encoder,out_buf,480,opus_packet.data,200);
        if (encoded_size < 0) {
            static uint32_t last_opus_encode_warning_us = 0;
            if (should_log_warning(last_opus_encode_warning_us)) {
                printf("[Audio] Warning: opus_encode_float failed: %d\n", encoded_size);
            }
            continue;
        }
        if (queue_is_full(&opus_fifo)) {
            opus_element dropped{};
            queue_try_remove(&opus_fifo, &dropped);
        }
        if (!queue_try_add(&opus_fifo,&opus_packet)) {
            static uint32_t last_opus_fifo_add_warning_us = 0;
            if (should_log_warning(last_opus_fifo_add_warning_us)) {
                printf("[Audio] Warning: opus_fifo add failed\n");
            }
        }
    }
}
