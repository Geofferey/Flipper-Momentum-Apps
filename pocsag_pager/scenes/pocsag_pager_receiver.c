#include "../pocsag_pager_app_i.h"
#include "../views/pocsag_pager_receiver.h"
#include "../protocols/flex.h"
#include "../pocsag_pager_led.h"

static const NotificationSequence subghs_sequence_rx = {
    &message_vibro_on,
    &message_note_c6,
    &message_delay_50,
    &message_sound_off,
    &message_vibro_off,

    &message_delay_50,
    NULL,
};

static const NotificationSequence pcsg_sequence_rx_sound = {
    &message_note_c6,
    &message_delay_50,
    &message_sound_off,
    &message_delay_50,
    NULL,
};

static const NotificationSequence pcsg_sequence_rx_vibro = {
    &message_vibro_on,
    &message_delay_50,
    &message_vibro_off,
    &message_delay_50,
    NULL,
};

static const NotificationSequence pcsg_sequence_rx_silent = {
    &message_delay_50,
    NULL,
};

// Message alerts use sound/vibration only; reception never wakes the backlight.
static const NotificationSequence* pocsag_pager_scene_receiver_alert(POCSAGPagerApp* app) {
    switch(app->filter.alert) {
    case PCSGAlertSound:
        return &pcsg_sequence_rx_sound;
    case PCSGAlertVibro:
        return &pcsg_sequence_rx_vibro;
    case PCSGAlertOff:
        return &pcsg_sequence_rx_silent;
    default:
        return &subghs_sequence_rx;
    }
}

static void pocsag_pager_scene_receiver_update_statusbar(void* context) {
    POCSAGPagerApp* app = context;
    char frequency[12];
    char history[12];
    snprintf(
        frequency,
        sizeof(frequency),
        "%03lu.%02lu",
        app->txrx->preset->frequency / 1000000UL % 1000UL,
        app->txrx->preset->frequency / 10000UL % 100UL);
    snprintf(history, sizeof(history), "%lu", pcsg_history_get_total(app->txrx->history));

    const char* preset_name = furi_string_get_cstr(app->txrx->preset->name);
    const char* modulation;
    if(pcsg_preset_is_flex4(preset_name)) {
        modulation = "F4";
    } else if(!strncmp(preset_name, "FLEX", 4)) {
        modulation = "FX";
    } else if(!strcmp(preset_name, "POCSAG-W")) {
        modulation = "PW";
    } else {
        modulation = preset_name;
    }
    char modulation_short[3] = {
        modulation[0], modulation[0] && modulation[1] ? modulation[1] : '\0', '\0'};
    pcsg_view_receiver_add_data_statusbar(
        app->pcsg_receiver, frequency, modulation_short, history);
}

void pocsag_pager_scene_receiver_callback(PCSGCustomEvent event, void* context) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

static void pocsag_pager_scene_receiver_add_to_history_callback(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    void* context) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    // Both decoders report each message from inside a transmission (FLEX per
    // page, POCSAG per address) and reset themselves when it ends, so the
    // receiver must not be reset here.
    UNUSED(receiver);
    const bool display = pcsg_filter_accepts(&app->filter, decoder_base);

    const PCSGHistoryStateAddKey state =
        pcsg_history_add_to_history(
            app->txrx->history, decoder_base, app->txrx->preset, display);
    const uint16_t evicted = pcsg_history_last_evicted(app->txrx->history);
    if(state == PCSGHistoryStateAddKeyNewDada || state == PCSGHistoryStateAddKeyOverflow ||
       evicted) {
        pcsg_view_receiver_history_changed(
            app->pcsg_receiver, pcsg_history_get_item(app->txrx->history), evicted);
        app->txrx->idx_menu_chosen = app->txrx->idx_menu_chosen > evicted ?
                                         app->txrx->idx_menu_chosen - evicted :
                                         0;

        pocsag_pager_scene_receiver_update_statusbar(app);
    }
    if(state == PCSGHistoryStateAddKeyNewDada) {
        if(app->filter.decode_led) {
            notification_message(
                app->notifications, pcsg_led_blink_sequence(app->filter.decode_led_color));
        }
        notification_message(app->notifications, pocsag_pager_scene_receiver_alert(app));
        app->txrx->rx_key_state = PCSGRxKeyStateAddKey;
    }
}

static void pocsag_pager_scene_receiver_item_callback(
    void* context,
    uint16_t idx,
    FuriString* label,
    uint8_t* type) {
    POCSAGPagerApp* app = context;
    pcsg_history_get_text_item_menu(app->txrx->history, label, idx);
    *type = pcsg_history_get_type_protocol(app->txrx->history, idx);
}

