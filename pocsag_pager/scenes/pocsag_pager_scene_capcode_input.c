#include "../pocsag_pager_app_i.h"

// Scene state selects the field being edited: 0 for the capcode (or range
// minimum), 1 for the range maximum.

static void pocsag_pager_scene_capcode_input_callback(void* context, int32_t number) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    if(scene_manager_get_scene_state(app->scene_manager, POCSAGPagerSceneCapcodeInput)) {
        app->filter.capcode_max = number;
    } else {
        app->filter.capcode_min = number;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, PCSGCustomEventSceneCapcodeInputDone);
}

void pocsag_pager_scene_capcode_input_on_enter(void* context) {
    POCSAGPagerApp* app = context;
    const bool edit_max =
        scene_manager_get_scene_state(app->scene_manager, POCSAGPagerSceneCapcodeInput);
    const char* header = "Capcode";
    if(edit_max) {
        header = "Capcode max";
    } else if(app->filter.capcode_filter == PCSGCapcodeFilterRange) {
        header = "Capcode min";
    }

    number_input_set_header_text(app->number_input, header);
    number_input_set_result_callback(
        app->number_input,
        pocsag_pager_scene_capcode_input_callback,
        app,
        edit_max ? app->filter.capcode_max : app->filter.capcode_min,
        0,
        PCSG_CAPCODE_MAX);
    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewNumberInput);
}

bool pocsag_pager_scene_capcode_input_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == PCSGCustomEventSceneCapcodeInputDone) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

void pocsag_pager_scene_capcode_input_on_exit(void* context) {
    UNUSED(context);
}
