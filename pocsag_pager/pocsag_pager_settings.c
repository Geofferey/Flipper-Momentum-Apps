#include "pocsag_pager_settings.h"

#include "pocsag_pager_app_i.h"
#include "protocols/flex.h"
#include "protocols/pocsag.h"

#include <flipper_format/flipper_format.h>
#include <storage/storage.h>

#include <stddef.h>

#define TAG "PCSGSettings"

#define PCSG_SETTINGS_DIR       EXT_PATH("apps_data/pocsag_pager")
#define PCSG_SETTINGS_PATH      PCSG_SETTINGS_DIR "/settings.txt"
#define PCSG_SETTINGS_FILE_TYPE "POCSAG Pager Settings"
#define PCSG_SETTINGS_VERSION   1U

uint32_t pcsg_correct_frequency(int32_t ppm, uint32_t nominal) {
    if(ppm == 0) return nominal;
    // Round the offset to the nearest Hz, away from zero on a tie. Splitting
    // nominal into whole * 1e6 + fraction keeps both products inside 32 bits:
    // nominal * |ppm| / 1e6 == whole * |ppm| + (fraction * |ppm|) / 1e6. Doing
    // this in 64-bit instead links in __udivmoddi4 (~700 bytes) for nothing.
    const uint32_t magnitude = (uint32_t)(ppm < 0 ? -ppm : ppm);
    const uint32_t offset = (nominal / 1000000U) * magnitude +
                            ((nominal % 1000000U) * magnitude + 500000U) / 1000000U;
    if(ppm < 0) return offset >= nominal ? 0U : nominal - offset;
    return nominal + offset;
}

static bool pcsg_filter_in_range(
    PCSGCapcodeFilter mode,
    uint32_t first,
    uint32_t second,
    uint32_t value) {
    switch(mode) {
    case PCSGCapcodeFilterExact:
        return value == first;
    case PCSGCapcodeFilterRange:
        return value >= MIN(first, second) && value <= MAX(first, second);
    default:
        return true;
    }
}

bool pcsg_filter_accepts(
    const PCSGFilterSettings* filter,
    SubGhzProtocolDecoderBase* decoder_base) {
    furi_assert(filter);
    furi_assert(decoder_base);

    // POCSAG: only the RIC filter applies; the other filters are FLEX ones.
    if(decoder_base->protocol == &subghz_protocol_pocsag) {
        return pcsg_filter_in_range(
            filter->ric_filter,
            filter->ric_min,
            filter->ric_max,
            subghz_protocol_decoder_pocsag_get_ric(decoder_base));
    }
    if(decoder_base->protocol != &subghz_protocol_flex) return true;

    FlexPageInfo page;
    subghz_protocol_decoder_flex_get_page_info(decoder_base, &page);
    if(filter->flex_dups == PCSGFlexDupsHide && page.duplicate) return false;
    if(filter->flex_dups == PCSGFlexDupsCaps && page.same_cap_duplicate) return false;
    if(page.encrypted && filter->hide_encrypted) return false;
    if(page.partial && filter->hide_partial) return false;

    switch(page.kind) {
    case FlexPageKindAlpha:
        if(!filter->show_alpha) return false;
        break;
    case FlexPageKindNumeric:
        if(!filter->show_numeric) return false;
        break;
    case FlexPageKindTone:
        if(!filter->show_tone) return false;
        break;
    case FlexPageKindBinary:
        if(!filter->show_binary) return false;
        break;
    }

    if(filter->phase_filter != PCSGPhaseFilterAll &&
       page.phase != 'A' + (filter->phase_filter - PCSGPhaseFilterA)) {
        return false;
    }
    return pcsg_filter_in_range(
               filter->capcode_filter, filter->capcode_min, filter->capcode_max, page.capcode) &&
           pcsg_filter_in_range(
               filter->frame_filter, filter->frame_min, filter->frame_max, page.frame);
}

