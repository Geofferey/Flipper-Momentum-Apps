#include "../pocsag_pager_app_i.h"

enum PCSGSettingIndex {
    PCSGSettingIndexFrequency,
    PCSGSettingIndexModulation,
    PCSGSettingIndexShowAlpha,
    PCSGSettingIndexFlexDups,
    PCSGSettingIndexShowNumeric,
    PCSGSettingIndexShowTone,
    PCSGSettingIndexShowBinary,
    PCSGSettingIndexFlexEncrypted,
    PCSGSettingIndexFlexPartial,
    PCSGSettingIndexLogging,
    PCSGSettingIndexAlert,
    PCSGSettingIndexAlertTone,
    PCSGSettingIndexReceiveLed,
    PCSGSettingIndexDecodeLed,
    PCSGSettingIndexTimeSync,
    PCSGSettingIndexSignalSound,
    PCSGSettingIndexToneVolume,
    PCSGSettingIndexTonePitch,
    PCSGSettingIndexToneSpread,
    PCSGSettingIndexCapcodeFilter,
    PCSGSettingIndexCapcodeMin,
    PCSGSettingIndexCapcodeMax,
    PCSGSettingIndexRicFilter,
    PCSGSettingIndexRicMin,
    PCSGSettingIndexRicMax,
    PCSGSettingIndexFlexPhase,
    PCSGSettingIndexFrameFilter,
    PCSGSettingIndexFrameMin,
    PCSGSettingIndexFrameMax,
    PCSGSettingIndexFreqMode,
    PCSGSettingIndexFreqCorr,
    PCSGSettingIndexRawCapture,
    PCSGSettingIndexStatsLog,
    PCSGSettingIndexWeakDecode,
    PCSGSettingIndexLock,
};

#define FREQ_MODE_COUNT 2
const char* const freq_mode_text[FREQ_MODE_COUNT] = {
    "Manual",
    "Auto",
};

const char* const phase_filter_text[PCSGPhaseFilterCount] = {
    "All",
    "A",
    "B",
    "C",
    "D",
};

#define HIDE_SHOW_COUNT 2
const char* const hide_show_text[HIDE_SHOW_COUNT] = {
    "Hide",
    "Show",
};

const char* const flex_dups_text[PCSGFlexDupsCount] = {
    "Hide",
    "Show",
    "CAPS",
};

#define LOGGING_COUNT 2
const char* const logging_text[LOGGING_COUNT] = {
    "Save",
    "Temp",
};

const char* const alert_text[PCSGAlertCount] = {
    "Both",
    "Sound",
    "Vibro",
    "Off",
};

const char* const alert_tone_text[PCSGAlertToneCount] = {
    "Default",
    "Classic",
};

const char* const time_sync_text[PCSGTimeSyncCount] = {
    "Off",
    "MM:SS",
    "HH:MM:SS",
};

#define LED_SELECTION_COUNT (PCSGLedColorCount + 1U)
const char* const led_color_text[LED_SELECTION_COUNT] = {
    "Off",
    "Cyan",
    "Blue",
    "Green",
    "Yellow",
    "Magenta",
    "Red",
    "White",
    "Amber",
    "Lime",
    "Teal",
    "Sky",
    "Violet",
    "Pink",
    "Warm",
};

const char* const signal_sound_text[PCSGSoundCount] = {
    "Off",
    "Raw",
    "Tone",
    "Synth",
};

const char* const tone_volume_text[PCSGToneVolumeCount] = {
    "Low",
    "Medium",
    "High",
};

const char* const tone_pitch_text[PCSGTonePitchCount] = {
    "Low",
    "Medium",
    "High",
};

const char* const tone_spread_text[PCSGToneSpreadCount] = {
    "Narrow",
    "Normal",
    "Wide",
};

const char* const capcode_filter_text[PCSGCapcodeFilterCount] = {
    "Off",
    "Exact",
    "Range",
};

/*
 * The maximum is only used in range mode; its text shows that.
 *
 * Each "max" row is looked up by position every time instead of keeping the
 * VariableItem* that variable_item_list_add() returned. The list holds its
 * items in an m-array, so adding the rows that follow eventually reallocates
 * it and moves every item; a kept pointer then refers to freed memory, and
 * writing a row's text through it corrupts the heap.
 */
