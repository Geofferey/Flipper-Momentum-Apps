#include "../pocsag_pager_app_i.h"
#include "../views/pocsag_pager_receiver.h"
#include "../protocols/flex.h"

static const NotificationSequence subghs_sequence_rx = {
    &message_green_255,

    &message_vibro_on,
    &message_note_c6,
    &message_delay_50,
    &message_sound_off,
    &message_vibro_off,

    &message_delay_50,
    NULL,
};

static const NotificationSequence subghs_sequence_rx_locked = {
    &message_green_255,

    &message_display_backlight_on,

    &message_vibro_on,
    &message_note_c6,
    &message_delay_50,
    &message_sound_off,
    &message_vibro_off,

    &message_delay_500,

    &message_display_backlight_off,
    NULL,
};

static const NotificationSequence pcsg_sequence_rx_sound = {
    &message_green_255,
    &message_note_c6,
    &message_delay_50,
    &message_sound_off,
    &message_delay_50,
    NULL,
};

static const NotificationSequence pcsg_sequence_rx_vibro = {
    &message_green_255,
    &message_vibro_on,
    &message_delay_50,
    &message_vibro_off,
    &message_delay_50,
    NULL,
};

static const NotificationSequence pcsg_sequence_rx_silent = {
    &message_green_255,
    &message_delay_50,
    NULL,
};

static const NotificationSequence pcsg_sequence_rx_sound_locked = {
    &message_green_255,
    &message_display_backlight_on,
    &message_note_c6,
    &message_delay_50,
    &message_sound_off,
    &message_delay_500,
    &message_display_backlight_off,
    NULL,
};

static const NotificationSequence pcsg_sequence_rx_vibro_locked = {
    &message_green_255,
    &message_display_backlight_on,
    &message_vibro_on,
    &message_delay_50,
    &message_vibro_off,
    &message_delay_500,
    &message_display_backlight_off,
    NULL,
};

static const NotificationSequence pcsg_sequence_rx_silent_locked = {
    &message_green_255,
    &message_display_backlight_on,
    &message_delay_500,
    &message_display_backlight_off,
    NULL,
};

// New-message notification for the Alert setting; locked variants also wake
// the backlight briefly.
static const NotificationSequence* pocsag_pager_scene_receiver_alert(POCSAGPagerApp* app) {
    const bool locked = app->lock == PCSGLockOn;
    switch(app->filter.alert) {
    case PCSGAlertSound:
        if(locked) return &pcsg_sequence_rx_sound_locked;
        return &pcsg_sequence_rx_sound;
    case PCSGAlertVibro:
        if(locked) return &pcsg_sequence_rx_vibro_locked;
        return &pcsg_sequence_rx_vibro;
    case PCSGAlertOff:
        if(locked) return &pcsg_sequence_rx_silent_locked;
        return &pcsg_sequence_rx_silent;
    default:
        if(locked) return &subghs_sequence_rx_locked;
        return &subghs_sequence_rx;
    }
}

static void pocsag_pager_scene_receiver_update_statusbar(void* context) {
    POCSAGPagerApp* app = context;
    FuriString* history_stat_str;
    history_stat_str = furi_string_alloc();
    if(!pcsg_history_get_text_space_left(app->txrx->history, history_stat_str)) {
        FuriString* frequency_str;
        FuriString* modulation_str;

        frequency_str = furi_string_alloc();
        modulation_str = furi_string_alloc();

        pcsg_get_frequency_modulation(app, frequency_str, modulation_str);

        pcsg_view_receiver_add_data_statusbar(
            app->pcsg_receiver,
            furi_string_get_cstr(frequency_str),
            furi_string_get_cstr(modulation_str),
            furi_string_get_cstr(history_stat_str));

        furi_string_free(frequency_str);
        furi_string_free(modulation_str);
    } else {
        pcsg_view_receiver_add_data_statusbar(
            app->pcsg_receiver, furi_string_get_cstr(history_stat_str), "", "");
    }
    furi_string_free(history_stat_str);
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
    // FLEX reports each page of a frame from inside its decoder and keeps its
    // symbol clock running between frames, so it must not be reset here.
    const bool flex = decoder_base->protocol == &subghz_protocol_flex;
    if(!pcsg_filter_accepts(&app->filter, decoder_base)) {
        if(!flex) subghz_receiver_reset(receiver);
        return;
    }

    const bool dropped_oldest = pcsg_history_is_full(app->txrx->history);
    if(pcsg_history_add_to_history(app->txrx->history, decoder_base, app->txrx->preset) ==
       PCSGHistoryStateAddKeyNewDada) {
        pcsg_view_receiver_add_item(app->pcsg_receiver, dropped_oldest);
        if(dropped_oldest && app->txrx->idx_menu_chosen) app->txrx->idx_menu_chosen--;

        pocsag_pager_scene_receiver_update_statusbar(app);
        notification_message(app->notifications, &sequence_blink_green_10);
        notification_message(app->notifications, pocsag_pager_scene_receiver_alert(app));
    }
    if(!flex) subghz_receiver_reset(receiver);
    app->txrx->rx_key_state = PCSGRxKeyStateAddKey;
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
        }

        if(app->txrx->txrx_state == PCSGTxRxStateRx) {
            notification_message(app->notifications, &sequence_blink_cyan_10);
        }
    }
    return consumed;
}

void pocsag_pager_scene_receiver_on_exit(void* context) {
    UNUSED(context);
}