typedef enum {
    /** Bounded unsigned value, stored at the field's own width. */
    PCSGSetU32,
    PCSGSetBool,
    /** The file holds "shown", the field holds "hidden". */
    PCSGSetBoolInv,
    /** Signed, bounded symmetrically by +/- limit. */
    PCSGSetI32,
    /** Validated against the radio device before it is accepted. */
    PCSGSetFreq,
    /** One of the three POCSAG rates. */
    PCSGSetBaud,
    /** Like PCSGSetI32, and also a multiple of the TX correction step. */
    PCSGSetStep,
} PCSGSetKind;

typedef struct {
    const char* key;
    uint32_t limit;
    uint16_t offset;
    uint8_t width;
    uint8_t kind;
} PCSGSetting;

#define PCSG_FIELD(member) offsetof(POCSAGPagerApp, member), sizeof(((POCSAGPagerApp*)0)->member)

/*
 * Every key after Frequency and Preset, in the one order both load and save
 * use. FlipperFormat scans forward from the current stream position, so a key
 * read out of order would silently drop itself and everything after it.
 * Append new keys at the end so older settings files keep the preceding ones.
 *
 * A key that is no longer read, as Hopping became, needs no placeholder: the
 * forward scan steps over a line nobody asks for.
 */
static const PCSGSetting pcsg_settings[] = {
    {"FlexBinary", 0, PCSG_FIELD(filter.show_binary), PCSGSetBool},
    {"CapcodeFilter", PCSGCapcodeFilterCount - 1, PCSG_FIELD(filter.capcode_filter), PCSGSetU32},
    {"CapcodeMin", PCSG_CAPCODE_MAX, PCSG_FIELD(filter.capcode_min), PCSGSetU32},
    {"CapcodeMax", PCSG_CAPCODE_MAX, PCSG_FIELD(filter.capcode_max), PCSGSetU32},
    {"FlexDups", PCSGFlexDupsCount - 1, PCSG_FIELD(filter.flex_dups), PCSGSetU32},
    {"FlexEncrypted", 0, PCSG_FIELD(filter.hide_encrypted), PCSGSetBoolInv},
    {"FlexPartial", 0, PCSG_FIELD(filter.hide_partial), PCSGSetBoolInv},
    {"LogTemp", 0, PCSG_FIELD(filter.log_temp), PCSGSetBool},
    {"FlexPhase", PCSGPhaseFilterCount - 1, PCSG_FIELD(filter.phase_filter), PCSGSetU32},
    {"FrameFilter", PCSGCapcodeFilterCount - 1, PCSG_FIELD(filter.frame_filter), PCSGSetU32},
    {"FrameMin", PCSG_FRAME_MAX, PCSG_FIELD(filter.frame_min), PCSGSetU32},
    {"FrameMax", PCSG_FRAME_MAX, PCSG_FIELD(filter.frame_max), PCSGSetU32},
    {"RicFilter", PCSGCapcodeFilterCount - 1, PCSG_FIELD(filter.ric_filter), PCSGSetU32},
    {"RicMin", PCSG_RIC_MAX, PCSG_FIELD(filter.ric_min), PCSGSetU32},
    {"RicMax", PCSG_RIC_MAX, PCSG_FIELD(filter.ric_max), PCSGSetU32},
    {"Alert", PCSGAlertCount - 1, PCSG_FIELD(filter.alert), PCSGSetU32},
    {"SignalSound", PCSGSoundCount - 1, PCSG_FIELD(filter.signal_sound), PCSGSetU32},
    {"ToneVolume", PCSGToneVolumeCount - 1, PCSG_FIELD(filter.tone_volume), PCSGSetU32},
    {"TonePitch", PCSGTonePitchCount - 1, PCSG_FIELD(filter.tone_pitch), PCSGSetU32},
    {"ToneSpread", PCSGToneSpreadCount - 1, PCSG_FIELD(filter.tone_spread), PCSGSetU32},
    {"FreqCorrPpm", PCSG_FREQ_PPM_MAX, PCSG_FIELD(filter.freq_correction_ppm), PCSGSetI32},
    {"RawCapture", 0, PCSG_FIELD(filter.raw_capture), PCSGSetBool},
    {"StatsLog", 0, PCSG_FIELD(filter.stats_log), PCSGSetBool},
    {"WeakDecode", 0, PCSG_FIELD(filter.weak_decode), PCSGSetBool},
    {"TxFrequency", 0, PCSG_FIELD(tx_config.frequency), PCSGSetFreq},
    {"TxRic", PCSG_TX_RIC_MAX, PCSG_FIELD(tx_config.ric), PCSGSetU32},
    {"TxFunction", 3U, PCSG_FIELD(tx_config.function), PCSGSetU32},
    {"TxBaud", 0, PCSG_FIELD(tx_config.baud), PCSGSetBaud},
    {"TxInverted", 0, PCSG_FIELD(tx_config.inverted), PCSGSetBool},
    {"TxCorrection",
     PCSG_TX_CORRECTION_MAX_HZ,
     PCSG_FIELD(tx_config.frequency_correction_hz),
     PCSGSetStep},
    {"TxProtocol", PagerTxProtocolCount - 1, PCSG_FIELD(tx_config.protocol), PCSGSetU32},
    {"ReceiveLed", 0, PCSG_FIELD(filter.receive_led), PCSGSetBool},
    {"ReceiveLedColor", PCSGLedColorCount - 1, PCSG_FIELD(filter.receive_led_color), PCSGSetU32},
    {"DecodeLed", 0, PCSG_FIELD(filter.decode_led), PCSGSetBool},
    {"TimeSync", PCSGTimeSyncCount - 1, PCSG_FIELD(filter.time_sync), PCSGSetU32},
    {"DecodeLedColor", PCSGLedColorCount - 1, PCSG_FIELD(filter.decode_led_color), PCSGSetU32},
    {"AlertTone", PCSGAlertToneCount - 1, PCSG_FIELD(filter.alert_tone), PCSGSetU32},
    // The retired FlexTypes key is simply no longer read: the forward scan
    // steps over a line nobody asks for. These three replace it, and have to
    // go last because a key the file lacks ends every read after it.
    {"ShowAlpha", 0, PCSG_FIELD(filter.show_alpha), PCSGSetBool},
    {"ShowNumeric", 0, PCSG_FIELD(filter.show_numeric), PCSGSetBool},
    {"ShowTone", 0, PCSG_FIELD(filter.show_tone), PCSGSetBool},
};