static VariableItem* pcsg_config_row(POCSAGPagerApp* app, uint8_t index) {
    return variable_item_list_get(app->variable_item_list, index);
}

uint8_t pocsag_pager_scene_receiver_config_next_frequency(const uint32_t value, void* context) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    uint8_t index = 0;
    for(uint8_t i = 0; i < subghz_setting_get_frequency_count(app->setting); i++) {
        if(value == subghz_setting_get_frequency(app->setting, i)) {
            index = i;
            break;
        } else {
            index = subghz_setting_get_frequency_default_index(app->setting);
        }
    }
    return index;
}

uint8_t pocsag_pager_scene_receiver_config_next_preset(const char* preset_name, void* context) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    uint8_t index = 0;
    for(uint8_t i = 0; i < subghz_setting_get_preset_count(app->setting); i++) {
        if(!strcmp(subghz_setting_get_preset_name(app->setting, i), preset_name)) {
            index = i;
            break;
        } else {
            //  index = subghz_setting_get_frequency_default_index(app ->setting);
        }
    }
    return index;
}

static void pocsag_pager_scene_receiver_config_set_frequency(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    char text_buf[10] = {0};
    snprintf(
        text_buf,
        sizeof(text_buf),
        "%lu.%02lu",
        subghz_setting_get_frequency(app->setting, index) / 1000000,
        (subghz_setting_get_frequency(app->setting, index) % 1000000) / 10000);
    variable_item_set_current_value_text(item, text_buf);
    app->txrx->preset->frequency = subghz_setting_get_frequency(app->setting, index);
    app->txrx->rx_restart = true;
}

static void pocsag_pager_scene_receiver_config_set_preset(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(
        item, subghz_setting_get_preset_name(app->setting, index));
    pcsg_preset_init(
        app,
        subghz_setting_get_preset_name(app->setting, index),
        app->txrx->preset->frequency,
        subghz_setting_get_preset_data(app->setting, index),
        subghz_setting_get_preset_data_size(app->setting, index));
    app->txrx->rx_restart = true;
}


static void pocsag_pager_scene_receiver_config_set_capcode_text(VariableItem* item, uint32_t value) {
    char text_buf[10] = {0};
    snprintf(text_buf, sizeof(text_buf), "%lu", value);
    variable_item_set_current_value_text(item, text_buf);
}

static void pocsag_pager_scene_receiver_config_update_capcode_max(POCSAGPagerApp* app) {
    VariableItem* item = pcsg_config_row(app, PCSGSettingIndexCapcodeMax);
    if(!item) return;
    if(app->filter.capcode_filter == PCSGCapcodeFilterRange) {
        pocsag_pager_scene_receiver_config_set_capcode_text(item, app->filter.capcode_max);
    } else {
        variable_item_set_current_value_text(item, "--");
    }
}

static void pocsag_pager_scene_receiver_config_update_ric_max(POCSAGPagerApp* app) {
    VariableItem* item = pcsg_config_row(app, PCSGSettingIndexRicMax);
    if(!item) return;
    if(app->filter.ric_filter == PCSGCapcodeFilterRange) {
        pocsag_pager_scene_receiver_config_set_capcode_text(item, app->filter.ric_max);
    } else {
        variable_item_set_current_value_text(item, "--");
    }
}

static void pocsag_pager_scene_receiver_config_update_frame_max(POCSAGPagerApp* app) {
    VariableItem* item = pcsg_config_row(app, PCSGSettingIndexFrameMax);
    if(!item) return;
    if(app->filter.frame_filter == PCSGCapcodeFilterRange) {
        pocsag_pager_scene_receiver_config_set_capcode_text(item, app->filter.frame_max);
    } else {
        variable_item_set_current_value_text(item, "--");
    }
}

