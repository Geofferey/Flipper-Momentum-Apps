#pragma once

#include <lib/subghz/devices/devices.h>
#include <lib/subghz/protocols/base.h>

typedef struct FlexSampler FlexSampler;

FlexSampler* flex_sampler_alloc(
    SubGhzProtocolDecoderBase* decoder,
    const SubGhzDevice* radio_device,
    bool external_radio);
void flex_sampler_free(FlexSampler* instance);
void flex_sampler_start(FlexSampler* instance);
void flex_sampler_stop(FlexSampler* instance);
void flex_sampler_pause(FlexSampler* instance);
void flex_sampler_resume(FlexSampler* instance);
bool flex_sampler_is_running(const FlexSampler* instance);
bool flex_sampler_is_paused(const FlexSampler* instance);

/** True while the decoder is locked onto a frame (cached; reads no registers). */
bool flex_sampler_is_locked(const FlexSampler* instance);

/** Number of valid FLEX frames completed since the sampler was started. */
uint32_t flex_sampler_completed_frames(const FlexSampler* instance);

/** Record this receive session's raw sample stream to a timestamped file. */
void flex_sampler_set_capture(FlexSampler* instance, bool enable);

/** Write periodic counter-only diagnostics to /ext/pocsag/stats.txt. */
void flex_sampler_set_stats_log(FlexSampler* instance, bool enable);

/** Set the active RX crystal correction reported in diagnostics. */
void flex_sampler_set_freq_correction(FlexSampler* instance, int32_t ppm);

/** Enable opt-in soft-decision (Chase) recovery of weak FLEX4 words. */
void flex_sampler_set_weak_decode(FlexSampler* instance, bool enable);

typedef enum {
    FlexSamplerSoundOff,
    // The upper decision bit drives the speaker pin directly (full volume).
    FlexSamplerSoundRaw,
    // Each of the four levels plays its own pitch through the speaker's PWM.
    FlexSamplerSoundTone,
    // Like Tone but driven by the decoder's recovered symbols (clean, no noise).
    // The sampler thread owns the speaker; the decoder plays the tones.
    FlexSamplerSoundSynth,
} FlexSamplerSound;

/**
 * Play the received signal while sampling. Raw needs the speaker pin set up
 * as an output; Tone needs the speaker acquired and uses volume (0..1) and
 * one pitch in Hz per level, lowest frequency first (tones may be NULL
 * otherwise).
 */
void flex_sampler_set_sound(
    FlexSampler* instance,
    FlexSamplerSound sound,
    const GpioPin* pin,
    float volume,
    const float tones[4]);
