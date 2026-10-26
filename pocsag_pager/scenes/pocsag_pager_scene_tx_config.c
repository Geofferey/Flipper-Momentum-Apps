#include "../pocsag_pager_app_i.h"

typedef enum {
    PCSGTxConfigProtocol,
    PCSGTxConfigFrequency,
    PCSGTxConfigRic,
    PCSGTxConfigFunction,
    PCSGTxConfigBaud,
    PCSGTxConfigPolarity,
    PCSGTxConfigCorrection,
    PCSGTxConfigMessage,
    PCSGTxConfigReview,
} PCSGTxConfigIndex;

static const uint16_t tx_baud_values[] = {512, 1200, 2400};
static const char* const tx_baud_text[] = {"512", "1200", "2400"};
static const char* const tx_protocol_text[] = {"POCSAG", "FLEX"};
static const char* const tx_function_text[] = {"0", "1", "2", "3"};
static const char* const tx_polarity_text[] = {"Standard", "Inverted"};

#define PCSG_TX_CORRECTION_COUNT                                                   \
    ((PCSG_TX_CORRECTION_MAX_HZ - PCSG_TX_CORRECTION_MIN_HZ) /                    \
         PCSG_TX_CORRECTION_STEP_HZ +                                             \
     1)

static uint8_t pocsag_pager_scene_tx_frequency_index(POCSAGPagerApp* app) {
    for(uint8_t i = 0; i < subghz_setting_get_frequency_count(app->setting); i++) {
        if(subghz_setting_get_frequency(app->setting, i) == app->tx_config.frequency) return i;
    }
    return subghz_setting_get_frequency_default_index(app->setting);
}

static uint8_t pocsag_pager_scene_tx_baud_index(uint16_t baud) {
    for(uint8_t i = 0; i < COUNT_OF(tx_baud_values); i++) {
        if(tx_baud_values[i] == baud) return i;
    }
    return 1;
}

static uint8_t pocsag_pager_scene_tx_correction_index(int32_t correction_hz) {
    if(correction_hz < PCSG_TX_CORRECTION_MIN_HZ ||
       correction_hz > PCSG_TX_CORRECTION_MAX_HZ ||
       correction_hz % PCSG_TX_CORRECTION_STEP_HZ != 0) {
        correction_hz = 0;
    }
    return (correction_hz - PCSG_TX_CORRECTION_MIN_HZ) / PCSG_TX_CORRECTION_STEP_HZ;
}

static void pocsag_pager_scene_tx_show_correction(VariableItem* item, int32_t correction_hz) {
    if(correction_hz == 0) {
        variable_item_set_current_value_text(item, "Off");
        return;
    }

    char text[12];
    const uint32_t magnitude = correction_hz < 0 ? -correction_hz : correction_hz;
    snprintf(
        text,
        sizeof(text),
        "%c%lu.%01lu kHz",
        correction_hz < 0 ? '-' : '+',
        magnitude / 1000U,
        (magnitude % 1000U) / 100U);
    variable_item_set_current_value_text(item, text);
}

static void pocsag_pager_scene_tx_set_frequency(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->tx_config.frequency = subghz_setting_get_frequency(app->setting, index);
    char text[12];
    snprintf(
        text,
        sizeof(text),
        "%lu.%04lu",
        app->tx_config.frequency / 1000000U,
        (app->tx_config.frequency % 1000000U) / 100U);
    variable_item_set_current_value_text(item, text);
}

static void pocsag_pager_scene_tx_show_address(POCSAGPagerApp* app) {
    VariableItem* item =
        variable_item_list_get(app->variable_item_list, PCSGTxConfigRic);
    variable_item_set_item_label(
        item, app->tx_config.protocol == PagerTxProtocolFlex ? "Capcode:" : "RIC:");
    char text[20];
    snprintf(text, sizeof(text), "%lu", app->tx_config.ric);
    variable_item_set_current_value_text(item, text);
}