#define RAW_CAPTURE_COUNT 2
const char* const raw_capture_text[RAW_CAPTURE_COUNT] = {
    "Off",
    "On",
};
// Index 0 is -PCSG_FREQ_PPM_MAX; the centre index is 0 ppm.
#define FREQ_PPM_COUNT (2U * PCSG_FREQ_PPM_MAX + 1U)
static void pocsag_pager_scene_receiver_config_set_freq_corr(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    const int32_t ppm = (int32_t)variable_item_get_current_value_index(item) - PCSG_FREQ_PPM_MAX;
    app->filter.freq_correction_ppm = ppm;
    char buf[8];
    snprintf(buf, sizeof(buf), "%+ld", (long)ppm);
    variable_item_set_current_value_text(item, buf);
}

/*
 * Every row that is just "pick a value, store it, maybe poke something" is
 * described here instead of getting its own setter and its own four lines in
 * on_enter. The entries are placed by PCSGSettingIndex, so the table and the
 * list cannot drift apart, and the gaps are the rows that genuinely differ
 * (frequency, preset, the number entries, ppm, Lock).
 */
typedef enum {
    PCSGRowNone, // a row this table does not describe
    PCSGRowEnum, // field = index
    PCSGRowBool, // field = index != 0
    PCSGRowBoolInv, // the file shows "Show", the field holds "hidden"
    PCSGRowLed, // off, or one colour per index past it
} PCSGRowKind;

typedef enum {
    PCSGSideNone,
    PCSGSideCapcodeMax,
    PCSGSideRicMax,
    PCSGSideFrameMax,
    PCSGSideAlert,
    PCSGSideTimeSync,
    PCSGSideRxRestart,
} PCSGRowSide;

typedef struct {
    const char* label;
    const char* const* text;
    uint16_t offset;
    // PCSGRowLed only: the colour field that index 1 and up select.
    uint16_t extra;
    uint8_t count;
    uint8_t kind;
    uint8_t side;
} PCSGConfigRow;

#define PCSG_ROW(member) offsetof(POCSAGPagerApp, member)

// Every field below is one byte, which is what lets them share one accessor.
_Static_assert(sizeof(bool) == 1, "row fields are byte-wide");
_Static_assert(sizeof(PCSGAlert) == 1, "build is missing -fshort-enums");

