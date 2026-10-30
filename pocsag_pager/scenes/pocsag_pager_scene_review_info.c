#include "../pocsag_pager_app_i.h"

// One message from the log opened in Review Messages.

void pocsag_pager_scene_review_info_on_enter(void* context) {
    POCSAGPagerApp* app = context;
    PCSGHistoryEntry entry;
    if(pcsg_log_get_entry(app->log, app->review_idx, &entry)) {
        pcsg_view_receiver_info_update(app->pcsg_receiver_info, entry.ric, entry.message);
        pcsg_history_entry_free(&entry);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewReceiverInfo);
}

bool pocsag_pager_scene_review_info_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void pocsag_pager_scene_review_info_on_exit(void* context) {
    UNUSED(context);
}
