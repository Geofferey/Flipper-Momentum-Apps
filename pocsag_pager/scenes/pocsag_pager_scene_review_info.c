#include "../pocsag_pager_app_i.h"

// One message from the log opened in Review Messages.

static void pocsag_pager_scene_review_info_callback(PCSGCustomEvent event, void* context) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

static void pocsag_pager_scene_review_info_show(POCSAGPagerApp* app) {
    PCSGHistoryEntry entry;
    if(pcsg_log_get_entry(app->log, app->review_idx, &entry)) {
        pcsg_view_receiver_info_update(app->pcsg_receiver_info, entry.ric, entry.message);
        pcsg_history_entry_free(&entry);
    }
}

void pocsag_pager_scene_review_info_on_enter(void* context) {
    POCSAGPagerApp* app = context;
    pcsg_view_receiver_info_set_callback(
        app->pcsg_receiver_info, pocsag_pager_scene_review_info_callback, app);
    pocsag_pager_scene_review_info_show(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewReceiverInfo);
}

bool pocsag_pager_scene_review_info_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event != PCSGCustomEventViewInfoNextMessage) return false;

    // The log is fixed while it is open, unlike live history.
    if(app->review_idx + 1U < pcsg_log_get_count(app->log)) {
        app->review_idx++;
        pocsag_pager_scene_review_info_show(app);
    }
    return true;
}

void pocsag_pager_scene_review_info_on_exit(void* context) {
    UNUSED(context);
}