static const PCSGConfigRow pcsg_config_rows[] = {
    [PCSGSettingIndexShowAlpha] =
        {"Alphanumeric:", hide_show_text, PCSG_ROW(filter.show_alpha), 0, HIDE_SHOW_COUNT,
         PCSGRowBool, PCSGSideNone},
    [PCSGSettingIndexFlexDups] =
        {"Duplicates:", flex_dups_text, PCSG_ROW(filter.flex_dups), 0, PCSGFlexDupsCount,
         PCSGRowEnum, PCSGSideNone},
    [PCSGSettingIndexShowNumeric] =
        {"Numbers:", hide_show_text, PCSG_ROW(filter.show_numeric), 0, HIDE_SHOW_COUNT,
         PCSGRowBool, PCSGSideNone},
    [PCSGSettingIndexShowTone] =
        {"Tone only:", hide_show_text, PCSG_ROW(filter.show_tone), 0, HIDE_SHOW_COUNT,
         PCSGRowBool, PCSGSideNone},
    [PCSGSettingIndexShowBinary] =
        {"Binary:", hide_show_text, PCSG_ROW(filter.show_binary), 0, HIDE_SHOW_COUNT,
         PCSGRowBool, PCSGSideNone},
    [PCSGSettingIndexFlexEncrypted] =
        {"Encrypted:", hide_show_text, PCSG_ROW(filter.hide_encrypted), 0, HIDE_SHOW_COUNT,
         PCSGRowBoolInv, PCSGSideNone},
    // Hide = discard messages that could not be fully decoded.
    [PCSGSettingIndexFlexPartial] =
        {"Partial Msgs:", hide_show_text, PCSG_ROW(filter.hide_partial), 0, HIDE_SHOW_COUNT,
         PCSGRowBoolInv, PCSGSideNone},
    // Applies from the next receive session.
    [PCSGSettingIndexLogging] =
        {"Logging:", logging_text, PCSG_ROW(filter.log_temp), 0, LOGGING_COUNT, PCSGRowBool,
         PCSGSideNone},
    [PCSGSettingIndexAlert] =
        {"Alert:", alert_text, PCSG_ROW(filter.alert), 0, PCSGAlertCount, PCSGRowEnum,
         PCSGSideAlert},
    [PCSGSettingIndexAlertTone] =
        {"Alert Tone:", alert_tone_text, PCSG_ROW(filter.alert_tone), 0, PCSGAlertToneCount,
         PCSGRowEnum, PCSGSideAlert},
    [PCSGSettingIndexReceiveLed] =
        {"Receive LED:", led_color_text, PCSG_ROW(filter.receive_led),
         PCSG_ROW(filter.receive_led_color), LED_SELECTION_COUNT, PCSGRowLed, PCSGSideNone},
    [PCSGSettingIndexDecodeLed] =
        {"Decode LED:", led_color_text, PCSG_ROW(filter.decode_led),
         PCSG_ROW(filter.decode_led_color), LED_SELECTION_COUNT, PCSGRowLed, PCSGSideNone},
    [PCSGSettingIndexTimeSync] =
        {"Time sync:", time_sync_text, PCSG_ROW(filter.time_sync), 0, PCSGTimeSyncCount,
         PCSGRowEnum, PCSGSideTimeSync},
    // The speaker is set up when receiving starts, so these restart the radio.
    [PCSGSettingIndexSignalSound] =
        {"Signal Sound:", signal_sound_text, PCSG_ROW(filter.signal_sound), 0, PCSGSoundCount,
         PCSGRowEnum, PCSGSideRxRestart},
    [PCSGSettingIndexToneVolume] =
        {"Signal Vol:", tone_volume_text, PCSG_ROW(filter.tone_volume), 0, PCSGToneVolumeCount,
         PCSGRowEnum, PCSGSideRxRestart},
    [PCSGSettingIndexTonePitch] =
        {"Signal Pitch:", tone_pitch_text, PCSG_ROW(filter.tone_pitch), 0, PCSGTonePitchCount,
         PCSGRowEnum, PCSGSideRxRestart},
    [PCSGSettingIndexToneSpread] =
        {"Signal Spread:", tone_spread_text, PCSG_ROW(filter.tone_spread), 0,
         PCSGToneSpreadCount, PCSGRowEnum, PCSGSideRxRestart},
    [PCSGSettingIndexCapcodeFilter] =
        {"Cap. Filter:", capcode_filter_text, PCSG_ROW(filter.capcode_filter), 0,
         PCSGCapcodeFilterCount, PCSGRowEnum, PCSGSideCapcodeMax},
    [PCSGSettingIndexRicFilter] =
        {"RIC Filter:", capcode_filter_text, PCSG_ROW(filter.ric_filter), 0,
         PCSGCapcodeFilterCount, PCSGRowEnum, PCSGSideRicMax},
    [PCSGSettingIndexFlexPhase] =
        {"FLEX Phase:", phase_filter_text, PCSG_ROW(filter.phase_filter), 0,
         PCSGPhaseFilterCount, PCSGRowEnum, PCSGSideNone},
    [PCSGSettingIndexFrameFilter] =
        {"Frame Filter:", capcode_filter_text, PCSG_ROW(filter.frame_filter), 0,
         PCSGCapcodeFilterCount, PCSGRowEnum, PCSGSideFrameMax},
    [PCSGSettingIndexFreqMode] =
        {"Correct Mode:", freq_mode_text, PCSG_ROW(filter.freq_auto), 0, FREQ_MODE_COUNT,
         PCSGRowBool, PCSGSideNone},
    // Debug: record the raw FLEX4 sample stream to SD for offline analysis.
    [PCSGSettingIndexRawCapture] =
        {"Raw Capture:", raw_capture_text, PCSG_ROW(filter.raw_capture), 0, RAW_CAPTURE_COUNT,
         PCSGRowBool, PCSGSideNone},
    // Debug: periodic counter-only snapshots for untethered reception tests.
    [PCSGSettingIndexStatsLog] =
        {"Stats Log:", raw_capture_text, PCSG_ROW(filter.stats_log), 0, RAW_CAPTURE_COUNT,
         PCSGRowBool, PCSGSideNone},
    // Soft-decision (Chase) recovery of weak FLEX4 words. Off by default: it
    // allocates its reliability buffer in the decoder only while enabled.
    [PCSGSettingIndexWeakDecode] =
        {"Weak Decode:", raw_capture_text, PCSG_ROW(filter.weak_decode), 0, RAW_CAPTURE_COUNT,
         PCSGRowBool, PCSGSideNone},
    // Lock has no value to pick, and fixes the table's length to the list's.
    [PCSGSettingIndexLock] = {NULL, NULL, 0, 0, 0, PCSGRowNone, PCSGSideNone},
};

