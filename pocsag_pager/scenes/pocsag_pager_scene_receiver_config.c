#include "../pocsag_pager_app_i.h"

enum PCSGSettingIndex {
    PCSGSettingIndexFrequency,
    PCSGSettingIndexHopping,
    PCSGSettingIndexModulation,
    PCSGSettingIndexFlexTypes,
    PCSGSettingIndexFlexBinary,
    PCSGSettingIndexFlexDups,
    PCSGSettingIndexFlexEncrypted,
    PCSGSettingIndexLogging,
    PCSGSettingIndexAlert,
    PCSGSettingIndexSignalSound,
    PCSGSettingIndexToneVolume,
    PCSGSettingIndexTonePitch,
    PCSGSettingIndexToneSpread,
    PCSGSettingIndexCapcodeFilter,
    PCSGSettingIndexCapcodeMin,
    PCSGSettingIndexCapcodeMax,
    PCSGSettingIndexFlexPhase,
    PCSGSettingIndexFrameFilter,
    PCSGSettingIndexFrameMin,
    PCSGSettingIndexFrameMax,
    PCSGSettingIndexLock,
};

const char* const phase_filter_text[PCSGPhaseFilterCount] = {
    "All",
    "A",
    "B",
    "C",
    "D",
};

const char* const flex_types_text[PCSGTypeFilterCount] = {
    "All",
    "Alpha",
    "Alpha+Num",
    "No Tones",
};

#define FLEX_BINARY_COUNT 2
const char* const flex_binary_text[FLEX_BINARY_COUNT] = {
    "Off",
    "Hex",
};

#define FLEX_DUPS_COUNT 2
const char* const flex_dups_text[FLEX_DUPS_COUNT] = {
    "Hide",
    "Show",
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

const char* const signal_sound_text[PCSGSoundCount] = {
    "Off",
    "Raw",
    "Tone",
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

// The maximum is only used in range mode; its text shows that.
static VariableItem* capcode_max_item;
static VariableItem* frame_max_item;

#define HOPPING_COUNT 2
const char* const hopping_text[HOPPING_COUNT] = {
    "OFF",
    "ON",
};
const uint32_t hopping_value[HOPPING_COUNT] = {
    PCSGHopperStateOFF,
    PCSGHopperStateRunnig,
};

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

uint8_t pocsag_pager_scene_receiver_config_hopper_value_index(
    const uint32_t value,
    const uint32_t values[],
    uint8_t values_count,
    void* context) {
    furi_assert(context);
    UNUSED(values_count);
    POCSAGPagerApp* app = context;

    if(value == values[0]) {
        return 0;
    } else {
        variable_item_set_current_value_text(
            (VariableItem*)scene_manager_get_scene_state(
                app->scene_manager, POCSAGPagerSceneReceiverConfig),
            " -----");
        return 1;
    }
}

static void pocsag_pager_scene_receiver_config_set_frequency(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    if(app->txrx->hopper_state == PCSGHopperStateOFF) {
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
    } else {
        variable_item_set_current_value_index(
            item, subghz_setting_get_frequency_default_index(app->setting));
    }
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

static void pocsag_pager_scene_receiver_config_set_hopping_running(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, hopping_text[index]);
    if(hopping_value[index] == PCSGHopperStateOFF) {
        char text_buf[10] = {0};
        snprintf(
            text_buf,
            sizeof(text_buf),
            "%lu.%02lu",
            subghz_setting_get_default_frequency(app->setting) / 1000000,
            (subghz_setting_get_default_frequency(app->setting) % 1000000) / 10000);
        variable_item_set_current_value_text(
            (VariableItem*)scene_manager_get_scene_state(
                app->scene_manager, POCSAGPagerSceneReceiverConfig),
            text_buf);
        app->txrx->preset->frequency = subghz_setting_get_default_frequency(app->setting);
        variable_item_set_current_value_index(
            (VariableItem*)scene_manager_get_scene_state(
                app->scene_manager, POCSAGPagerSceneReceiverConfig),
            subghz_setting_get_frequency_default_index(app->setting));
    } else {
        variable_item_set_current_value_text(
            (VariableItem*)scene_manager_get_scene_state(
                app->scene_manager, POCSAGPagerSceneReceiverConfig),
            " -----");
        variable_item_set_current_value_index(
            (VariableItem*)scene_manager_get_scene_state(
                app->scene_manager, POCSAGPagerSceneReceiverConfig),
            subghz_setting_get_frequency_default_index(app->setting));
    }

    app->txrx->hopper_state = hopping_value[index];
    app->txrx->rx_restart = true;
}

static void pocsag_pager_scene_receiver_config_set_capcode_text(VariableItem* item, uint32_t value) {
    char text_buf[10] = {0};
    snprintf(text_buf, sizeof(text_buf), "%lu", value);
    variable_item_set_current_value_text(item, text_buf);
}

static void pocsag_pager_scene_receiver_config_update_capcode_max(POCSAGPagerApp* app) {
    if(app->filter.capcode_filter == PCSGCapcodeFilterRange) {
        pocsag_pager_scene_receiver_config_set_capcode_text(
            capcode_max_item, app->filter.capcode_max);
    } else {
        variable_item_set_current_value_text(capcode_max_item, "--");
    }
}

static void pocsag_pager_scene_receiver_config_update_frame_max(POCSAGPagerApp* app) {
    if(app->filter.frame_filter == PCSGCapcodeFilterRange) {
        pocsag_pager_scene_receiver_config_set_capcode_text(frame_max_item, app->filter.frame_max);
    } else {
        variable_item_set_current_value_text(frame_max_item, "--");
    }
}

static void pocsag_pager_scene_receiver_config_set_phase_filter(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, phase_filter_text[index]);
    app->filter.phase_filter = index;
}

static void pocsag_pager_scene_receiver_config_set_frame_filter(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, capcode_filter_text[index]);
    app->filter.frame_filter = index;
    pocsag_pager_scene_receiver_config_update_frame_max(app);
}

