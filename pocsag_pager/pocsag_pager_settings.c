#include "pocsag_pager_settings.h"

#include "pocsag_pager_app_i.h"
#include "protocols/flex.h"
#include "protocols/pocsag.h"

#include <flipper_format/flipper_format.h>
#include <storage/storage.h>

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
        break;
    case FlexPageKindNumeric:
        if(filter->type_filter == PCSGTypeFilterAlpha) return false;
        break;
    case FlexPageKindTone:
        if(filter->type_filter != PCSGTypeFilterAll) return false;
        break;
    case FlexPageKindBinary:
        if(!filter->show_binary) return false;
        if(filter->type_filter != PCSGTypeFilterAll &&
           filter->type_filter != PCSGTypeFilterNoTones) {
            return false;
        }
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

void pcsg_settings_load(POCSAGPagerApp* app) {
    furi_assert(app);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* text = furi_string_alloc();
    PCSGFilterSettings* filter = &app->filter;
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

        if(flipper_format_read_uint32(file, "Hopping", &value, 1)) {
            app->txrx->hopper_state = value ? PCSGHopperStateRunnig : PCSGHopperStateOFF;
        }
        if(flipper_format_read_uint32(file, "FlexTypes", &value, 1) &&
           value < PCSGTypeFilterCount) {
            filter->type_filter = value;
        }
        if(flipper_format_read_uint32(file, "FlexBinary", &value, 1)) {
            filter->show_binary = value != 0;
        }
        if(flipper_format_read_uint32(file, "CapcodeFilter", &value, 1) &&
           value < PCSGCapcodeFilterCount) {
            filter->capcode_filter = value;
        }
        if(flipper_format_read_uint32(file, "CapcodeMin", &value, 1) &&
           value <= PCSG_CAPCODE_MAX) {
            filter->capcode_min = value;
        }
        if(flipper_format_read_uint32(file, "CapcodeMax", &value, 1) &&
           value <= PCSG_CAPCODE_MAX) {
            filter->capcode_max = value;
        }
        if(flipper_format_read_uint32(file, "FlexDups", &value, 1) &&
           value < PCSGFlexDupsCount) {
            filter->flex_dups = value;
        }
        if(flipper_format_read_uint32(file, "FlexEncrypted", &value, 1)) {
            filter->hide_encrypted = value == 0;
        }
        if(flipper_format_read_uint32(file, "FlexPartial", &value, 1)) {
            filter->hide_partial = value == 0;
        }
        if(flipper_format_read_uint32(file, "LogTemp", &value, 1)) {
            filter->log_temp = value != 0;
        }
        if(flipper_format_read_uint32(file, "FlexPhase", &value, 1) &&
           value < PCSGPhaseFilterCount) {
            filter->phase_filter = value;
        }
        if(flipper_format_read_uint32(file, "FrameFilter", &value, 1) &&
           value < PCSGCapcodeFilterCount) {
            filter->frame_filter = value;
        }
        if(flipper_format_read_uint32(file, "FrameMin", &value, 1) && value <= PCSG_FRAME_MAX) {
            filter->frame_min = value;
        }
        if(flipper_format_read_uint32(file, "FrameMax", &value, 1) && value <= PCSG_FRAME_MAX) {
            filter->frame_max = value;
        }
        if(flipper_format_read_uint32(file, "RicFilter", &value, 1) &&
           value < PCSGCapcodeFilterCount) {
            filter->ric_filter = value;
        }
        if(flipper_format_read_uint32(file, "RicMin", &value, 1) && value <= PCSG_RIC_MAX) {
            filter->ric_min = value;
        }
        if(flipper_format_read_uint32(file, "RicMax", &value, 1) && value <= PCSG_RIC_MAX) {
            filter->ric_max = value;
        }
        if(flipper_format_read_uint32(file, "Alert", &value, 1) && value < PCSGAlertCount) {
            filter->alert = value;
        }
        if(flipper_format_read_uint32(file, "SignalSound", &value, 1) && value < PCSGSoundCount) {
            filter->signal_sound = value;
        }
        if(flipper_format_read_uint32(file, "ToneVolume", &value, 1) &&
           value < PCSGToneVolumeCount) {
            filter->tone_volume = value;
        }
        if(flipper_format_read_uint32(file, "TonePitch", &value, 1) &&
           value < PCSGTonePitchCount) {
            filter->tone_pitch = value;
        }
        if(flipper_format_read_uint32(file, "ToneSpread", &value, 1) &&
           value < PCSGToneSpreadCount) {
            filter->tone_spread = value;
        }
        int32_t svalue;
        if(flipper_format_read_int32(file, "FreqCorrPpm", &svalue, 1) &&
           svalue >= -PCSG_FREQ_PPM_MAX && svalue <= PCSG_FREQ_PPM_MAX) {
            filter->freq_correction_ppm = svalue;
        }
        if(flipper_format_read_uint32(file, "FreqAuto", &value, 1)) {
            filter->freq_auto = value != 0;
        }
        if(flipper_format_read_uint32(file, "RawCapture", &value, 1)) {
            filter->raw_capture = value != 0;
        }
        if(flipper_format_read_uint32(file, "StatsLog", &value, 1)) {
            filter->stats_log = value != 0;
        }
        if(flipper_format_read_uint32(file, "WeakDecode", &value, 1)) {
            filter->weak_decode = value != 0;
        }
        if(flipper_format_read_uint32(file, "TxFrequency", &value, 1) &&
           subghz_devices_is_frequency_valid(app->txrx->radio_device, value)) {
            app->tx_config.frequency = value;
        }
        if(flipper_format_read_uint32(file, "TxRic", &value, 1) && value <= PCSG_TX_RIC_MAX) {
            app->tx_config.ric = value;
        }
        if(flipper_format_read_uint32(file, "TxFunction", &value, 1) && value <= 3U) {
            app->tx_config.function = value;
        }
        if(flipper_format_read_uint32(file, "TxBaud", &value, 1) &&
           (value == 512U || value == 1200U || value == 2400U)) {
            app->tx_config.baud = value;
        }
        if(flipper_format_read_uint32(file, "TxInverted", &value, 1)) {
            app->tx_config.inverted = value != 0;
        }
        int32_t correction;
        if(flipper_format_read_int32(file, "TxCorrection", &correction, 1) &&
           correction >= PCSG_TX_CORRECTION_MIN_HZ &&
           correction <= PCSG_TX_CORRECTION_MAX_HZ &&
           correction % PCSG_TX_CORRECTION_STEP_HZ == 0) {
            app->tx_config.frequency_correction_hz = correction;
        }
        if(flipper_format_read_uint32(file, "TxProtocol", &value, 1) &&
           value < PagerTxProtocolCount) {
            app->tx_config.protocol = value;
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
    const PCSGFilterSettings* filter = &app->filter;
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
        value = app->txrx->hopper_state != PCSGHopperStateOFF;
        if(!flipper_format_write_uint32(file, "Hopping", &value, 1)) break;
        value = filter->type_filter;
        if(!flipper_format_write_uint32(file, "FlexTypes", &value, 1)) break;
        value = filter->show_binary;
        if(!flipper_format_write_uint32(file, "FlexBinary", &value, 1)) break;
        value = filter->capcode_filter;
        if(!flipper_format_write_uint32(file, "CapcodeFilter", &value, 1)) break;
        if(!flipper_format_write_uint32(file, "CapcodeMin", &filter->capcode_min, 1)) break;
        if(!flipper_format_write_uint32(file, "CapcodeMax", &filter->capcode_max, 1)) break;
        value = filter->flex_dups;
        if(!flipper_format_write_uint32(file, "FlexDups", &value, 1)) break;
        value = !filter->hide_encrypted;
        if(!flipper_format_write_uint32(file, "FlexEncrypted", &value, 1)) break;
        value = !filter->hide_partial;
        if(!flipper_format_write_uint32(file, "FlexPartial", &value, 1)) break;
        value = filter->log_temp;
        if(!flipper_format_write_uint32(file, "LogTemp", &value, 1)) break;
        value = filter->phase_filter;
        if(!flipper_format_write_uint32(file, "FlexPhase", &value, 1)) break;
        value = filter->frame_filter;
        if(!flipper_format_write_uint32(file, "FrameFilter", &value, 1)) break;
        if(!flipper_format_write_uint32(file, "FrameMin", &filter->frame_min, 1)) break;
        if(!flipper_format_write_uint32(file, "FrameMax", &filter->frame_max, 1)) break;
        value = filter->ric_filter;
        if(!flipper_format_write_uint32(file, "RicFilter", &value, 1)) break;
        if(!flipper_format_write_uint32(file, "RicMin", &filter->ric_min, 1)) break;
        if(!flipper_format_write_uint32(file, "RicMax", &filter->ric_max, 1)) break;
        value = filter->alert;
        if(!flipper_format_write_uint32(file, "Alert", &value, 1)) break;
        value = filter->signal_sound;
        if(!flipper_format_write_uint32(file, "SignalSound", &value, 1)) break;
        value = filter->tone_volume;
        if(!flipper_format_write_uint32(file, "ToneVolume", &value, 1)) break;
        value = filter->tone_pitch;
        if(!flipper_format_write_uint32(file, "TonePitch", &value, 1)) break;
        value = filter->tone_spread;
        if(!flipper_format_write_uint32(file, "ToneSpread", &value, 1)) break;
        if(!flipper_format_write_int32(file, "FreqCorrPpm", &filter->freq_correction_ppm, 1)) break;
        value = filter->freq_auto;
        if(!flipper_format_write_uint32(file, "FreqAuto", &value, 1)) break;
        value = filter->raw_capture;
        if(!flipper_format_write_uint32(file, "RawCapture", &value, 1)) break;
        value = filter->stats_log;
        if(!flipper_format_write_uint32(file, "StatsLog", &value, 1)) break;
        value = filter->weak_decode;
        if(!flipper_format_write_uint32(file, "WeakDecode", &value, 1)) break;
        value = app->tx_config.frequency;
        if(!flipper_format_write_uint32(file, "TxFrequency", &value, 1)) break;
        value = app->tx_config.ric;
        if(!flipper_format_write_uint32(file, "TxRic", &value, 1)) break;
        value = app->tx_config.function;
        if(!flipper_format_write_uint32(file, "TxFunction", &value, 1)) break;
        value = app->tx_config.baud;
        if(!flipper_format_write_uint32(file, "TxBaud", &value, 1)) break;
        value = app->tx_config.inverted;
        if(!flipper_format_write_uint32(file, "TxInverted", &value, 1)) break;
        if(!flipper_format_write_int32(
               file, "TxCorrection", &app->tx_config.frequency_correction_hz, 1)) {
            break;
        }
        value = app->tx_config.protocol;
        if(!flipper_format_write_uint32(file, "TxProtocol", &value, 1)) break;
        saved = true;
    } while(false);

    if(!saved) FURI_LOG_E(TAG, "Failed to save settings");
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
}
