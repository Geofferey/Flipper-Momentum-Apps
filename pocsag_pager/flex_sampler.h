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