static void pocsag_pager_scene_receiver_config_set_flex_types(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, flex_types_text[index]);
    app->filter.type_filter = index;
}

static void pocsag_pager_scene_receiver_config_set_flex_binary(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, flex_binary_text[index]);
    app->filter.show_binary = index != 0;
}

static void pocsag_pager_scene_receiver_config_set_flex_dups(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, flex_dups_text[index]);
    app->filter.hide_duplicates = index == 0;
}

static void pocsag_pager_scene_receiver_config_set_flex_encrypted(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, flex_dups_text[index]);
    app->filter.hide_encrypted = index == 0;
}

// Applies from the next receive session.
static void pocsag_pager_scene_receiver_config_set_logging(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, logging_text[index]);
    app->filter.log_temp = index == 1;
}

static void pocsag_pager_scene_receiver_config_set_alert(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, alert_text[index]);
    app->filter.alert = index;
}

// The speaker is set up when receiving starts, so restart the radio.
static void pocsag_pager_scene_receiver_config_set_signal_sound(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, signal_sound_text[index]);
    app->filter.signal_sound = index;
    app->txrx->rx_restart = true;
}

static void pocsag_pager_scene_receiver_config_set_tone_volume(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, tone_volume_text[index]);
    app->filter.tone_volume = index;
    app->txrx->rx_restart = true;
}

static void pocsag_pager_scene_receiver_config_set_tone_pitch(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, tone_pitch_text[index]);
    app->filter.tone_pitch = index;
    app->txrx->rx_restart = true;
}

static void pocsag_pager_scene_receiver_config_set_tone_spread(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, tone_spread_text[index]);
    app->filter.tone_spread = index;
    app->txrx->rx_restart = true;
}