void pocsag_pager_scene_receiver_on_enter(void* context) {
    POCSAGPagerApp* app = context;
    const bool flex4 =
        pcsg_preset_is_flex4(furi_string_get_cstr(app->txrx->preset->name));

    // FLEX4 uses the CC1101 MISO/GDO1 pin as a second hard-decision data bit.
    // Frequency hopping and RSSI reads use SPI and would corrupt those samples.
    if(flex4) app->txrx->hopper_state = PCSGHopperStateOFF;

    // A new session keeps the frequency, preset and hopping chosen in Config
    // (and restored from settings at startup).
    if(app->txrx->rx_key_state == PCSGRxKeyStateIDLE) {
        pcsg_history_reset(app->txrx->history);
        pcsg_history_set_temp_mode(app->txrx->history, app->filter.log_temp);
        app->txrx->rx_key_state = PCSGRxKeyStateStart;
    }

    pcsg_view_receiver_set_lock(app->pcsg_receiver, app->lock);
    pcsg_view_receiver_set_ext_module_state(
        app->pcsg_receiver, radio_device_loader_is_external(app->txrx->radio_device));

    //Load history to receiver
    pcsg_view_receiver_exit(app->pcsg_receiver);
    pcsg_view_receiver_set_review_mode(app->pcsg_receiver, false);
    pcsg_view_receiver_set_item_callback(
        app->pcsg_receiver, pocsag_pager_scene_receiver_item_callback, app);
    pcsg_view_receiver_set_item_count(app->pcsg_receiver, pcsg_history_get_item(app->txrx->history));
    if(pcsg_history_get_item(app->txrx->history)) app->txrx->rx_key_state = PCSGRxKeyStateAddKey;
    pocsag_pager_scene_receiver_update_statusbar(app);

    pcsg_view_receiver_set_callback(app->pcsg_receiver, pocsag_pager_scene_receiver_callback, app);
    subghz_receiver_set_rx_callback(
        app->txrx->receiver, pocsag_pager_scene_receiver_add_to_history_callback, app);

    // Returning from a message or an unchanged Config keeps the radio running,
    // so a frame (or the rest of a split message) is not lost to a restart.
    if(app->txrx->txrx_state == PCSGTxRxStateRx && app->txrx->rx_restart) {
        pcsg_rx_end(app);
    };
    app->txrx->rx_restart = false;
    if((app->txrx->txrx_state == PCSGTxRxStateIDLE) ||
       (app->txrx->txrx_state == PCSGTxRxStateSleep)) {
        // Start RX
        pcsg_begin(
            app,
            subghz_setting_get_preset_data_by_name(
                app->setting, furi_string_get_cstr(app->txrx->preset->name)));

        pcsg_rx(app, app->txrx->preset->frequency);
    }

    pcsg_view_receiver_set_idx_menu(app->pcsg_receiver, app->txrx->idx_menu_chosen);
    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewReceiver);
}

bool pocsag_pager_scene_receiver_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case PCSGCustomEventViewReceiverBack:
            // Stop CC1101 Rx
            if(app->txrx->txrx_state == PCSGTxRxStateRx) {
                pcsg_rx_end(app);
                pcsg_idle(app);
            };
            // Frequency, preset and hopping stay as chosen for the next session.
            app->txrx->idx_menu_chosen = 0;
            subghz_receiver_set_rx_callback(app->txrx->receiver, NULL, app);

            app->txrx->rx_key_state = PCSGRxKeyStateIDLE;
            // Close the session log and release its index; it is on SD now.
            pcsg_history_reset(app->txrx->history);
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, POCSAGPagerSceneStart);
            consumed = true;
            break;
        case PCSGCustomEventViewReceiverOK:
            app->txrx->idx_menu_chosen = pcsg_view_receiver_get_idx_menu(app->pcsg_receiver);
            scene_manager_next_scene(app->scene_manager, POCSAGPagerSceneReceiverInfo);
            consumed = true;
            break;
        case PCSGCustomEventViewReceiverFilterAddress: {
            const uint16_t index = pcsg_view_receiver_get_idx_menu(app->pcsg_receiver);
            PCSGHistoryAddressType type;
            uint32_t address;
            if(pcsg_history_get_address(app->txrx->history, index, &type, &address)) {
                if(type == PCSGHistoryAddressFlexCap && address <= PCSG_CAPCODE_MAX) {
                    app->filter.capcode_filter = PCSGCapcodeFilterExact;
                    app->filter.capcode_min = address;
                    app->filter.capcode_max = address;
                    pcsg_view_receiver_show_address_filter(app->pcsg_receiver, true, address);
                    pcsg_settings_save(app);
                } else if(type == PCSGHistoryAddressPocsagRic && address <= PCSG_RIC_MAX) {
                    app->filter.ric_filter = PCSGCapcodeFilterExact;
                    app->filter.ric_min = address;
                    app->filter.ric_max = address;
                    pcsg_view_receiver_show_address_filter(app->pcsg_receiver, false, address);
                    pcsg_settings_save(app);
                }
            }
            consumed = true;
            break;
        }
        case PCSGCustomEventViewReceiverConfig:
            app->config_standalone = false;
            app->txrx->idx_menu_chosen = pcsg_view_receiver_get_idx_menu(app->pcsg_receiver);
            scene_manager_next_scene(app->scene_manager, POCSAGPagerSceneReceiverConfig);
            consumed = true;
            break;
        case PCSGCustomEventViewReceiverOffDisplay:
            notification_message(app->notifications, &sequence_display_backlight_off);
            consumed = true;
            break;
        case PCSGCustomEventViewReceiverUnlock:
            app->lock = PCSGLockOff;
            consumed = true;
            break;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        pcsg_time_sync(app);
        const bool flex4 =
            pcsg_preset_is_flex4(furi_string_get_cstr(app->txrx->preset->name));
        if(!flex4 && app->txrx->hopper_state != PCSGHopperStateOFF) {
            pcsg_hopper_update(app);
            pocsag_pager_scene_receiver_update_statusbar(app);
        }
        if(!flex4) {
            // FLEX4 needs uninterrupted access to both hard-decision GPIOs.
            const float rssi = subghz_devices_get_rssi(app->txrx->radio_device);
            pcsg_receiver_rssi(app->pcsg_receiver, rssi);
            pcsg_freq_auto_tick(app, rssi);
        } else {
            // No RSSI to gate on here for the same reason; the FLEX4 path
            // pauses sampling between frames to measure instead.
            pcsg_freq_auto_tick(app, 0.0f);
        }

        if(app->txrx->txrx_state == PCSGTxRxStateRx && app->filter.receive_led) {
            notification_message(
                app->notifications,
                pcsg_led_blink_sequence(app->filter.receive_led_color));
        }
    }
    return consumed;
}

void pocsag_pager_scene_receiver_on_exit(void* context) {
    UNUSED(context);
}