static void pocsag_pager_scene_tx_show_protocol_options(POCSAGPagerApp* app) {
    const bool flex = app->tx_config.protocol == PagerTxProtocolFlex;
    pocsag_pager_scene_tx_show_address(app);

    VariableItem* function =
        variable_item_list_get(app->variable_item_list, PCSGTxConfigFunction);
    variable_item_set_values_count(function, flex ? 1U : COUNT_OF(tx_function_text));
    variable_item_set_current_value_index(function, flex ? 0U : app->tx_config.function);
    variable_item_set_current_value_text(
        function, flex ? "N/A" : tx_function_text[app->tx_config.function]);

    VariableItem* baud = variable_item_list_get(app->variable_item_list, PCSGTxConfigBaud);
    variable_item_set_item_label(baud, flex ? "Mode:" : "Baud:");
    variable_item_set_values_count(baud, flex ? 1U : COUNT_OF(tx_baud_values));
    if(flex) {
        variable_item_set_current_value_index(baud, 0);
        variable_item_set_current_value_text(baud, "1600/2FSK");
    } else {
        const uint8_t baud_index = pocsag_pager_scene_tx_baud_index(app->tx_config.baud);
        variable_item_set_current_value_index(baud, baud_index);
        variable_item_set_current_value_text(baud, tx_baud_text[baud_index]);
    }
}

static void pocsag_pager_scene_tx_set_protocol(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    app->tx_config.protocol = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, tx_protocol_text[app->tx_config.protocol]);
    if(app->tx_config.protocol == PagerTxProtocolFlex) {
        if(app->tx_config.ric < FLEX_TX_SHORT_CAPCODE_MIN) {
            app->tx_config.ric = FLEX_TX_SHORT_CAPCODE_MIN;
        } else if(app->tx_config.ric > FLEX_TX_SHORT_CAPCODE_MAX) {
            app->tx_config.ric = FLEX_TX_SHORT_CAPCODE_MAX;
        }
    }
    pocsag_pager_scene_tx_show_protocol_options(app);
}

static void pocsag_pager_scene_tx_set_function(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    if(app->tx_config.protocol == PagerTxProtocolFlex) {
        variable_item_set_current_value_index(item, 0);
        variable_item_set_current_value_text(item, "N/A");
        return;
    }
    app->tx_config.function = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, tx_function_text[app->tx_config.function]);
}

static void pocsag_pager_scene_tx_set_baud(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    if(app->tx_config.protocol == PagerTxProtocolFlex) {
        variable_item_set_current_value_index(item, 0);
        variable_item_set_current_value_text(item, "1600/2FSK");
        return;
    }
    const uint8_t index = variable_item_get_current_value_index(item);
    app->tx_config.baud = tx_baud_values[index];
    variable_item_set_current_value_text(item, tx_baud_text[index]);
}

static void pocsag_pager_scene_tx_set_polarity(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    app->tx_config.inverted = variable_item_get_current_value_index(item) != 0;
    variable_item_set_current_value_text(
        item, tx_polarity_text[app->tx_config.inverted ? 1U : 0U]);
}

static void pocsag_pager_scene_tx_set_correction(VariableItem* item) {
    POCSAGPagerApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->tx_config.frequency_correction_hz =
        PCSG_TX_CORRECTION_MIN_HZ + index * PCSG_TX_CORRECTION_STEP_HZ;
    pocsag_pager_scene_tx_show_correction(item, app->tx_config.frequency_correction_hz);
}

