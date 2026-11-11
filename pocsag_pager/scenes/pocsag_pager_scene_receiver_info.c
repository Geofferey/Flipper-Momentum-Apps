#include "../pocsag_pager_app_i.h"
#include "../views/pocsag_pager_receiver.h"
#include "../protocols/flex.h"
#include "../pocsag_pager_led.h"

void pocsag_pager_scene_receiver_info_callback(PCSGCustomEvent event, void* context) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

static void pocsag_pager_scene_receiver_info_add_to_history_callback(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    void* context) {
    furi_assert(context);
    POCSAGPagerApp* app = context;

    // Keep logging while a message is open. The list is rebuilt from history
    // on return, so only the selection needs to follow a dropped oldest entry.
    // Decoders reset themselves at the end of a transmission; see the
    // receiver scene.
    UNUSED(receiver);
    const bool display = pcsg_filter_accepts(&app->filter, decoder_base);
    const PCSGHistoryStateAddKey state = pcsg_history_add_to_history(
        app->txrx->history, decoder_base, app->txrx->preset, display);
    if(display) {
        const uint16_t evicted = pcsg_history_last_evicted(app->txrx->history);
        if(state == PCSGHistoryStateAddKeyNewDada || state == PCSGHistoryStateAddKeyOverflow ||
           evicted) {
            app->txrx->idx_menu_chosen = app->txrx->idx_menu_chosen > evicted ?
                                             app->txrx->idx_menu_chosen - evicted :
                                             0;
        }
        if(state == PCSGHistoryStateAddKeyNewDada) {
            if(app->filter.decode_led) {
                notification_message(
                    app->notifications, pcsg_led_blink_sequence(app->filter.decode_led_color));
            }
            pcsg_classic_alert_request(app);
            app->txrx->rx_key_state = PCSGRxKeyStateAddKey;
        }
    }
}

// Show the message at the current history index, keeping the list selection
// in step so returning to the receiver lands on whatever was last read.
static void pocsag_pager_scene_receiver_info_show(POCSAGPagerApp* app, bool at_end) {
    PCSGHistoryEntry entry;
    if(pcsg_history_get_entry(app->txrx->history, app->txrx->idx_menu_chosen, &entry)) {
        pcsg_view_receiver_info_update(app->pcsg_receiver_info, entry.ric, entry.message);
        if(at_end) pcsg_view_receiver_info_scroll_to_end(app->pcsg_receiver_info);
        pcsg_history_entry_free(&entry);
    }
}

/*
 * Find the nearest message sharing the open one's capcode or RIC. Each probe
 * reads the log, and copies to one address normally arrive close together, so
 * the search is bounded instead of walking a whole session: a match further
 * away than this is treated as none.
 */
#define PCSG_INFO_ADDRESS_SCAN 256U

static bool pocsag_pager_scene_receiver_info_find_address(
    POCSAGPagerApp* app,
    bool forward,
    uint16_t* found) {
    PCSGHistoryAddressType type;
    uint32_t address;
    if(!pcsg_history_get_address(
           app->txrx->history, app->txrx->idx_menu_chosen, &type, &address)) {
        return false;
    }

    const uint16_t count = pcsg_history_get_item(app->txrx->history);
    uint16_t idx = app->txrx->idx_menu_chosen;
    for(uint16_t step = 0; step < PCSG_INFO_ADDRESS_SCAN; step++) {
        if(forward) {
            if(idx + 1U >= count) return false;
            idx++;
        } else {
            if(idx == 0U) return false;
            idx--;
        }

        PCSGHistoryAddressType other_type;
        uint32_t other_address;
        if(pcsg_history_get_address(app->txrx->history, idx, &other_type, &other_address) &&
           other_type == type && other_address == address) {
            *found = idx;
            return true;
        }
    }
    return false;
}

void pocsag_pager_scene_receiver_info_on_enter(void* context) {
    POCSAGPagerApp* app = context;

    subghz_receiver_set_rx_callback(
        app->txrx->receiver, pocsag_pager_scene_receiver_info_add_to_history_callback, app);
    pcsg_view_receiver_info_set_callback(
        app->pcsg_receiver_info, pocsag_pager_scene_receiver_info_callback, app);
    pocsag_pager_scene_receiver_info_show(app, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewReceiverInfo);
}

bool pocsag_pager_scene_receiver_info_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == PCSGCustomEventViewInfoNextMessage) {
            // Messages received while reading extend the list, so the count is
            // read now rather than when the message was opened.
            if(app->txrx->idx_menu_chosen + 1U < pcsg_history_get_item(app->txrx->history)) {
                app->txrx->idx_menu_chosen++;
                pocsag_pager_scene_receiver_info_show(app, false);
            }
            consumed = true;
        } else if(event.event == PCSGCustomEventViewInfoPrevMessage) {
            if(app->txrx->idx_menu_chosen) {
                app->txrx->idx_menu_chosen--;
                pocsag_pager_scene_receiver_info_show(app, true);
            }
            consumed = true;
        } else if(
            event.event == PCSGCustomEventViewInfoNextAddress ||
            event.event == PCSGCustomEventViewInfoPrevAddress) {
            uint16_t found;
            if(pocsag_pager_scene_receiver_info_find_address(
                   app, event.event == PCSGCustomEventViewInfoNextAddress, &found)) {
                app->txrx->idx_menu_chosen = found;
                pocsag_pager_scene_receiver_info_show(app, false);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        pcsg_time_sync(app);
    }
    return consumed;
}

void pocsag_pager_scene_receiver_info_on_exit(void* context) {
    UNUSED(context);
}
