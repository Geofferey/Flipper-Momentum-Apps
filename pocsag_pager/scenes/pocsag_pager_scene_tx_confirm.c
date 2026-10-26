#include "../pocsag_pager_app_i.h"

static void pocsag_pager_scene_tx_confirm_callback(
    GuiButtonType result,
    InputType type,
    void* context) {
    POCSAGPagerApp* app = context;
    if(result == GuiButtonTypeCenter && type == InputTypeShort) {
        view_dispatcher_send_custom_event(app->view_dispatcher, PCSGCustomEventTxSend);
    }
}

static void pocsag_pager_scene_tx_confirm_show_result(POCSAGPagerApp* app, const char* text) {
    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget, 4, 18, 120, 32, AlignCenter, AlignCenter, text, false);
}

void pocsag_pager_scene_tx_confirm_on_enter(void* context) {
    POCSAGPagerApp* app = context;
    char correction[16];
    if(app->tx_config.frequency_correction_hz == 0) {
        strlcpy(correction, "Off", sizeof(correction));
    } else {
        const int32_t correction_hz = app->tx_config.frequency_correction_hz;
        const uint32_t magnitude =
            correction_hz < 0 ? (uint32_t)-correction_hz : (uint32_t)correction_hz;
        snprintf(
            correction,
            sizeof(correction),
            "%c%lu Hz",
            correction_hz < 0 ? '-' : '+',
            (unsigned long)magnitude);
    }
    FuriString* text;
    if(app->tx_config.protocol == PagerTxProtocolFlex) {
        text = furi_string_alloc_printf(
            "\e#Transmit FLEX?\e#\n"
            "%lu.%04lu MHz | 1600/2FSK\n"
            "Capcode %lu\n"
            "%s polarity | Correction %s\n\n"
            "%s\n\n"
            "Only transmit where authorized.",
            app->tx_config.frequency / 1000000U,
            (app->tx_config.frequency % 1000000U) / 100U,
            app->tx_config.ric,
            app->tx_config.inverted ? "Inverted" : "Standard",
            correction,
            app->tx_config.message);
    } else {
        text = furi_string_alloc_printf(
            "\e#Transmit POCSAG?\e#\n"
            "%lu.%04lu MHz | %u baud\n"
            "RIC %lu | Function %u\n"
            "%s polarity | Correction %s\n\n"
            "%s\n\n"
            "Only transmit where authorized.",
            app->tx_config.frequency / 1000000U,
            (app->tx_config.frequency % 1000000U) / 100U,
            app->tx_config.baud,
            app->tx_config.ric,
            app->tx_config.function,
            app->tx_config.inverted ? "Inverted" : "Standard",
            correction,
            app->tx_config.message);
    }
    widget_add_text_scroll_element(app->widget, 2, 0, 124, 52, furi_string_get_cstr(text));
    widget_add_button_element(
        app->widget,
        GuiButtonTypeCenter,
        "Send",
        pocsag_pager_scene_tx_confirm_callback,
        app);
    furi_string_free(text);
    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewWidget);
}

bool pocsag_pager_scene_tx_confirm_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == PCSGCustomEventTxSend) {
        const PCSGTxStartStatus status = pcsg_tx_start(app);
        if(status == PCSGTxStartOk) {
            pocsag_pager_scene_tx_confirm_show_result(app, "\e#Transmitting...\e#\n\nBack cancels");
            notification_message(app->notifications, &sequence_blink_magenta_10);
        } else if(status == PCSGTxStartRestricted) {
            pocsag_pager_scene_tx_confirm_show_result(
                app, "\e#Transmission blocked\e#\n\nFrequency is restricted\nby region settings");
            notification_message(app->notifications, &sequence_error);
        } else {
            pocsag_pager_scene_tx_confirm_show_result(
                app, "\e#Cannot transmit\e#\n\nCheck address, message,\nfrequency and mode");
            notification_message(app->notifications, &sequence_error);
        }
        return true;
    }
    if(event.type == SceneManagerEventTypeTick && pcsg_tx_complete(app)) {
        pcsg_tx_stop(app);
        pocsag_pager_scene_tx_confirm_show_result(
            app,
            app->tx_config.protocol == PagerTxProtocolFlex ?
                "\e#FLEX page sent\e#" :
                "\e#POCSAG page sent\e#");
        notification_message(app->notifications, &sequence_success);
        return true;
    }
    return false;
}

void pocsag_pager_scene_tx_confirm_on_exit(void* context) {
    POCSAGPagerApp* app = context;
    if(app->txrx->txrx_state == PCSGTxRxStateTx) pcsg_tx_stop(app);
    notification_message(app->notifications, &sequence_blink_stop);
    widget_reset(app->widget);
}