static void pocsag_pager_scene_tx_enter_callback(void* context, uint32_t index) {
    POCSAGPagerApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void pocsag_pager_scene_tx_config_on_enter(void* context) {
    POCSAGPagerApp* app = context;
    VariableItem* item;
    char text[20];

    if(app->tx_config.protocol == PagerTxProtocolFlex) {
        if(app->tx_config.ric < FLEX_TX_SHORT_CAPCODE_MIN) {
            app->tx_config.ric = FLEX_TX_SHORT_CAPCODE_MIN;
        } else if(app->tx_config.ric > FLEX_TX_SHORT_CAPCODE_MAX) {
            app->tx_config.ric = FLEX_TX_SHORT_CAPCODE_MAX;
        }
    }

    item = variable_item_list_add(
        app->variable_item_list,
        "Protocol:",
        PagerTxProtocolCount,
        pocsag_pager_scene_tx_set_protocol,
        app);
    variable_item_set_current_value_index(item, app->tx_config.protocol);
    variable_item_set_current_value_text(item, tx_protocol_text[app->tx_config.protocol]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Frequency:",
        subghz_setting_get_frequency_count(app->setting),
        pocsag_pager_scene_tx_set_frequency,
        app);
    variable_item_set_current_value_index(item, pocsag_pager_scene_tx_frequency_index(app));
    pocsag_pager_scene_tx_set_frequency(item);

    item = variable_item_list_add(
        app->variable_item_list,
        app->tx_config.protocol == PagerTxProtocolFlex ? "Capcode:" : "RIC:",
        1,
        NULL,
        app);
    snprintf(text, sizeof(text), "%lu", app->tx_config.ric);
    variable_item_set_current_value_text(item, text);

    item = variable_item_list_add(
        app->variable_item_list,
        "Function:",
        app->tx_config.protocol == PagerTxProtocolFlex ? 1U : COUNT_OF(tx_function_text),
        pocsag_pager_scene_tx_set_function,
        app);
    variable_item_set_current_value_index(
        item, app->tx_config.protocol == PagerTxProtocolFlex ? 0U : app->tx_config.function);
    variable_item_set_current_value_text(
        item,
        app->tx_config.protocol == PagerTxProtocolFlex ?
            "N/A" :
            tx_function_text[app->tx_config.function]);

    item = variable_item_list_add(
        app->variable_item_list,
        app->tx_config.protocol == PagerTxProtocolFlex ? "Mode:" : "Baud:",
        app->tx_config.protocol == PagerTxProtocolFlex ? 1U : COUNT_OF(tx_baud_values),
        pocsag_pager_scene_tx_set_baud,
        app);
    const uint8_t baud_index = app->tx_config.protocol == PagerTxProtocolFlex ?
                                   0U :
                                   pocsag_pager_scene_tx_baud_index(app->tx_config.baud);
    variable_item_set_current_value_index(item, baud_index);
    variable_item_set_current_value_text(
        item,
        app->tx_config.protocol == PagerTxProtocolFlex ?
            "1600/2FSK" :
            tx_baud_text[baud_index]);

    item = variable_item_list_add(
        app->variable_item_list,
        "Polarity:",
        COUNT_OF(tx_polarity_text),
        pocsag_pager_scene_tx_set_polarity,
        app);
    const uint8_t polarity_index = app->tx_config.inverted ? 1U : 0U;
    variable_item_set_current_value_index(item, polarity_index);
    variable_item_set_current_value_text(item, tx_polarity_text[polarity_index]);

    item = variable_item_list_add(
        app->variable_item_list,
        "TX correction:",
        PCSG_TX_CORRECTION_COUNT,
        pocsag_pager_scene_tx_set_correction,
        app);
    const uint8_t correction_index =
        pocsag_pager_scene_tx_correction_index(app->tx_config.frequency_correction_hz);
    variable_item_set_current_value_index(item, correction_index);
    pocsag_pager_scene_tx_set_correction(item);

    item = variable_item_list_add(app->variable_item_list, "Message:", 1, NULL, app);
    snprintf(text, sizeof(text), "%u chars", (unsigned)strlen(app->tx_config.message));
    variable_item_set_current_value_text(item, text);

    item = variable_item_list_add(app->variable_item_list, "Review & send", 1, NULL, app);
    variable_item_set_current_value_text(item, ">");

    variable_item_list_set_enter_callback(
        app->variable_item_list, pocsag_pager_scene_tx_enter_callback, app);
    variable_item_list_set_selected_item(
        app->variable_item_list, app->tx_config_selected_index);
    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewVariableItemList);
}

bool pocsag_pager_scene_tx_config_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == PCSGTxConfigRic) {
        scene_manager_next_scene(app->scene_manager, POCSAGPagerSceneTxRicInput);
        return true;
    }
    if(event.event == PCSGTxConfigMessage) {
        scene_manager_next_scene(app->scene_manager, POCSAGPagerSceneTxMessageInput);
        return true;
    }
    if(event.event == PCSGTxConfigReview) {
        if(app->tx_config.message[0] == '\0') {
            notification_message(app->notifications, &sequence_error);
        } else {
            scene_manager_next_scene(app->scene_manager, POCSAGPagerSceneTxConfirm);
        }
        return true;
    }
    return false;
}

void pocsag_pager_scene_tx_config_on_exit(void* context) {
    POCSAGPagerApp* app = context;
    app->tx_config_selected_index =
        variable_item_list_get_selected_item_index(app->variable_item_list);
    variable_item_list_reset(app->variable_item_list);
    pcsg_settings_save(app);
}
