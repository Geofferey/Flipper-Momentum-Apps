#include "flex_sampler.h"

#include "protocols/flex.h"

#include <stdio.h>
#include <furi.h>
#include <furi_hal_bus.h>
#include <furi_hal_interrupt.h>
#include <furi_hal_resources.h>
#include <furi_hal_speaker.h>
#include <furi_hal_rtc.h>
#include <stm32wbxx_ll_tim.h>

#define TAG "FlexSampler"

// 320 ms of samples (2 KB packed): the decoder runs every page callback of a
// frame back to back while the next frame's sync is already arriving.
#define FLEX_SAMPLE_BUFFER_SIZE 8192U
#define FLEX_SAMPLE_BUFFER_MASK (FLEX_SAMPLE_BUFFER_SIZE - 1U)
#define FLEX_STATS_LOG_INTERVAL_MS 1000U

#include <storage/storage.h>

// Optional raw capture (Settings > Raw Capture) for offline analysis: the
// RX_HARD_DATA stream, four 2-bit samples per byte with the earliest in the
// top bits, at FLEX_4FSK_SAMPLE_RATE_HZ. One bounded window per session, to a
// timestamped file; used to tune clock recovery against real signals.
// On the SD card next to the logs. Captures run until the session ends or the
// cap (~3.8 MB at 25.6 kHz) is reached.
#define FLEX_CAPTURE_DIR      EXT_PATH("pocsag/captures")
#define FLEX_CAPTURE_WINDOW_S 600U
#define FLEX_CAPTURE_CHUNK    512U
#define FLEX_STATS_PATH       EXT_PATH("pocsag/stats.txt")
#define FLEX_STATS_FILE_INTERVAL_MS 30000U

typedef struct {
    Storage* storage;
    File* file;
    bool open;
    uint32_t remaining;
    uint8_t packed;
    uint8_t packed_count;
    uint16_t length;
    uint8_t chunk[FLEX_CAPTURE_CHUNK];
} FlexCapture;

typedef struct {
    Storage* storage;
    File* file;
    bool open;
    uint32_t last_write_tick;
} FlexStatsFile;

