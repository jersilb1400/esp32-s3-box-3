#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Thread-safe RMS from PCM about to hit the DAC (TTS / voice reply / UI sounds).
 * Call from AudioOutputTask; read from LVGL JarvisPointCloud::Tick().
 */
void JarvisSpeechMeterFeedPlaybackPcm(const int16_t* samples, size_t sample_count, int channels);

float JarvisSpeechMeterTargetLevel(void);