static void pocsag_pager_scene_receiver_config_set_capcode_filter(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, capcode_filter_text[index]);
    app->filter.capcode_filter = index;
    pocsag_pager_scene_receiver_config_update_capcode_max(app);
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
    scene_manager_set_scene_state(
        app->scene_manager, POCSAGPagerSceneReceiverConfig, (uint32_t)item);
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
        "Hopping:",
        HOPPING_COUNT,
        pocsag_pager_scene_receiver_config_set_hopping_running,
        app);
    value_index = pocsag_pager_scene_receiver_config_hopper_value_index(
        app->txrx->hopper_state, hopping_value, HOPPING_COUNT, app);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, hopping_text[value_index]);

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

    item = variable_item_list_add(
        app->variable_item_list,
        "FLEX Types:",
        PCSGTypeFilterCount,
        pocsag_pager_scene_receiver_config_set_flex_types,
        app);
    variable_item_set_current_value_index(item, app->filter.type_filter);
    variable_item_set_current_value_text(item, flex_types_text[app->filter.type_filter]);

    item = variable_item_list_add(
        app->variable_item_list,
        "FLEX Binary:",
        FLEX_BINARY_COUNT,
        pocsag_pager_scene_receiver_config_set_flex_binary,
        app);
    variable_item_set_current_value_index(item, app->filter.show_binary);
    variable_item_set_current_value_text(item, flex_binary_text[app->filter.show_binary]);

    item = variable_item_list_add(
        app->variable_item_list,
        "FLEX Dups:",
        FLEX_DUPS_COUNT,
        pocsag_pager_scene_receiver_config_set_flex_dups,
        app);
    value_index = app->filter.hide_duplicates ? 0 : 1;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, flex_dups_text[value_index]);

    // Hide/Show, the same choices as duplicates.
    item = variable_item_list_add(
        app->variable_item_list,
        "Encrypted:",
        FLEX_DUPS_COUNT,
        pocsag_pager_scene_receiver_config_set_flex_encrypted,
        app);
    value_index = app->filter.hide_encrypted ? 0 : 1;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, flex_dups_text[value_index]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Logging:",
        LOGGING_COUNT,
        pocsag_pager_scene_receiver_config_set_logging,
        app);
    value_index = app->filter.log_temp ? 1 : 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, logging_text[value_index]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Alert:",
        PCSGAlertCount,
        pocsag_pager_scene_receiver_config_set_alert,
        app);
    variable_item_set_current_value_index(item, app->filter.alert);
    variable_item_set_current_value_text(item, alert_text[app->filter.alert]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Signal Sound:",
        PCSGSoundCount,
        pocsag_pager_scene_receiver_config_set_signal_sound,
        app);
    variable_item_set_current_value_index(item, app->filter.signal_sound);
    variable_item_set_current_value_text(item, signal_sound_text[app->filter.signal_sound]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Tone Volume:",
        PCSGToneVolumeCount,
        pocsag_pager_scene_receiver_config_set_tone_volume,
        app);
    variable_item_set_current_value_index(item, app->filter.tone_volume);
    variable_item_set_current_value_text(item, tone_volume_text[app->filter.tone_volume]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Tone Pitch:",
        PCSGTonePitchCount,
        pocsag_pager_scene_receiver_config_set_tone_pitch,
        app);
    variable_item_set_current_value_index(item, app->filter.tone_pitch);
    variable_item_set_current_value_text(item, tone_pitch_text[app->filter.tone_pitch]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Tone Spread:",
        PCSGToneSpreadCount,
        pocsag_pager_scene_receiver_config_set_tone_spread,
        app);
    variable_item_set_current_value_index(item, app->filter.tone_spread);
    variable_item_set_current_value_text(item, tone_spread_text[app->filter.tone_spread]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Cap. Filter:",
        PCSGCapcodeFilterCount,
        pocsag_pager_scene_receiver_config_set_capcode_filter,
        app);
    variable_item_set_current_value_index(item, app->filter.capcode_filter);
    variable_item_set_current_value_text(item, capcode_filter_text[app->filter.capcode_filter]);

    item = variable_item_list_add(app->variable_item_list, "Capcode:", 1, NULL, app);
    pocsag_pager_scene_receiver_config_set_capcode_text(item, app->filter.capcode_min);

    capcode_max_item = variable_item_list_add(app->variable_item_list, "Cap. Max:", 1, NULL, app);
    pocsag_pager_scene_receiver_config_update_capcode_max(app);

    item = variable_item_list_add(
        app->variable_item_list,
        "FLEX Phase:",
        PCSGPhaseFilterCount,
        pocsag_pager_scene_receiver_config_set_phase_filter,
        app);
    variable_item_set_current_value_index(item, app->filter.phase_filter);
    variable_item_set_current_value_text(item, phase_filter_text[app->filter.phase_filter]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Frame Filter:",
        PCSGCapcodeFilterCount,
        pocsag_pager_scene_receiver_config_set_frame_filter,
        app);
    variable_item_set_current_value_index(item, app->filter.frame_filter);
    variable_item_set_current_value_text(item, capcode_filter_text[app->filter.frame_filter]);

    item = variable_item_list_add(app->variable_item_list, "Frame:", 1, NULL, app);
    pocsag_pager_scene_receiver_config_set_capcode_text(item, app->filter.frame_min);

    frame_max_item = variable_item_list_add(app->variable_item_list, "Frame Max:", 1, NULL, app);
    pocsag_pager_scene_receiver_config_update_frame_max(app);

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
            event.event == PCSGCustomEventSceneSettingFrameMax) {
            PCSGNumberField field = PCSGNumberFieldCapcodeMin;
            if(event.event == PCSGCustomEventSceneSettingCapcodeMax) {
                field = PCSGNumberFieldCapcodeMax;
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
    capcode_max_item = NULL;
    frame_max_item = NULL;
    pcsg_settings_save(app);
}
