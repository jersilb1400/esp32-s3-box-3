#include "jarvis_speech_meter.h"

#include <atomic>
#include <cmath>

static float RmsNormalizePcmFrames(const int16_t* pcm, size_t sample_count, int channels) {
    if (pcm == nullptr || channels < 1) {
        return 0.f;
    }
    if (channels > 8) {
        channels = 8;
    }
    if (sample_count < (size_t)channels) {
        return 0.f;
    }

    const size_t frames = sample_count / (size_t)channels;
    double acc = 0.0;

    for (size_t f = 0; f < frames; ++f) {
        float sum = 0.f;
        const int16_t* frame = pcm + f * (size_t)channels;
        for (int ch = 0; ch < channels; ++ch) {
            sum += (float)frame[ch];
        }
        float mono = sum / ((float)channels * 32768.f);
        acc += (double)mono * (double)mono;
    }

    float ms = static_cast<float>(acc / (double)frames);
    return std::sqrt(ms);
}

static std::atomic<float> g_latest_rms_norm{0.f};

void JarvisSpeechMeterFeedPlaybackPcm(const int16_t* samples, size_t sample_count, int channels) {
    constexpr float POST_GAIN = 11.5f;
    float raw = RmsNormalizePcmFrames(samples, sample_count, channels);
    float n = raw * POST_GAIN;
    if (n > 1.f) {
        n = 1.f;
    }
    g_latest_rms_norm.store(n, std::memory_order_relaxed);
}

float JarvisSpeechMeterTargetLevel(void) {
    return g_latest_rms_norm.load(std::memory_order_relaxed);
}
