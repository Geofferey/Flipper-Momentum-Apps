#include "pocsag_pager_app_i.h"

#define TAG "POCSAGPager"
#include <flipper_format/flipper_format_i.h>
#include <furi_hal_spi.h>

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

    // Compensate the crystal offset so the LO lands on the selected channel.
    // The displayed frequency stays nominal; only the tuning is shifted.
    uint32_t tuned = pcsg_correct_frequency(app->filter.freq_correction_ppm, frequency);
    if(!subghz_devices_is_frequency_valid(app->txrx->radio_device, tuned)) tuned = frequency;

    subghz_devices_idle(app->txrx->radio_device);
    uint32_t value = subghz_devices_set_frequency(app->txrx->radio_device, tuned);

    // Not need. init in subghz_devices_start_async_tx
    // furi_hal_gpio_init(furi_hal_subghz.cc1101_g0_pin, GpioModeInput, GpioPullNo, GpioSpeedLow);

    subghz_devices_flush_rx(app->txrx->radio_device);
    subghz_devices_set_rx(app->txrx->radio_device);

    // Apply the saved setting from a known silent state. This also clears a
    // sampler-owned Synth mode left by an earlier receive session.
    flex_sampler_set_sound(app->txrx->flex_sampler, FlexSamplerSoundOff, NULL, 0.0f, NULL);
    subghz_devices_set_async_mirror_pin(app->txrx->radio_device, NULL);

    // Signal sound. Raw mirrors the received data to the speaker pin, as the
    // Sub-GHz app does; FLEX4 has no async capture to mirror, so its sampler
    // drives the pin. Tone (FLEX4 only) plays each level as a pitch through
    // the speaker's PWM, which has a volume; the async path cannot (its
    // worker thread does not own the speaker) and falls back to Raw.
    const bool flex4 = pcsg_preset_is_flex4(furi_string_get_cstr(app->txrx->preset->name));
    if(app->filter.signal_sound != PCSGSoundOff) {
        static const float volumes[PCSGToneVolumeCount] = {0.1f, 0.4f, 1.0f};
        // Lowest pitch, and the step between levels as a share of it.
        static const float bases[PCSGTonePitchCount] = {300.0f, 600.0f, 1200.0f};
        static const float spreads[PCSGToneSpreadCount] = {0.25f, 0.5f, 1.0f};
        const float base = bases[app->filter.tone_pitch];
        const float step = base * spreads[app->filter.tone_spread];
        const float tones[4] = {base, base + step, base + 2.0f * step, base + 3.0f * step};
        const float volume = volumes[app->filter.tone_volume];

        if(flex4 && app->filter.signal_sound == PCSGSoundSynth) {
            // The sampler thread owns the speaker and the decoder plays tones,
            // so nothing is acquired here.
            flex_sampler_set_sound(
                app->txrx->flex_sampler, FlexSamplerSoundSynth, NULL, volume, tones);
        } else if(furi_hal_speaker_acquire(30)) {
            app->txrx->speaker_on = true;
            if(flex4 && app->filter.signal_sound == PCSGSoundTone) {
                flex_sampler_set_sound(
                    app->txrx->flex_sampler, FlexSamplerSoundTone, NULL, volume, tones);
            } else if(flex4) {
                furi_hal_gpio_init(&gpio_speaker, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
                flex_sampler_set_sound(
                    app->txrx->flex_sampler, FlexSamplerSoundRaw, &gpio_speaker, 0.0f, NULL);
            } else {
                // POCSAG / two-level FLEX: mirror the data line (Raw).
                subghz_devices_set_async_mirror_pin(app->txrx->radio_device, &gpio_speaker);
            }
        }
    }

    if(flex4) {
        flex_sampler_set_capture(app->txrx->flex_sampler, app->filter.raw_capture);
        flex_sampler_set_stats_log(app->txrx->flex_sampler, app->filter.stats_log);
        flex_sampler_set_freq_correction(
            app->txrx->flex_sampler, app->filter.freq_correction_ppm);
        flex_sampler_set_weak_decode(app->txrx->flex_sampler, app->filter.weak_decode);
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
    // Synth owns and releases the speaker inside the sampler thread, so sound
    // state must be cleared independently of the app-owned speaker flag.
    flex_sampler_set_sound(app->txrx->flex_sampler, FlexSamplerSoundOff, NULL, 0.0f, NULL);
    subghz_devices_set_async_mirror_pin(app->txrx->radio_device, NULL);
    if(app->txrx->speaker_on) {
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

// CC1101 status register read: header = reg | READ(0x80) | BURST(0x40).
#define PCSG_CC1101_FREQEST 0x32U
// FREQEST step = Fxtal / 2^14 = 26e6 / 16384 Hz.
#define PCSG_FREQEST_STEP_HZ 1587

// Read the demodulator's carrier-offset estimate, in Hz (signed), or INT32_MIN
// if unavailable. SPI shares MISO with FLEX4 sampling, so a running sampler
// must be paused first.
int32_t pcsg_freqest_read_hz(POCSAGPagerApp* app) {
    furi_assert(app);
    if(flex_sampler_is_running(app->txrx->flex_sampler) &&
       !flex_sampler_is_paused(app->txrx->flex_sampler)) {
        return INT32_MIN;
    }

    const FuriHalSpiBusHandle* handle =
        radio_device_loader_is_external(app->txrx->radio_device) ?
            &furi_hal_spi_bus_handle_external :
            &furi_hal_spi_bus_handle_subghz;

    uint8_t tx[2] = {PCSG_CC1101_FREQEST | 0x80U | 0x40U, 0x00};
    uint8_t rx[2] = {0};
    furi_hal_spi_acquire(handle);
    const bool ok = furi_hal_spi_bus_trx(handle, tx, rx, sizeof(tx), 10);
    furi_hal_spi_release(handle);
    if(!ok) return INT32_MIN;

    return (int32_t)(int8_t)rx[1] * PCSG_FREQEST_STEP_HZ;
}

// Re-apply the current tuning (e.g. after the ppm correction changed). Reuses
// the proven RX stop/start path, which also re-hands the speaker.
void pcsg_retune(POCSAGPagerApp* app) {
    furi_assert(app);
    if(app->txrx->txrx_state != PCSGTxRxStateRx) return;
    pcsg_rx_end(app);
    pcsg_rx(app, app->txrx->preset->frequency);
}

#define PCSG_FREQ_AUTO_RSSI_MIN     (-80.0f)
#define PCSG_FREQ_AUTO_MIN_SAMPLES  15U
#define PCSG_FREQ_AUTO_INTERVAL_MS  3000U
// FLEX4 briefly pauses sampling after a valid frame. Several agreeing frames
// are required to retune.
#define PCSG_FREQ_AUTO_FLEX4_INTERVAL_MS 20000U
#define PCSG_FREQ_AUTO_FLEX4_STALE_MS    60000U
#define PCSG_FREQ_AUTO_FLEX4_RECENT_MS   3000U
#define PCSG_FREQ_AUTO_FLEX4_WINDOWS     3U
#define PCSG_FREQ_AUTO_MAX_EST_HZ        30000
#define PCSG_FREQ_AUTO_MAX_STEP_PPM      2
// One ppm is the finest correction the setting can express, so anything larger
// would leave a standing error the loop refuses to act on.
#define PCSG_FREQ_AUTO_DEADBAND_PPM 1

/*
 * Closed-loop RX auto-calibration: while a signal is present, average the
 * demodulator's residual carrier offset and nudge the ppm correction to null
 * it, re-tuning when it changes. Converges in a few steps, then holds, and
 * tracks slow temperature drift. Only runs on POCSAG and two-level FLEX;
 * FLEX4 uses SPI-shared pins, so FREQEST cannot be read while it samples.
 */
/*
 * FLEX4 cannot read FREQEST while sampling, because the sampler owns the pin
 * SPI needs for MISO. Pause briefly after a completed frame and read the held
 * carrier estimate immediately, before idle noise replaces it. Raw capture
 * remains uninterrupted by skipping calibration while it is enabled.
 */
static void pcsg_freq_auto_flex4_blip(POCSAGPagerApp* app) {
    if(app->filter.raw_capture) return;
    if(flex_sampler_is_locked(app->txrx->flex_sampler)) return;

    const uint32_t now = furi_get_tick();
    if(app->freq_cal_flex_frame_tick == 0 ||
       now - app->freq_cal_flex_frame_tick >
           furi_ms_to_ticks(PCSG_FREQ_AUTO_FLEX4_RECENT_MS)) {
        return;
    }
    if(app->freq_cal_blip_tick != 0 &&
       now - app->freq_cal_blip_tick < furi_ms_to_ticks(PCSG_FREQ_AUTO_FLEX4_INTERVAL_MS)) {
        return;
    }
    app->freq_cal_blip_tick = now;

    // Preserve the last carrier-derived estimate: once the frame is gone,
    // delayed reads follow noise and can keep steering through the optimum.
    flex_sampler_pause(app->txrx->flex_sampler);
    const int32_t estimate_hz = pcsg_freqest_read_hz(app);
    flex_sampler_resume(app->txrx->flex_sampler);
    app->freq_cal_flex_frame_tick = 0;

    if(estimate_hz == INT32_MIN || estimate_hz > PCSG_FREQ_AUTO_MAX_EST_HZ ||
       estimate_hz < -PCSG_FREQ_AUTO_MAX_EST_HZ) {
        return;
    }

    // A centered frame cancels older evidence. Otherwise require successive
    // completed frames to agree on which direction the receiver should move.
    if(estimate_hz <= PCSG_FREQEST_STEP_HZ && estimate_hz >= -PCSG_FREQEST_STEP_HZ) {
        app->freq_cal_sum_hz = 0;
        app->freq_cal_count = 0;
        app->freq_cal_direction = 0;
        return;
    }

    const int8_t direction = estimate_hz > 0 ? 1 : -1;
    if(app->freq_cal_direction != 0 && app->freq_cal_direction != direction) {
        app->freq_cal_sum_hz = 0;
        app->freq_cal_count = 0;
    }
    app->freq_cal_direction = direction;
    app->freq_cal_sum_hz += estimate_hz;
    app->freq_cal_count++;
}

void pcsg_freq_auto_tick(POCSAGPagerApp* app, float rssi) {
    furi_assert(app);
    if(!app->filter.freq_auto || app->txrx->txrx_state != PCSGTxRxStateRx) return;

    const uint32_t now = furi_get_tick();
    const bool flex4 = flex_sampler_is_running(app->txrx->flex_sampler);
    if(app->freq_cal_flex4 != flex4) {
        app->freq_cal_sum_hz = 0;
        app->freq_cal_count = 0;
        app->freq_cal_direction = 0;
        app->freq_cal_blip_tick = 0;
        app->freq_cal_tick = now;
        app->freq_cal_flex4 = flex4;
        app->freq_cal_flex_frames = 0;
        app->freq_cal_flex_frame_tick = 0;
    }

    if(flex4 && app->freq_cal_count > 0 && app->freq_cal_blip_tick != 0 &&
       now - app->freq_cal_blip_tick > furi_ms_to_ticks(PCSG_FREQ_AUTO_FLEX4_STALE_MS)) {
        app->freq_cal_sum_hz = 0;
        app->freq_cal_count = 0;
        app->freq_cal_direction = 0;
    }

    if(flex4) {
        const uint32_t completed_frames =
            flex_sampler_completed_frames(app->txrx->flex_sampler);
        if(completed_frames < app->freq_cal_flex_frames) {
            app->freq_cal_flex_frames = completed_frames;
            app->freq_cal_flex_frame_tick = 0;
        } else if(completed_frames > app->freq_cal_flex_frames) {
            app->freq_cal_flex_frames = completed_frames;
            app->freq_cal_flex_frame_tick = now;
        }
        pcsg_freq_auto_flex4_blip(app);
    } else if(rssi > PCSG_FREQ_AUTO_RSSI_MIN) {
        const int32_t hz = pcsg_freqest_read_hz(app);
        if(hz != INT32_MIN) {
            app->freq_cal_sum_hz += hz;
            app->freq_cal_count++;
        }
    }

    const uint16_t required_samples =
        flex4 ? PCSG_FREQ_AUTO_FLEX4_WINDOWS : PCSG_FREQ_AUTO_MIN_SAMPLES;
    if(app->freq_cal_count < required_samples ||
       now - app->freq_cal_tick < furi_ms_to_ticks(PCSG_FREQ_AUTO_INTERVAL_MS)) {
        return;
    }

    const int32_t mean_hz = app->freq_cal_sum_hz / (int32_t)app->freq_cal_count;
    const int32_t freq_mhz = (int32_t)(app->txrx->preset->frequency / 1000000U);
    // Residual above the LO reads positive; raise the LO (more ppm) to center it.
    // Round to the nearest ppm rather than truncating: one ppm is 930 Hz at
    // 929 MHz, so truncation discarded up to a whole ppm of the averaged
    // estimate and left a correspondingly large standing error.
    int32_t delta_ppm = 0;
    if(freq_mhz > 0) {
        const int32_t half = freq_mhz / 2;
        delta_ppm = (mean_hz >= 0 ? mean_hz + half : mean_hz - half) / freq_mhz;
    }
    app->freq_cal_sum_hz = 0;
    app->freq_cal_count = 0;
    app->freq_cal_direction = 0;
    app->freq_cal_tick = now;

    if(delta_ppm <= PCSG_FREQ_AUTO_DEADBAND_PPM && delta_ppm >= -PCSG_FREQ_AUTO_DEADBAND_PPM) {
        return;
    }
    if(flex4) {
        delta_ppm = CLAMP(
            delta_ppm, PCSG_FREQ_AUTO_MAX_STEP_PPM, -PCSG_FREQ_AUTO_MAX_STEP_PPM);
    }
    int32_t ppm = app->filter.freq_correction_ppm + delta_ppm;
    ppm = CLAMP(ppm, PCSG_FREQ_PPM_MAX, -PCSG_FREQ_PPM_MAX);
    if(ppm == app->filter.freq_correction_ppm) return;

    app->filter.freq_correction_ppm = ppm;
    FURI_LOG_I(TAG, "Freq auto: %+ld ppm (residual %+ld Hz)", (long)ppm, (long)mean_hz);
    pcsg_retune(app);
    pcsg_settings_save(app);
}

static void pcsg_tx_release_encoder(POCSAGPagerApp* app) {
    pocsag_tx_encoder_free(app->tx_encoder);
    flex_tx_encoder_free(app->flex_tx_encoder);
    app->tx_encoder = NULL;
    app->flex_tx_encoder = NULL;
}

PCSGTxStartStatus pcsg_tx_start(POCSAGPagerApp* app) {
    furi_assert(app);
    if(app->txrx->txrx_state == PCSGTxRxStateRx) pcsg_rx_end(app);
    if(app->txrx->txrx_state == PCSGTxRxStateTx) pcsg_tx_stop(app);
    if(app->txrx->txrx_state == PCSGTxRxStateSleep) return PCSGTxStartInvalid;

    // Global ppm correction (same as RX) plus the TX per-shot Hz nudge.
    const uint32_t ppm_corrected =
        pcsg_correct_frequency(app->filter.freq_correction_ppm, app->tx_config.frequency);
    const int64_t corrected_frequency =
        (int64_t)ppm_corrected + app->tx_config.frequency_correction_hz;
    if(corrected_frequency <= 0 || corrected_frequency > UINT32_MAX) {
        return PCSGTxStartInvalid;
    }
    const uint32_t tx_frequency = (uint32_t)corrected_frequency;

    const bool flex = app->tx_config.protocol == PagerTxProtocolFlex;
    uint8_t* preset = pocsag_tx_get_preset(flex ? FLEX_TX_BAUD : app->tx_config.baud);
    if(!preset || !subghz_devices_is_frequency_valid(app->txrx->radio_device, tx_frequency)) {
        return PCSGTxStartInvalid;
    }

    bool prepared;
    if(flex) {
        app->flex_tx_encoder = flex_tx_encoder_alloc();
        prepared = app->flex_tx_encoder &&
                   flex_tx_encoder_prepare(app->flex_tx_encoder, &app->tx_config);
    } else {
        app->tx_encoder = pocsag_tx_encoder_alloc();
        prepared = app->tx_encoder &&
                   pocsag_tx_encoder_prepare(app->tx_encoder, &app->tx_config);
    }
    if(!prepared) {
        pcsg_tx_release_encoder(app);
        return PCSGTxStartInvalid;
    }

    subghz_devices_reset(app->txrx->radio_device);
    subghz_devices_idle(app->txrx->radio_device);
    subghz_devices_load_preset(app->txrx->radio_device, FuriHalSubGhzPresetCustom, preset);
    subghz_devices_set_frequency(app->txrx->radio_device, tx_frequency);

    FuriHalSubGhzAsyncTxCallback tx_callback =
        flex ? flex_tx_encoder_yield : pocsag_tx_encoder_yield;
    void* tx_context = flex ? (void*)app->flex_tx_encoder : (void*)app->tx_encoder;
    if(!subghz_devices_start_async_tx(app->txrx->radio_device, tx_callback, tx_context)) {
        subghz_devices_idle(app->txrx->radio_device);
        pcsg_tx_release_encoder(app);
        return PCSGTxStartRestricted;
    }

    app->txrx->txrx_state = PCSGTxRxStateTx;
    return PCSGTxStartOk;
}

bool pcsg_tx_complete(POCSAGPagerApp* app) {
    furi_assert(app);
    return app->txrx->txrx_state == PCSGTxRxStateTx &&
           subghz_devices_is_async_complete_tx(app->txrx->radio_device);
}

void pcsg_tx_stop(POCSAGPagerApp* app) {
    furi_assert(app);
    if(app->txrx->txrx_state != PCSGTxRxStateTx) return;
    subghz_devices_stop_async_tx(app->txrx->radio_device);
    subghz_devices_idle(app->txrx->radio_device);
    pcsg_tx_release_encoder(app);
    app->txrx->txrx_state = PCSGTxRxStateIDLE;
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