static uint32_t pcsg_setting_get(const POCSAGPagerApp* app, const PCSGSetting* setting) {
    const void* field = (const uint8_t*)app + setting->offset;
    if(setting->width == 1) return *(const uint8_t*)field;
    if(setting->width == 2) return *(const uint16_t*)field;
    return *(const uint32_t*)field;
}

static void pcsg_setting_set(POCSAGPagerApp* app, const PCSGSetting* setting, uint32_t value) {
    void* field = (uint8_t*)app + setting->offset;
    if(setting->width == 1) *(uint8_t*)field = (uint8_t)value;
    else if(setting->width == 2) *(uint16_t*)field = (uint16_t)value;
    else *(uint32_t*)field = value;
}

static int32_t* pcsg_setting_signed(POCSAGPagerApp* app, const PCSGSetting* setting) {
    return (int32_t*)((uint8_t*)app + setting->offset);
}

void pcsg_settings_load(POCSAGPagerApp* app) {
    furi_assert(app);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* text = furi_string_alloc();
    uint32_t value = 0;

    // Keys are read in the order pcsg_settings_save() writes them; anything
    // missing or out of range keeps its default.
    do {
        if(!flipper_format_file_open_existing(file, PCSG_SETTINGS_PATH)) break;
        if(!flipper_format_read_header(file, text, &value) ||
           furi_string_cmp_str(text, PCSG_SETTINGS_FILE_TYPE) != 0 ||
           value != PCSG_SETTINGS_VERSION) {
            FURI_LOG_W(TAG, "Ignoring unknown settings file");
            break;
        }

        uint32_t frequency = app->txrx->preset->frequency;
        if(flipper_format_read_uint32(file, "Frequency", &value, 1) &&
           subghz_devices_is_frequency_valid(app->txrx->radio_device, value)) {
            frequency = value;
        }
        if(flipper_format_read_string(file, "Preset", text) &&
           subghz_setting_get_preset_data_by_name(app->setting, furi_string_get_cstr(text))) {
            pcsg_preset_init(app, furi_string_get_cstr(text), frequency, NULL, 0);
        } else {
            app->txrx->preset->frequency = frequency;
        }
        // Older settings files do not have a separate transmit frequency.
        app->tx_config.frequency = app->txrx->preset->frequency;

        for(size_t i = 0; i < COUNT_OF(pcsg_settings); i++) {
            const PCSGSetting* setting = &pcsg_settings[i];

            if(setting->kind == PCSGSetI32 || setting->kind == PCSGSetStep) {
                int32_t signed_value;
                if(!flipper_format_read_int32(file, setting->key, &signed_value, 1)) continue;
                if(signed_value < -(int32_t)setting->limit ||
                   signed_value > (int32_t)setting->limit) {
                    continue;
                }
                if(setting->kind == PCSGSetStep &&
                   signed_value % PCSG_TX_CORRECTION_STEP_HZ != 0) {
                    continue;
                }
                *pcsg_setting_signed(app, setting) = signed_value;
                continue;
            }

            if(!flipper_format_read_uint32(file, setting->key, &value, 1)) continue;
            switch(setting->kind) {
            case PCSGSetU32:
                if(value <= setting->limit) pcsg_setting_set(app, setting, value);
                break;
            case PCSGSetBool:
                pcsg_setting_set(app, setting, value != 0);
                break;
            case PCSGSetBoolInv:
                pcsg_setting_set(app, setting, value == 0);
                break;
            case PCSGSetFreq:
                if(subghz_devices_is_frequency_valid(app->txrx->radio_device, value)) {
                    pcsg_setting_set(app, setting, value);
                }
                break;
            case PCSGSetBaud:
                if(value == 512U || value == 1200U || value == 2400U) {
                    pcsg_setting_set(app, setting, value);
                }
                break;
            default:
                break;
            }
        }
    } while(false);

    furi_string_free(text);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
}

