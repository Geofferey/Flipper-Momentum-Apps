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

typedef enum {
    PCSGCapcodeFilterOff,
    PCSGCapcodeFilterExact,
    PCSGCapcodeFilterRange,
    PCSGCapcodeFilterCount,
} PCSGCapcodeFilter;

typedef struct {
    PCSGTypeFilter type_filter;
    bool show_binary;
    PCSGCapcodeFilter capcode_filter;
    uint32_t capcode_min;
    uint32_t capcode_max;
    bool hide_duplicates;
    bool hide_encrypted;
    // Log to one temp file destroyed on exit instead of a kept log per session.
    bool log_temp;
} PCSGFilterSettings;

typedef struct POCSAGPagerApp POCSAGPagerApp;

/** Restore frequency, preset, hopping and filters saved on the SD card. */
void pcsg_settings_load(POCSAGPagerApp* app);

/** Save frequency, preset, hopping and filters to the SD card. */
void pcsg_settings_save(POCSAGPagerApp* app);

/** Whether a decoded message passes the filters and should enter history. */
bool pcsg_filter_accepts(
    const PCSGFilterSettings* filter,
    SubGhzProtocolDecoderBase* decoder_base);
