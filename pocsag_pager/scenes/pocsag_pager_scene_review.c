#include "../pocsag_pager_app_i.h"
#include "../views/pocsag_pager_receiver.h"

#include <dialogs/dialogs.h>
#include <storage/storage.h>

// Review Messages: pick a saved session log, then browse it in the receiver
// list. The log stays open while a message is shown and closes on Back.

static void pocsag_pager_scene_review_free(POCSAGPagerApp* app) {
    if(app->log) {
        pcsg_log_free(app->log);
        app->log = NULL;
    }
    if(app->log_path) {
        furi_string_free(app->log_path);
        app->log_path = NULL;
    }
}

static void pocsag_pager_scene_review_callback(PCSGCustomEvent event, void* context) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

static void pocsag_pager_scene_review_item_callback(
    void* context,
    uint16_t idx,
    FuriString* label,
    uint8_t* type) {
    POCSAGPagerApp* app = context;
    pcsg_log_get_label(app->log, idx, label);
    *type = SubGhzProtocolTypeStatic;
}

static bool pocsag_pager_scene_review_pick_log(POCSAGPagerApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("pocsag"));
    storage_simply_mkdir(storage, PCSG_HISTORY_LOG_DIR);
    furi_record_close(RECORD_STORAGE);

    if(furi_string_empty(app->log_path)) furi_string_set_str(app->log_path, PCSG_HISTORY_LOG_DIR);

    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, PCSG_HISTORY_LOG_EXT, NULL);
    options.base_path = PCSG_HISTORY_LOG_DIR;
    options.hide_ext = false;

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    const bool picked =
        dialog_file_browser_show(dialogs, app->log_path, app->log_path, &options);
    furi_record_close(RECORD_DIALOGS);

    return picked && pcsg_log_open(app->log, furi_string_get_cstr(app->log_path));
}

// Status bar: session start time from the file name (YYYY-MM-DD_HH-MM-SS.txt)
// and the message count. The date is already shown by the file browser, and
// the bar has no room for both.
static void pocsag_pager_scene_review_update_statusbar(POCSAGPagerApp* app) {
    const char* name = strrchr(furi_string_get_cstr(app->log_path), '/');
    name = name ? name + 1 : furi_string_get_cstr(app->log_path);

    char time[6] = {0};
    if(strlen(name) >= 16 && name[10] == '_') {
        snprintf(time, sizeof(time), "%.2s:%.2s", name + 11, name + 14);
    } else {
        snprintf(time, sizeof(time), "%.5s", name);
    }

    char count[8];
    snprintf(
        count,
        sizeof(count),
        "%u%s",
        pcsg_log_get_count(app->log),
        pcsg_log_is_truncated(app->log) ? "+" : "");
    pcsg_view_receiver_add_data_statusbar(app->pcsg_receiver, time, "", count);
}

void pocsag_pager_scene_review_on_enter(void* context) {
    POCSAGPagerApp* app = context;

    if(!app->log) {
        app->log = pcsg_log_alloc();
        app->log_path = furi_string_alloc();
    }

    if(!pcsg_log_is_open(app->log)) {
        if(!pocsag_pager_scene_review_pick_log(app)) {
            pocsag_pager_scene_review_free(app);
            scene_manager_previous_scene(app->scene_manager);
            return;
        }
        app->review_idx = 0;
    }

    pcsg_view_receiver_exit(app->pcsg_receiver);
    pcsg_view_receiver_set_review_mode(app->pcsg_receiver, true);
    pcsg_view_receiver_set_lock(app->pcsg_receiver, PCSGLockOff);
    pcsg_view_receiver_set_item_callback(
        app->pcsg_receiver, pocsag_pager_scene_review_item_callback, app);
    pcsg_view_receiver_set_item_count(app->pcsg_receiver, pcsg_log_get_count(app->log));
    pocsag_pager_scene_review_update_statusbar(app);
    pcsg_view_receiver_set_callback(app->pcsg_receiver, pocsag_pager_scene_review_callback, app);
    pcsg_view_receiver_set_idx_menu(app->pcsg_receiver, app->review_idx);
    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewReceiver);
}

bool pocsag_pager_scene_review_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == PCSGCustomEventViewReceiverOK) {
        app->review_idx = pcsg_view_receiver_get_idx_menu(app->pcsg_receiver);
        scene_manager_next_scene(app->scene_manager, POCSAGPagerSceneReviewInfo);
        return true;
    } else if(event.event == PCSGCustomEventViewReceiverBack) {
        pcsg_view_receiver_set_item_callback(app->pcsg_receiver, NULL, NULL);
        pocsag_pager_scene_review_free(app);
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

void pocsag_pager_scene_review_on_exit(void* context) {
    UNUSED(context);
}