void pcsg_settings_save(POCSAGPagerApp* app) {
    furi_assert(app);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool saved = false;
    uint32_t value;

    do {
        if(!storage_simply_mkdir(storage, PCSG_SETTINGS_DIR)) break;
        if(!flipper_format_file_open_always(file, PCSG_SETTINGS_PATH)) break;
        if(!flipper_format_write_header_cstr(
               file, PCSG_SETTINGS_FILE_TYPE, PCSG_SETTINGS_VERSION)) {
            break;
        }
        value = app->txrx->preset->frequency;
        if(!flipper_format_write_uint32(file, "Frequency", &value, 1)) break;
        if(!flipper_format_write_string(file, "Preset", app->txrx->preset->name)) break;

        bool written = true;
        for(size_t i = 0; i < COUNT_OF(pcsg_settings) && written; i++) {
            const PCSGSetting* setting = &pcsg_settings[i];

            if(setting->kind == PCSGSetI32 || setting->kind == PCSGSetStep) {
                written = flipper_format_write_int32(
                    file, setting->key, pcsg_setting_signed(app, setting), 1);
                continue;
            }

            switch(setting->kind) {
            case PCSGSetBoolInv:
                value = !pcsg_setting_get(app, setting);
                break;
            default:
                value = pcsg_setting_get(app, setting);
                break;
            }
            written = flipper_format_write_uint32(file, setting->key, &value, 1);
        }
        if(!written) break;
        saved = true;
    } while(false);

    if(!saved) FURI_LOG_E(TAG, "Failed to save settings");
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
}