static void flex_capture_open(FlexCapture* capture) {
    memset(capture, 0, sizeof(FlexCapture));
    capture->storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(capture->storage, EXT_PATH("pocsag"));
    storage_simply_mkdir(capture->storage, FLEX_CAPTURE_DIR);

    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    FuriString* path = furi_string_alloc_printf(
        "%s/flex4_%04u%02u%02u_%02u%02u%02u.bin",
        FLEX_CAPTURE_DIR,
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second);

    capture->file = storage_file_alloc(capture->storage);
    if(storage_file_open(
           capture->file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        capture->open = true;
        capture->remaining = FLEX_CAPTURE_WINDOW_S * FLEX_4FSK_SAMPLE_RATE_HZ;
        FURI_LOG_I(
            "FlexSampler", "Capturing %us to %s", FLEX_CAPTURE_WINDOW_S, furi_string_get_cstr(path));
    } else {
        FURI_LOG_E("FlexSampler", "Capture open failed");
    }
    furi_string_free(path);
}

static void flex_capture_flush(FlexCapture* capture) {
    if(capture->length) storage_file_write(capture->file, capture->chunk, capture->length);
    capture->length = 0;
}

static void flex_capture_push(FlexCapture* capture, uint8_t sample) {
    if(!capture->open || !capture->remaining) return;
    capture->packed = (capture->packed << 2) | (sample & 0x03U);
    if(++capture->packed_count == 4U) {
        capture->chunk[capture->length++] = capture->packed;
        capture->packed_count = 0;
        if(capture->length == FLEX_CAPTURE_CHUNK) flex_capture_flush(capture);
    }
    if(--capture->remaining == 0) {
        flex_capture_flush(capture);
        storage_file_close(capture->file);
        capture->open = false;
        FURI_LOG_I("FlexSampler", "Capture complete");
    }
}

static void flex_capture_close(FlexCapture* capture) {
    if(capture->open) {
        flex_capture_flush(capture);
        storage_file_close(capture->file);
    }
    if(capture->file) storage_file_free(capture->file);
    furi_record_close(RECORD_STORAGE);
}

struct FlexSampler {
    FuriThread* thread;
    SubGhzProtocolDecoderBase* decoder;
    const GpioPin* data_msb;
    const GpioPin* data_lsb;
    volatile bool running;
    volatile bool paused;
    volatile bool overrun;
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile uint32_t sample_count;
    volatile uint32_t dropped_samples;
    volatile uint32_t symbol_count[4];
    uint32_t transition_count;
    uint8_t last_symbol;
    bool last_symbol_valid;
    // Allocated only while receiving. 2-bit samples packed four per byte,
    // earliest in the top bits.
    uint8_t* samples;
    // Signal sound, see flex_sampler_set_sound().
    volatile FlexSamplerSound sound;
    const GpioPin* volatile sound_pin;
    float sound_volume;
    // Tone mode pitch per signal level, lowest frequency first.
    float sound_tones[4];
    uint8_t sound_level;
    // Tone squelch: the thread sets this from the decoder's lock state so the
    // tone only plays during a frame, not on the noise between pages. 0xFF in
    // sound_level marks the speaker as currently silenced.
    volatile bool sound_gate;
    // Record the raw sample stream this session (see flex_sampler_set_capture).
    bool capture_enable;
    // Write counter-only snapshots to /ext/pocsag/stats.txt.
    bool stats_log_enable;
    bool stats_session_started;
    uint16_t stats_segment;
    int32_t freq_correction_ppm;
    bool weak_decode_enable;
};

#define FLEX_SOUND_SILENCED 0xFFU

// RX_HARD_DATA symbol to level, as in the FLEX decoder.
static const uint8_t flex_sampler_levels[] = {1U, 0U, 2U, 3U};

static void flex_stats_file_open(FlexStatsFile* stats_file, FlexSampler* instance) {
    memset(stats_file, 0, sizeof(FlexStatsFile));
    stats_file->storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(stats_file->storage, EXT_PATH("pocsag"));
    stats_file->file = storage_file_alloc(stats_file->storage);
    const FS_OpenMode open_mode =
        instance->stats_session_started ? FSOM_OPEN_APPEND : FSOM_CREATE_ALWAYS;
    stats_file->open = storage_file_open(
        stats_file->file, FLEX_STATS_PATH, FSAM_WRITE, open_mode);
    if(stats_file->open) {
        instance->stats_session_started = true;
        instance->stats_segment++;
        char header[128];
        const int length = snprintf(
            header,
            sizeof(header),
            "# FLEX4 segment %u; counters reset; no message content\n"
            "weak_decode=%u raw_capture=%u ppm=%ld\n\n",
            instance->stats_segment,
            instance->weak_decode_enable,
            instance->capture_enable,
            (long)instance->freq_correction_ppm);
        if(length > 0) {
            storage_file_write(
                stats_file->file, header, MIN((size_t)length, sizeof(header) - 1U));
        }
        storage_file_sync(stats_file->file);
        stats_file->last_write_tick = furi_get_tick();
    } else {
        FURI_LOG_E(TAG, "Stats file open failed");
    }
}

static void flex_stats_file_write(
    FlexStatsFile* stats_file,
    const FlexSampler* instance,
    const FlexDecoderStats* stats) {
    if(!stats_file->open) return;

    char buffer[768];
    const int length = snprintf(
        buffer,
        sizeof(buffer),
        "n=%lu ppm=%ld h=%lu/%lu/%lu/%lu tr=%lu drop=%lu sync=%lu fiw=%lu/%lu soft=%lu frame=%lu page=%lu enc=%lu fix=%lu\n"
        "words=%lu bad=%lu soft=%lu pages_soft=%lu biw_bad=%lu hdr_bad=%lu long=%lu partial=%lu\n"
        "bad A/B/C/D=%lu/%lu/%lu/%lu short=%lu range=%lu parse=%lu\n"
        "frag first/more/last=%lu/%lu/%lu joined=%lu orphan=%lu timeout=%lu stop=%lu gap=%lu dup=%lu kreject=%lu sigbad=%lu ksum_bad=%lu\n\n",
        instance->sample_count,
        (long)instance->freq_correction_ppm,
        instance->symbol_count[0],
        instance->symbol_count[1],
        instance->symbol_count[2],
        instance->symbol_count[3],
        instance->transition_count,
        instance->dropped_samples,
        stats->sync_detected,
        stats->fiw_valid,
        stats->fiw_invalid,
        stats->fiw_chase,
        stats->frames_completed,
        stats->pages_decoded,
        stats->pages_encrypted,
        stats->pages_repaired,
        stats->words_total,
        stats->words_bad,
        stats->words_chase,
        stats->pages_chase,
        stats->phases_bad_biw,
        stats->pages_bad_header,
        stats->pages_long_address,
        stats->pages_incomplete,
        stats->words_bad_phase[0],
        stats->words_bad_phase[1],
        stats->words_bad_phase[2],
        stats->words_bad_phase[3],
        stats->pages_short_instruction,
        stats->pages_bad_range,
        stats->pages_parse_failed,
        stats->fragments_first,
        stats->fragments_more,
        stats->fragments_last,
        stats->joined_complete,
        stats->joined_orphan,
        stats->joined_timeout,
        stats->joined_session_flush,
        stats->joined_sequence_gap,
        stats->joined_duplicate,
        stats->joined_checksum_rejected,
        stats->joined_signature_bad,
        stats->pages_checksum_bad);
    if(length > 0) {
        storage_file_write(
            stats_file->file, buffer, MIN((size_t)length, sizeof(buffer) - 1U));
        storage_file_sync(stats_file->file);
        stats_file->last_write_tick = furi_get_tick();
    }
}

static void flex_stats_file_close(FlexStatsFile* stats_file) {
    if(stats_file->open) storage_file_close(stats_file->file);
    if(stats_file->file) storage_file_free(stats_file->file);
    if(stats_file->storage) furi_record_close(RECORD_STORAGE);
}

static inline uint8_t flex_sampler_get(const uint8_t* samples, uint16_t index) {
    return (samples[index >> 2] >> ((3U - (index & 3U)) * 2U)) & 0x03U;
}

static inline void flex_sampler_put(uint8_t* samples, uint16_t index, uint8_t sample) {
    const uint8_t shift = (3U - (index & 3U)) * 2U;
    samples[index >> 2] = (samples[index >> 2] & ~(0x03U << shift)) | (sample << shift);
}

static void flex_sampler_timer_isr(void* context) {
    FlexSampler* instance = context;
    const uint16_t next = (instance->head + 1U) & FLEX_SAMPLE_BUFFER_MASK;
    if(next == instance->tail) {
        instance->overrun = true;
        instance->dropped_samples++;
    } else {
        const uint8_t sample =
            (furi_hal_gpio_read(instance->data_msb) ? 2U : 0U) |
            (furi_hal_gpio_read(instance->data_lsb) ? 1U : 0U);
        if(instance->sound == FlexSamplerSoundRaw) {
            furi_hal_gpio_write(instance->sound_pin, sample & 2U);
        } else if(instance->sound == FlexSamplerSoundTone) {
            if(instance->sound_gate) {
                // Retune only when the level changes (at most at the symbol rate).
                const uint8_t level = flex_sampler_levels[sample];
                if(level != instance->sound_level) {
                    instance->sound_level = level;
                    furi_hal_speaker_start(instance->sound_tones[level], instance->sound_volume);
                }
            } else if(instance->sound_level != FLEX_SOUND_SILENCED) {
                // Squelch closed: silence the speaker (volume 0 stops the timer).
                instance->sound_level = FLEX_SOUND_SILENCED;
                furi_hal_speaker_start(instance->sound_tones[0], 0.0f);
            }
        }
        flex_sampler_put(instance->samples, instance->head, sample);
        instance->sample_count++;
        instance->symbol_count[sample]++;
        instance->head = next;
    }
    LL_TIM_ClearFlag_UPDATE(TIM2);
}

static int32_t flex_sampler_thread(void* context) {
    FlexSampler* instance = context;
    uint32_t last_log_tick = furi_get_tick();
    FlexStatsFile stats_file = {0};
    if(instance->stats_log_enable) flex_stats_file_open(&stats_file, instance);
    FlexCapture* capture = NULL;
    if(instance->capture_enable) {
        capture = malloc(sizeof(FlexCapture));
        if(capture) {
            flex_capture_open(capture);
        } else {
            FURI_LOG_E(TAG, "Raw Capture allocation failed");
        }
    }
    // Synth mode: this thread owns the speaker so the decoder (fed here) can
    // play a tone per recovered symbol. Disable synth if the speaker is busy.
    bool synth_speaker = false;
    if(instance->sound == FlexSamplerSoundSynth) {
        synth_speaker = furi_hal_speaker_acquire(30);
        if(synth_speaker) {
            subghz_protocol_decoder_flex_set_synth(
                instance->decoder, instance->sound_tones, instance->sound_volume, true);
        } else {
            subghz_protocol_decoder_flex_set_synth(instance->decoder, NULL, 0.0f, false);
        }
    }
    while(instance->running) {
        if(instance->overrun) {
            FURI_CRITICAL_ENTER();
            instance->tail = instance->head;
            instance->overrun = false;
            FURI_CRITICAL_EXIT();
            instance->decoder->protocol->decoder->reset(instance->decoder);
        }

        bool processed = false;
        while(instance->tail != instance->head) {
            const uint8_t sample = flex_sampler_get(instance->samples, instance->tail);
            instance->tail = (instance->tail + 1U) & FLEX_SAMPLE_BUFFER_MASK;
            if(instance->last_symbol_valid && sample != instance->last_symbol) {
                instance->transition_count++;
            }
            instance->last_symbol = sample;
            instance->last_symbol_valid = true;
            if(capture) flex_capture_push(capture, sample);
            subghz_protocol_decoder_flex_feed_4fsk_sample(instance->decoder, sample);
            processed = true;
        }

        // Open the tone squelch only while the decoder is locked onto a frame.
        instance->sound_gate = subghz_protocol_decoder_flex_is_locked(instance->decoder);

        const uint32_t now = furi_get_tick();
        if(now - last_log_tick >= furi_ms_to_ticks(FLEX_STATS_LOG_INTERVAL_MS)) {
            FlexDecoderStats decoder_stats;
            subghz_protocol_decoder_flex_get_stats(instance->decoder, &decoder_stats);
            FURI_LOG_I(
                TAG,
                "n=%lu ppm=%ld h=%lu/%lu/%lu/%lu tr=%lu drop=%lu sync=%lu fiw=%lu/%lu soft=%lu frame=%lu page=%lu enc=%lu fix=%lu",
                instance->sample_count,
                (long)instance->freq_correction_ppm,
                instance->symbol_count[0],
                instance->symbol_count[1],
                instance->symbol_count[2],
                instance->symbol_count[3],
                instance->transition_count,
                instance->dropped_samples,
                decoder_stats.sync_detected,
                decoder_stats.fiw_valid,
                decoder_stats.fiw_invalid,
                decoder_stats.fiw_chase,
                decoder_stats.frames_completed,
                decoder_stats.pages_decoded,
                decoder_stats.pages_encrypted,
                decoder_stats.pages_repaired);
            // Where pages are lost: word error rate, then each drop reason.
            FURI_LOG_I(
                TAG,
                "words=%lu bad=%lu soft=%lu pages_soft=%lu biw_bad=%lu hdr_bad=%lu long=%lu partial=%lu",
                decoder_stats.words_total,
                decoder_stats.words_bad,
                decoder_stats.words_chase,
                decoder_stats.pages_chase,
                decoder_stats.phases_bad_biw,
                decoder_stats.pages_bad_header,
                decoder_stats.pages_long_address,
                decoder_stats.pages_incomplete);
            FURI_LOG_I(
                TAG,
                "bad A/B/C/D=%lu/%lu/%lu/%lu short=%lu range=%lu parse=%lu",
                decoder_stats.words_bad_phase[0],
                decoder_stats.words_bad_phase[1],
                decoder_stats.words_bad_phase[2],
                decoder_stats.words_bad_phase[3],
                decoder_stats.pages_short_instruction,
                decoder_stats.pages_bad_range,
                decoder_stats.pages_parse_failed);
            FURI_LOG_I(
                TAG,
                "frag first/more/last=%lu/%lu/%lu joined=%lu orphan=%lu timeout=%lu stop=%lu gap=%lu dup=%lu kreject=%lu sigbad=%lu ksum_bad=%lu",
                decoder_stats.fragments_first,
                decoder_stats.fragments_more,
                decoder_stats.fragments_last,
                decoder_stats.joined_complete,
                decoder_stats.joined_orphan,
                decoder_stats.joined_timeout,
                decoder_stats.joined_session_flush,
                decoder_stats.joined_sequence_gap,
                decoder_stats.joined_duplicate,
                decoder_stats.joined_checksum_rejected,
                decoder_stats.joined_signature_bad,
                decoder_stats.pages_checksum_bad);
            if(stats_file.open &&
               now - stats_file.last_write_tick >=
                   furi_ms_to_ticks(FLEX_STATS_FILE_INTERVAL_MS)) {
                flex_stats_file_write(&stats_file, instance, &decoder_stats);
            }
            last_log_tick = now;
        }
        if(!processed) furi_delay_tick(1);
    }
    if(synth_speaker) {
        subghz_protocol_decoder_flex_set_synth(instance->decoder, NULL, 0.0f, false);
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
    // Do not silently discard a long message whose final fragment never
    // arrived. This callback still runs on the sampler thread while history
    // and logging are alive; the Partial Msgs setting controls only display.
    subghz_protocol_decoder_flex_flush_fragments(instance->decoder);
    if(capture) {
        flex_capture_close(capture);
        free(capture);
    }
    if(stats_file.open) {
        FlexDecoderStats final_stats;
        subghz_protocol_decoder_flex_get_stats(instance->decoder, &final_stats);
        flex_stats_file_write(&stats_file, instance, &final_stats);
    }
    flex_stats_file_close(&stats_file);
    return 0;
}

static void flex_sampler_timer_start(FlexSampler* instance) {
    furi_hal_bus_enable(FuriHalBusTIM2);

    // 64 MHz timer clock divided down to FLEX_4FSK_SAMPLE_RATE_HZ.
    LL_TIM_InitTypeDef timer_config = {
        .Prescaler = 0,
        .CounterMode = LL_TIM_COUNTERMODE_UP,
        .Autoreload = (64000000U / FLEX_4FSK_SAMPLE_RATE_HZ) - 1U,
    };
    LL_TIM_Init(TIM2, &timer_config);
    LL_TIM_SetClockSource(TIM2, LL_TIM_CLOCKSOURCE_INTERNAL);
    LL_TIM_DisableCounter(TIM2);
    LL_TIM_SetCounter(TIM2, 0);
    LL_TIM_ClearFlag_UPDATE(TIM2);
    furi_hal_interrupt_set_isr(FuriHalInterruptIdTIM2, flex_sampler_timer_isr, instance);
    LL_TIM_EnableIT_UPDATE(TIM2);
    LL_TIM_EnableCounter(TIM2);
}

static void flex_sampler_timer_stop(void) {
    FURI_CRITICAL_ENTER();
    LL_TIM_DisableCounter(TIM2);
    LL_TIM_DisableIT_UPDATE(TIM2);
    furi_hal_interrupt_set_isr(FuriHalInterruptIdTIM2, NULL, NULL);
    furi_hal_bus_disable(FuriHalBusTIM2);
    FURI_CRITICAL_EXIT();
}

FlexSampler* flex_sampler_alloc(
    SubGhzProtocolDecoderBase* decoder,
    const SubGhzDevice* radio_device,
    bool external_radio) {
    furi_assert(decoder);
    furi_assert(radio_device);

    // The thread and sample buffer only exist while receiving, so POCSAG and
    // two-level FLEX sessions do not pay for them.
    FlexSampler* instance = calloc(1, sizeof(FlexSampler));
    instance->decoder = decoder;
    instance->data_msb = subghz_devices_get_data_gpio(radio_device);
    instance->data_lsb = external_radio ? &gpio_ext_pa6 : &gpio_spi_r_miso;
    return instance;
}

void flex_sampler_free(FlexSampler* instance) {
    furi_assert(instance);
    flex_sampler_stop(instance);
    free(instance);
}

void flex_sampler_start(FlexSampler* instance) {
    furi_assert(instance);
    furi_assert(!instance->running);

    instance->samples = calloc(1, FLEX_SAMPLE_BUFFER_SIZE / 4U);
    // The decoder callback chain (history, storage) runs on this thread.
    instance->thread =
        furi_thread_alloc_ex("Flex4Sampler", 4096, flex_sampler_thread, instance);
    furi_thread_set_priority(instance->thread, FuriThreadPriorityHigh);

    instance->head = 0;
    instance->tail = 0;
    instance->overrun = false;
    instance->sample_count = 0;
    instance->dropped_samples = 0;
    memset((void*)instance->symbol_count, 0, sizeof(instance->symbol_count));
    instance->transition_count = 0;
    instance->last_symbol = 0;
    instance->last_symbol_valid = false;
    instance->paused = false;
    subghz_protocol_decoder_flex_reset_stats(instance->decoder);
    instance->decoder->protocol->decoder->reset(instance->decoder);
    furi_hal_gpio_init(instance->data_msb, GpioModeInput, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_init(instance->data_lsb, GpioModeInput, GpioPullNo, GpioSpeedVeryHigh);

    instance->running = true;
    furi_thread_start(instance->thread);
    flex_sampler_timer_start(instance);
}

void flex_sampler_stop(FlexSampler* instance) {
    furi_assert(instance);
    if(!instance->running) return;

    if(!instance->paused) flex_sampler_timer_stop();
    instance->paused = false;
    instance->running = false;
    furi_thread_join(instance->thread);
    furi_thread_free(instance->thread);
    instance->thread = NULL;
    free(instance->samples);
    instance->samples = NULL;
    instance->head = 0;
    instance->tail = 0;
}

void flex_sampler_pause(FlexSampler* instance) {
    furi_assert(instance);
    furi_assert(instance->running);
    furi_assert(!instance->paused);
    flex_sampler_timer_stop();
    instance->paused = true;
}

void flex_sampler_resume(FlexSampler* instance) {
    furi_assert(instance);
    furi_assert(instance->running);
    furi_assert(instance->paused);
    furi_hal_gpio_init(instance->data_msb, GpioModeInput, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_init(instance->data_lsb, GpioModeInput, GpioPullNo, GpioSpeedVeryHigh);
    instance->paused = false;
    flex_sampler_timer_start(instance);
}

void flex_sampler_set_sound(
    FlexSampler* instance,
    FlexSamplerSound sound,
    const GpioPin* pin,
    float volume,
    const float tones[4]) {
    furi_assert(instance);
    furi_assert((sound != FlexSamplerSoundTone && sound != FlexSamplerSoundSynth) || tones);
    instance->sound = FlexSamplerSoundOff;
    instance->sound_pin = pin;
    instance->sound_volume = volume;
    if(tones) memcpy(instance->sound_tones, tones, sizeof(instance->sound_tones));
    instance->sound_level = UINT8_MAX;
    // Synth is played by the decoder, from the thread that owns the speaker.
    // The thread acquires the speaker at start and releases it on exit.
    subghz_protocol_decoder_flex_set_synth(
        instance->decoder, tones, volume, sound == FlexSamplerSoundSynth);
    instance->sound = sound;
}

void flex_sampler_set_capture(FlexSampler* instance, bool enable) {
    furi_assert(instance);
    instance->capture_enable = enable;
}

void flex_sampler_set_stats_log(FlexSampler* instance, bool enable) {
    furi_assert(instance);
    if(enable && !instance->stats_log_enable) {
        instance->stats_session_started = false;
        instance->stats_segment = 0;
    }
    instance->stats_log_enable = enable;
}

void flex_sampler_set_freq_correction(FlexSampler* instance, int32_t ppm) {
    furi_assert(instance);
    instance->freq_correction_ppm = ppm;
}

void flex_sampler_set_weak_decode(FlexSampler* instance, bool enable) {
    furi_assert(instance);
    instance->weak_decode_enable = enable;
    subghz_protocol_decoder_flex_set_weak_decode(instance->decoder, enable);
}

bool flex_sampler_is_running(const FlexSampler* instance) {
    furi_assert(instance);
    return instance->running;
}

bool flex_sampler_is_paused(const FlexSampler* instance) {
    furi_assert(instance);
    return instance->paused;
}

bool flex_sampler_is_locked(const FlexSampler* instance) {
    furi_assert(instance);
    // Cached by the sampler thread each pass; reading it costs no SPI, unlike
    // asking the radio, so it is safe to poll while sampling.
    return instance->running && instance->sound_gate;
}

uint32_t flex_sampler_completed_frames(const FlexSampler* instance) {
    furi_assert(instance);
    FlexDecoderStats stats;
    subghz_protocol_decoder_flex_get_stats(instance->decoder, &stats);
    return stats.frames_completed;
}
