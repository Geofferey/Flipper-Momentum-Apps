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

/** Notification for a new message; the LED always blinks. */
typedef enum {
    PCSGAlertBoth,
    PCSGAlertSound,
    PCSGAlertVibro,
    PCSGAlertOff,
    PCSGAlertCount,
} PCSGAlert;

/** Signal sound while receiving; saved as SignalSound (1 was the old "On"). */
typedef enum {
    PCSGSoundOff,
    // The data line drives the speaker directly: the true signal, full volume.
    PCSGSoundRaw,
    // FLEX4 levels play as pitches at the tone volume; other presets use Raw.
    PCSGSoundTone,
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
    bool hide_duplicates;
    bool hide_encrypted;
    // Log to one temp file destroyed on exit instead of a kept log per session.
    bool log_temp;
    PCSGAlert alert;
    PCSGSound signal_sound;
    PCSGToneVolume tone_volume;
    PCSGTonePitch tone_pitch;
    PCSGToneSpread tone_spread;
} PCSGFilterSettings;

typedef struct POCSAGPagerApp POCSAGPagerApp;

/** Restore receiver and transmitter configuration saved on the SD card. */
void pcsg_settings_load(POCSAGPagerApp* app);

/** Save receiver and transmitter configuration to the SD card. */
void pcsg_settings_save(POCSAGPagerApp* app);

/** Whether a decoded message passes the filters and should enter history. */
bool pcsg_filter_accepts(
    const PCSGFilterSettings* filter,
    SubGhzProtocolDecoderBase* decoder_base);