static uint8_t pcsg_config_row_value(const POCSAGPagerApp* app, const PCSGConfigRow* row) {
    const uint8_t stored = *((const uint8_t*)app + row->offset);
    if(row->kind == PCSGRowBoolInv) return stored ? 0U : 1U;
    if(row->kind == PCSGRowLed) {
        return stored ? (uint8_t)(*((const uint8_t*)app + row->extra) + 1U) : 0U;
    }
    return stored;
}

// The row being changed is always the selected one, which is how this finds
// its description without a separate callback per row.
static void pcsg_config_row_changed(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_list_get_selected_item_index(app->variable_item_list);
    if(index >= COUNT_OF(pcsg_config_rows)) return;
    const PCSGConfigRow* row = &pcsg_config_rows[index];
    if(row->kind == PCSGRowNone) return;

    const uint8_t value = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, row->text[value]);

    uint8_t* field = (uint8_t*)app + row->offset;
    switch(row->kind) {
    case PCSGRowEnum:
        *field = value;
        break;
    case PCSGRowBoolInv:
        *field = value == 0U;
        break;
    case PCSGRowLed:
        *field = value != 0U;
        if(value) *((uint8_t*)app + row->extra) = value - 1U;
        break;
    default:
        *field = value != 0U;
        break;
    }

    switch(row->side) {
    case PCSGSideCapcodeMax:
        pocsag_pager_scene_receiver_config_update_capcode_max(app);
        break;
    case PCSGSideRicMax:
        pocsag_pager_scene_receiver_config_update_ric_max(app);
        break;
    case PCSGSideFrameMax:
        pocsag_pager_scene_receiver_config_update_frame_max(app);
        break;
    case PCSGSideAlert:
        pcsg_classic_alert_reset(app);
        break;
    case PCSGSideTimeSync:
        app->time_sync_sequence = 0;
        break;
    case PCSGSideRxRestart:
        app->txrx->rx_restart = true;
        break;
    default:
        break;
    }
}

static void pcsg_config_row_add(POCSAGPagerApp* app, uint8_t index) {
    const PCSGConfigRow* row = &pcsg_config_rows[index];
    VariableItem* item = variable_item_list_add(
        app->variable_item_list, row->label, row->count, pcsg_config_row_changed, app);
    const uint8_t value = pcsg_config_row_value(app, row);
    variable_item_set_current_value_index(item, value);
    variable_item_set_current_value_text(item, row->text[value]);
}

static void
    pocsag_pager_scene_receiver_config_var_list_enter_callback(void* context, uint32_t index) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    if(index == PCSGSettingIndexLock) {
        view_dispatcher_send_custom_event(app->view_dispatcher, PCSGCustomEventSceneSettingLock);
    } else if(index == PCSGSettingIndexCapcodeMin) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, PCSGCustomEventSceneSettingCapcodeMin);
    } else if(
        index == PCSGSettingIndexCapcodeMax &&
        app->filter.capcode_filter == PCSGCapcodeFilterRange) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, PCSGCustomEventSceneSettingCapcodeMax);
    } else if(index == PCSGSettingIndexRicMin) {
        view_dispatcher_send_custom_event(app->view_dispatcher, PCSGCustomEventSceneSettingRicMin);
    } else if(
        index == PCSGSettingIndexRicMax && app->filter.ric_filter == PCSGCapcodeFilterRange) {
        view_dispatcher_send_custom_event(app->view_dispatcher, PCSGCustomEventSceneSettingRicMax);
    } else if(index == PCSGSettingIndexFrameMin) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, PCSGCustomEventSceneSettingFrameMin);
    } else if(
        index == PCSGSettingIndexFrameMax &&
        app->filter.frame_filter == PCSGCapcodeFilterRange) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, PCSGCustomEventSceneSettingFrameMax);
    }
}

