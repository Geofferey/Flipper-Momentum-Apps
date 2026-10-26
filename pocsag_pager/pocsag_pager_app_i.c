#include "pocsag_pager_app_i.h"

#define TAG "POCSAGPager"
#include <flipper_format/flipper_format_i.h>

static bool pcsg_preset_has_mode_prefix(const char* preset_name, const char* prefix) {
    const size_t prefix_length = strlen(prefix);
    if(strncmp(preset_name, prefix, prefix_length) != 0) return false;
    const char suffix = preset_name[prefix_length];
    return suffix == '\0' || suffix == '-' || suffix == ' ' || suffix == '\t';
}

bool pcsg_preset_is_flex4(const char* preset_name) {
    furi_assert(preset_name);
    return pcsg_preset_has_mode_prefix(preset_name, "FLEX4") ||
           pcsg_preset_has_mode_prefix(preset_name, "FLEX-4");
}

void pcsg_preset_init(
    void* context,
    const char* preset_name,
    uint32_t frequency,
    uint8_t* preset_data,
    size_t preset_data_size) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    furi_string_set(app->txrx->preset->name, preset_name);
    app->txrx->preset->frequency = frequency;
    app->txrx->preset->data = preset_data;
    app->txrx->preset->data_size = preset_data_size;
}

void pcsg_get_frequency_modulation(
    POCSAGPagerApp* app,
    FuriString* frequency,
    FuriString* modulation) {
    furi_assert(app);
    if(frequency != NULL) {
        furi_string_printf(
            frequency,
            "%03ld.%02ld",
            app->txrx->preset->frequency / 1000000 % 1000,
            app->txrx->preset->frequency / 10000 % 100);
    }
    if(modulation != NULL) {
        const char* preset_name = furi_string_get_cstr(app->txrx->preset->name);
        if(pcsg_preset_is_flex4(preset_name)) {
            furi_string_set_str(modulation, "F4");
        } else if(!strncmp(preset_name, "FLEX", 4)) {
            furi_string_set_str(modulation, "FX");
        } else if(!strcmp(preset_name, "POCSAG-W")) {
            furi_string_set_str(modulation, "PW");
        } else {
            furi_string_printf(modulation, "%.2s", preset_name);
        }
    }
}

void pcsg_begin(POCSAGPagerApp* app, uint8_t* preset_data) {
    furi_assert(app);

    subghz_devices_reset(app->txrx->radio_device);
    subghz_devices_idle(app->txrx->radio_device);
    subghz_devices_load_preset(app->txrx->radio_device, FuriHalSubGhzPresetCustom, preset_data);

    // furi_hal_gpio_init(furi_hal_subghz.cc1101_g0_pin, GpioModeInput, GpioPullNo, GpioSpeedLow);
    app->txrx->txrx_state = PCSGTxRxStateIDLE;
}

