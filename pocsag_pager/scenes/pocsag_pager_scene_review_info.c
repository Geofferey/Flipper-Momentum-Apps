#include "../pocsag_pager_app_i.h"

// One message from the log opened in Review Messages.

static void pocsag_pager_scene_review_info_callback(PCSGCustomEvent event, void* context) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

static void pocsag_pager_scene_review_info_show(POCSAGPagerApp* app, bool at_end) {
    PCSGHistoryEntry entry;
    if(pcsg_log_get_entry(app->log, app->review_idx, &entry)) {
        pcsg_view_receiver_info_update(app->pcsg_receiver_info, entry.ric, entry.message);
        if(at_end) pcsg_view_receiver_info_scroll_to_end(app->pcsg_receiver_info);
        pcsg_history_entry_free(&entry);
    }
}

/*
 * Bound for logs with no saved index: a session that kept one indexed every
 * message it displayed, and that search covers the whole log. Without one
 * each probe reads a header prefix, so the search is bounded instead; copies
 * to one address arrive close together, and a match further off than this is
 * treated as none.
 */
#define PCSG_REVIEW_ADDRESS_SCAN 256U

static bool pocsag_pager_scene_review_info_find_address(
    POCSAGPagerApp* app,
    bool forward,
    uint16_t* found) {
    PCSGHistoryAddressType type;
    uint32_t address;
    if(!pcsg_log_get_address(app->log, app->review_idx, &type, &address)) return false;

    if(pcsg_log_has_index(app->log)) {
        return pcsg_log_find_address(
            app->log, app->review_idx, forward, type, address, found);
    }

    const uint16_t count = pcsg_log_get_count(app->log);
    uint16_t idx = app->review_idx;
    for(uint16_t step = 0; step < PCSG_REVIEW_ADDRESS_SCAN; step++) {
        if(forward) {
            if(idx + 1U >= count) return false;
            idx++;
        } else {
            if(idx == 0U) return false;
            idx--;
        }

        PCSGHistoryAddressType other_type;
        uint32_t other_address;
        if(pcsg_log_get_address(app->log, idx, &other_type, &other_address) &&
           other_type == type && other_address == address) {
            *found = idx;
            return true;
        }
    }
    return false;
}

void pocsag_pager_scene_review_info_on_enter(void* context) {
    POCSAGPagerApp* app = context;
    pcsg_view_receiver_info_set_callback(
        app->pcsg_receiver_info, pocsag_pager_scene_review_info_callback, app);
    pocsag_pager_scene_review_info_show(app, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewReceiverInfo);
}

bool pocsag_pager_scene_review_info_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    // The log is fixed while it is open, unlike live history.
    if(event.event == PCSGCustomEventViewInfoNextMessage) {
        if(app->review_idx + 1U < pcsg_log_get_count(app->log)) {
            app->review_idx++;
            pocsag_pager_scene_review_info_show(app, false);
        }
        return true;
    }
    if(event.event == PCSGCustomEventViewInfoPrevMessage) {
        if(app->review_idx) {
            app->review_idx--;
            pocsag_pager_scene_review_info_show(app, true);
        }
        return true;
    }
    if(event.event == PCSGCustomEventViewInfoNextAddress ||
       event.event == PCSGCustomEventViewInfoPrevAddress) {
        uint16_t found;
        if(pocsag_pager_scene_review_info_find_address(
               app, event.event == PCSGCustomEventViewInfoNextAddress, &found)) {
            app->review_idx = found;
            pocsag_pager_scene_review_info_show(app, false);
        }
        return true;
    }
    return false;
}

void pocsag_pager_scene_review_info_on_exit(void* context) {
    UNUSED(context);
}