void pocsag_pager_scene_receiver_config_on_enter(void* context) {
    POCSAGPagerApp* app = context;
    VariableItem* item;
    uint8_t value_index;

    item = variable_item_list_add(
        app->variable_item_list,
        "Frequency:",
        subghz_setting_get_frequency_count(app->setting),
        pocsag_pager_scene_receiver_config_set_frequency,
        app);
    value_index =
        pocsag_pager_scene_receiver_config_next_frequency(app->txrx->preset->frequency, app);
    variable_item_set_current_value_index(item, value_index);
    char text_buf[10] = {0};
    snprintf(
        text_buf,
        sizeof(text_buf),
        "%lu.%02lu",
        subghz_setting_get_frequency(app->setting, value_index) / 1000000,
        (subghz_setting_get_frequency(app->setting, value_index) % 1000000) / 10000);
    variable_item_set_current_value_text(item, text_buf);

    item = variable_item_list_add(
        app->variable_item_list,
        "Modulation:",
        subghz_setting_get_preset_count(app->setting),
        pocsag_pager_scene_receiver_config_set_preset,
        app);
    value_index = pocsag_pager_scene_receiver_config_next_preset(
        furi_string_get_cstr(app->txrx->preset->name), app);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(
        item, subghz_setting_get_preset_name(app->setting, value_index));

    // One Hide/Show per FLEX page kind, replacing the old combined type
    // filter. Duplicates keeps its third CAPS choice.
    pcsg_config_row_add(app, PCSGSettingIndexShowAlpha);
    pcsg_config_row_add(app, PCSGSettingIndexFlexDups);
    pcsg_config_row_add(app, PCSGSettingIndexShowNumeric);
    pcsg_config_row_add(app, PCSGSettingIndexShowTone);
    pcsg_config_row_add(app, PCSGSettingIndexShowBinary);
    pcsg_config_row_add(app, PCSGSettingIndexFlexEncrypted);
    pcsg_config_row_add(app, PCSGSettingIndexFlexPartial);
    pcsg_config_row_add(app, PCSGSettingIndexLogging);
    pcsg_config_row_add(app, PCSGSettingIndexAlert);
    pcsg_config_row_add(app, PCSGSettingIndexAlertTone);
    pcsg_config_row_add(app, PCSGSettingIndexReceiveLed);
    pcsg_config_row_add(app, PCSGSettingIndexDecodeLed);
    pcsg_config_row_add(app, PCSGSettingIndexTimeSync);
    pcsg_config_row_add(app, PCSGSettingIndexSignalSound);
    pcsg_config_row_add(app, PCSGSettingIndexToneVolume);
    pcsg_config_row_add(app, PCSGSettingIndexTonePitch);
    pcsg_config_row_add(app, PCSGSettingIndexToneSpread);
    pcsg_config_row_add(app, PCSGSettingIndexCapcodeFilter);

    item = variable_item_list_add(app->variable_item_list, "Capcode:", 1, NULL, app);
    pocsag_pager_scene_receiver_config_set_capcode_text(item, app->filter.capcode_min);

    variable_item_list_add(app->variable_item_list, "Cap. Max:", 1, NULL, app);
    pocsag_pager_scene_receiver_config_update_capcode_max(app);

    pcsg_config_row_add(app, PCSGSettingIndexRicFilter);

    item = variable_item_list_add(app->variable_item_list, "RIC:", 1, NULL, app);
    pocsag_pager_scene_receiver_config_set_capcode_text(item, app->filter.ric_min);

    variable_item_list_add(app->variable_item_list, "RIC Max:", 1, NULL, app);
    pocsag_pager_scene_receiver_config_update_ric_max(app);

    pcsg_config_row_add(app, PCSGSettingIndexFlexPhase);
    pcsg_config_row_add(app, PCSGSettingIndexFrameFilter);

    item = variable_item_list_add(app->variable_item_list, "Frame:", 1, NULL, app);
    pocsag_pager_scene_receiver_config_set_capcode_text(item, app->filter.frame_min);

    variable_item_list_add(app->variable_item_list, "Frame Max:", 1, NULL, app);
    pocsag_pager_scene_receiver_config_update_frame_max(app);

    pcsg_config_row_add(app, PCSGSettingIndexFreqMode);

    item = variable_item_list_add(
        app->variable_item_list,
        "ppm +/- :",
        FREQ_PPM_COUNT,
        pocsag_pager_scene_receiver_config_set_freq_corr,
        app);
    value_index = (uint8_t)CLAMP(
        app->filter.freq_correction_ppm + PCSG_FREQ_PPM_MAX, (int32_t)FREQ_PPM_COUNT - 1, 0);
    variable_item_set_current_value_index(item, value_index);
    char ppm_buf[8];
    snprintf(ppm_buf, sizeof(ppm_buf), "%+ld", (long)app->filter.freq_correction_ppm);
    variable_item_set_current_value_text(item, ppm_buf);

    pcsg_config_row_add(app, PCSGSettingIndexRawCapture);
    pcsg_config_row_add(app, PCSGSettingIndexStatsLog);
    pcsg_config_row_add(app, PCSGSettingIndexWeakDecode);

    // Locking the keyboard only makes sense while receiving.
    uint8_t selected = app->config_selected_index;
    if(app->config_standalone) {
        if(selected >= PCSGSettingIndexLock) selected = 0;
    } else {
        variable_item_list_add(app->variable_item_list, "Lock Keyboard", 1, NULL, NULL);
    }
    variable_item_list_set_enter_callback(
        app->variable_item_list, pocsag_pager_scene_receiver_config_var_list_enter_callback, app);
    variable_item_list_set_selected_item(app->variable_item_list, selected);

    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewVariableItemList);
}