uint32_t pcsg_rx(POCSAGPagerApp* app, uint32_t frequency) {
    furi_assert(app);
    if(!subghz_devices_is_frequency_valid(app->txrx->radio_device, frequency)) {
        furi_crash("POCSAGPager: Incorrect RX frequency.");
    }
    furi_assert(
        app->txrx->txrx_state != PCSGTxRxStateRx && app->txrx->txrx_state != PCSGTxRxStateSleep);

    subghz_devices_idle(app->txrx->radio_device);
    uint32_t value = subghz_devices_set_frequency(app->txrx->radio_device, frequency);

    // Not need. init in subghz_devices_start_async_tx
    // furi_hal_gpio_init(furi_hal_subghz.cc1101_g0_pin, GpioModeInput, GpioPullNo, GpioSpeedLow);

    subghz_devices_flush_rx(app->txrx->radio_device);
    subghz_devices_set_rx(app->txrx->radio_device);

    // Signal sound. Raw mirrors the received data to the speaker pin, as the
    // Sub-GHz app does; FLEX4 has no async capture to mirror, so its sampler
    // drives the pin. Tone (FLEX4 only) plays each level as a pitch through
    // the speaker's PWM, which has a volume; the async path cannot (its
    // worker thread does not own the speaker) and falls back to Raw.
    const bool flex4 = pcsg_preset_is_flex4(furi_string_get_cstr(app->txrx->preset->name));
    if(app->filter.signal_sound != PCSGSoundOff && furi_hal_speaker_acquire(30)) {
        app->txrx->speaker_on = true;
        if(flex4 && app->filter.signal_sound == PCSGSoundTone) {
            static const float volumes[PCSGToneVolumeCount] = {0.1f, 0.4f, 1.0f};
            // Lowest pitch, and the step between levels as a share of it.
            static const float bases[PCSGTonePitchCount] = {300.0f, 600.0f, 1200.0f};
            static const float spreads[PCSGToneSpreadCount] = {0.25f, 0.5f, 1.0f};
            const float base = bases[app->filter.tone_pitch];
            const float step = base * spreads[app->filter.tone_spread];
            const float tones[4] = {base, base + step, base + 2.0f * step, base + 3.0f * step};
            flex_sampler_set_sound(
                app->txrx->flex_sampler,
                FlexSamplerSoundTone,
                NULL,
                volumes[app->filter.tone_volume],
                tones);
        } else if(flex4) {
            furi_hal_gpio_init(&gpio_speaker, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
            flex_sampler_set_sound(
                app->txrx->flex_sampler, FlexSamplerSoundRaw, &gpio_speaker, 0.0f, NULL);
        } else {
            subghz_devices_set_async_mirror_pin(app->txrx->radio_device, &gpio_speaker);
        }
    }

    if(flex4) {
        flex_sampler_start(app->txrx->flex_sampler);
    } else {
        subghz_devices_start_async_rx(
            app->txrx->radio_device, subghz_worker_rx_callback, app->txrx->worker);
        subghz_worker_start(app->txrx->worker);
    }
    app->txrx->txrx_state = PCSGTxRxStateRx;
    return value;
}

void pcsg_idle(POCSAGPagerApp* app) {
    furi_assert(app);
    furi_assert(app->txrx->txrx_state != PCSGTxRxStateSleep);
    subghz_devices_idle(app->txrx->radio_device);
    app->txrx->txrx_state = PCSGTxRxStateIDLE;
}

void pcsg_rx_end(POCSAGPagerApp* app) {
    furi_assert(app);
    furi_assert(app->txrx->txrx_state == PCSGTxRxStateRx);
    if(flex_sampler_is_running(app->txrx->flex_sampler)) {
        flex_sampler_stop(app->txrx->flex_sampler);
    } else if(subghz_worker_is_running(app->txrx->worker)) {
        subghz_worker_stop(app->txrx->worker);
        subghz_devices_stop_async_rx(app->txrx->radio_device);
    }
    if(app->txrx->speaker_on) {
        flex_sampler_set_sound(app->txrx->flex_sampler, FlexSamplerSoundOff, NULL, 0.0f, NULL);
        subghz_devices_set_async_mirror_pin(app->txrx->radio_device, NULL);
        if(furi_hal_speaker_is_mine()) {
            furi_hal_speaker_stop();
            furi_hal_speaker_release();
        }
        app->txrx->speaker_on = false;
    }
    subghz_devices_idle(app->txrx->radio_device);
    app->txrx->txrx_state = PCSGTxRxStateIDLE;
}

void pcsg_sleep(POCSAGPagerApp* app) {
    furi_assert(app);
    subghz_devices_sleep(app->txrx->radio_device);
    app->txrx->txrx_state = PCSGTxRxStateSleep;
}

void pcsg_hopper_update(POCSAGPagerApp* app) {
    furi_assert(app);

    switch(app->txrx->hopper_state) {
    case PCSGHopperStateOFF:
        return;
        break;
    case PCSGHopperStatePause:
        return;
        break;
    case PCSGHopperStateRSSITimeOut:
        if(app->txrx->hopper_timeout != 0) {
            app->txrx->hopper_timeout--;
            return;
        }
        break;
    default:
        break;
    }
    float rssi = -127.0f;
    if(app->txrx->hopper_state != PCSGHopperStateRSSITimeOut) {
        // See RSSI Calculation timings in CC1101 17.3 RSSI
        rssi = subghz_devices_get_rssi(app->txrx->radio_device);

        // Stay if RSSI is high enough
        if(rssi > -90.0f) {
            app->txrx->hopper_timeout = 10;
            app->txrx->hopper_state = PCSGHopperStateRSSITimeOut;
            return;
        }
    } else {
        app->txrx->hopper_state = PCSGHopperStateRunnig;
    }
    // Select next frequency
    if(app->txrx->hopper_idx_frequency <
       subghz_setting_get_hopper_frequency_count(app->setting) - 1) {
        app->txrx->hopper_idx_frequency++;
    } else {
        app->txrx->hopper_idx_frequency = 0;
    }

    if(app->txrx->txrx_state == PCSGTxRxStateRx) {
        pcsg_rx_end(app);
    };
    if(app->txrx->txrx_state == PCSGTxRxStateIDLE) {
        subghz_receiver_reset(app->txrx->receiver);
        app->txrx->preset->frequency =
            subghz_setting_get_hopper_frequency(app->setting, app->txrx->hopper_idx_frequency);
        pcsg_rx(app, app->txrx->preset->frequency);
    }
}
