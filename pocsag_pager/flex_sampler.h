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
bool flex_sampler_is_running(const FlexSampler* instance);

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