bool pocsag_pager_scene_receiver_config_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == PCSGCustomEventSceneSettingLock) {
            app->lock = PCSGLockOn;
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        } else if(
            event.event == PCSGCustomEventSceneSettingCapcodeMin ||
            event.event == PCSGCustomEventSceneSettingCapcodeMax ||
            event.event == PCSGCustomEventSceneSettingFrameMin ||
            event.event == PCSGCustomEventSceneSettingFrameMax ||
            event.event == PCSGCustomEventSceneSettingRicMin ||
            event.event == PCSGCustomEventSceneSettingRicMax) {
            PCSGNumberField field = PCSGNumberFieldCapcodeMin;
            if(event.event == PCSGCustomEventSceneSettingCapcodeMax) {
                field = PCSGNumberFieldCapcodeMax;
            } else if(event.event == PCSGCustomEventSceneSettingRicMin) {
                field = PCSGNumberFieldRicMin;
            } else if(event.event == PCSGCustomEventSceneSettingRicMax) {
                field = PCSGNumberFieldRicMax;
            } else if(event.event == PCSGCustomEventSceneSettingFrameMin) {
                field = PCSGNumberFieldFrameMin;
            } else if(event.event == PCSGCustomEventSceneSettingFrameMax) {
                field = PCSGNumberFieldFrameMax;
            }
            scene_manager_set_scene_state(app->scene_manager, POCSAGPagerSceneCapcodeInput, field);
            scene_manager_next_scene(app->scene_manager, POCSAGPagerSceneCapcodeInput);
            consumed = true;
        }
    }
    return consumed;
}

void pocsag_pager_scene_receiver_config_on_exit(void* context) {
    POCSAGPagerApp* app = context;
    app->config_selected_index =
        variable_item_list_get_selected_item_index(app->variable_item_list);
    variable_item_list_set_selected_item(app->variable_item_list, 0);
    variable_item_list_reset(app->variable_item_list);
    pcsg_settings_save(app);
}
