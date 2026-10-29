#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <lib/subghz/protocols/base.h>

/** Highest FLEX capcode accepted by the capcode filter (NumberInput is int32). */
#define PCSG_CAPCODE_MAX 2147483647U

typedef enum {
    PCSGTypeFilterAll,
    PCSGTypeFilterAlpha,
    PCSGTypeFilterAlphaNumeric,
    PCSGTypeFilterNoTones,
    PCSGTypeFilterCount,
} PCSGTypeFilter;

/** Off / Exact / Range filter, used for capcodes and FLEX frames. */
typedef enum {
    PCSGCapcodeFilterOff,
    PCSGCapcodeFilterExact,
    PCSGCapcodeFilterRange,
    PCSGCapcodeFilterCount,
} PCSGCapcodeFilter;

/** Highest POCSAG RIC (21-bit address). */
#define PCSG_RIC_MAX 2097151U

/** Highest FLEX frame number (128 frames per 4-minute cycle). */
#define PCSG_FRAME_MAX 127U

/** Field edited by the number entry scene (its scene state). */
typedef enum {
    PCSGNumberFieldCapcodeMin,
    PCSGNumberFieldCapcodeMax,
    PCSGNumberFieldFrameMin,
    PCSGNumberFieldFrameMax,
    PCSGNumberFieldRicMin,
    PCSGNumberFieldRicMax,
    PCSGNumberFieldFreqPpm,
} PCSGNumberField;

/** FLEX phase to show: all, or one of A-D. */
typedef enum {
    PCSGPhaseFilterAll,
    PCSGPhaseFilterA,
    PCSGPhaseFilterB,
    PCSGPhaseFilterC,
    PCSGPhaseFilterD,
    PCSGPhaseFilterCount,
} PCSGPhaseFilter;

/** FLEX duplicate handling in the live viewer. */
typedef enum {
    PCSGFlexDupsHide,
    PCSGFlexDupsShow,
    /** Show copies to different capcodes, but hide same-cap retransmits. */
    PCSGFlexDupsCaps,
    PCSGFlexDupsCount,
} PCSGFlexDups;

/** Sound/vibration notification for a new message; LED control is separate. */
typedef enum {
    PCSGAlertBoth,
    PCSGAlertSound,
    PCSGAlertVibro,
    PCSGAlertOff,
    PCSGAlertCount,
} PCSGAlert;

/** RGB color used by the periodic receive indicator. */
typedef enum {
    PCSGLedColorCyan,
    PCSGLedColorBlue,
    PCSGLedColorGreen,
    PCSGLedColorYellow,
    PCSGLedColorMagenta,
    PCSGLedColorRed,
    PCSGLedColorWhite,
    PCSGLedColorCount,
} PCSGLedColor;

/** Signal sound while receiving; saved as SignalSound (1 was the old "On"). */
typedef enum {
    PCSGSoundOff,
    // The data line drives the speaker directly: the true signal, full volume.
    PCSGSoundRaw,
    // FLEX4 levels play as pitches at the tone volume; other presets use Raw.
    PCSGSoundTone,
    // Like Tone but from the decoder's recovered symbols: clean, no noise.
    PCSGSoundSynth,
    PCSGSoundCount,
} PCSGSound;

typedef enum {
    PCSGToneVolumeLow,
    PCSGToneVolumeMedium,
    PCSGToneVolumeHigh,
    PCSGToneVolumeCount,
} PCSGToneVolume;

/** Tone mode: lowest pitch (300, 600 or 1200 Hz). */
typedef enum {
    PCSGTonePitchLow,
    PCSGTonePitchMedium,
    PCSGTonePitchHigh,
    PCSGTonePitchCount,
} PCSGTonePitch;

/** Tone mode: step between levels, a quarter, half or all of the lowest pitch. */
typedef enum {
    PCSGToneSpreadNarrow,
    PCSGToneSpreadNormal,
    PCSGToneSpreadWide,
    PCSGToneSpreadCount,
} PCSGToneSpread;

typedef struct {
    PCSGTypeFilter type_filter;
    bool show_binary;
    PCSGCapcodeFilter capcode_filter;
    uint32_t capcode_min;
    uint32_t capcode_max;
    PCSGPhaseFilter phase_filter;
    PCSGCapcodeFilter frame_filter;
    uint32_t frame_min;
    uint32_t frame_max;
    // POCSAG only; FLEX capcodes have their own filter.
    PCSGCapcodeFilter ric_filter;
    uint32_t ric_min;
    uint32_t ric_max;
    PCSGFlexDups flex_dups;
    bool hide_encrypted;
    // Discard FLEX messages that could not be fully decoded (off by default).
    bool hide_partial;
    // Log to one temp file destroyed on exit instead of a kept log per session.
    bool log_temp;
    PCSGAlert alert;
    bool receive_led;
    PCSGLedColor receive_led_color;
    bool decode_led;
    PCSGSound signal_sound;
    PCSGToneVolume tone_volume;
    PCSGTonePitch tone_pitch;
    PCSGToneSpread tone_spread;
    // Crystal offset compensation applied to both RX and TX tuning, in ppm.
    // One value works on every band; +ppm corrects a carrier that runs low.
    int32_t freq_correction_ppm;
    // Auto-calibrate the ppm from the demodulator while receiving (RX only).
    bool freq_auto;
    // Record the raw FLEX4 sample stream to SD (debug / offline analysis).
    bool raw_capture;
    // Write counter-only FLEX4 diagnostics to /ext/pocsag/stats.txt.
    bool stats_log;
    // FLEX4 soft-decision (Chase) recovery of weak words; off by default, and
    // allocates nothing in the decoder unless enabled.
    bool weak_decode;
} PCSGFilterSettings;

/** Largest crystal correction offered, in ppm. */
#define PCSG_FREQ_PPM_MAX 100

/** Apply a ppm correction to a nominal frequency: nominal * (1 + ppm/1e6). */
uint32_t pcsg_correct_frequency(int32_t ppm, uint32_t nominal);

typedef struct POCSAGPagerApp POCSAGPagerApp;

/** Restore receiver and transmitter configuration saved on the SD card. */
void pcsg_settings_load(POCSAGPagerApp* app);

/** Save receiver and transmitter configuration to the SD card. */
void pcsg_settings_save(POCSAGPagerApp* app);

/** Whether a decoded message passes the filters and should enter history. */
bool pcsg_filter_accepts(
    const PCSGFilterSettings* filter,
    SubGhzProtocolDecoderBase* decoder_base);
