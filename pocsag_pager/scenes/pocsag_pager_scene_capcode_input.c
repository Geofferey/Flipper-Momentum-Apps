#include "../pocsag_pager_app_i.h"

// Number entry for the capcode and frame filters. The scene state selects
// the field being edited.

static uint32_t* pocsag_pager_scene_capcode_input_field(POCSAGPagerApp* app) {
    switch(scene_manager_get_scene_state(app->scene_manager, POCSAGPagerSceneCapcodeInput)) {
    case PCSGNumberFieldCapcodeMax:
        return &app->filter.capcode_max;
    case PCSGNumberFieldFrameMin:
        return &app->filter.frame_min;
    case PCSGNumberFieldFrameMax:
        return &app->filter.frame_max;
    default:
        return &app->filter.capcode_min;
    }
}

static void pocsag_pager_scene_capcode_input_callback(void* context, int32_t number) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    *pocsag_pager_scene_capcode_input_field(app) = number;
    view_dispatcher_send_custom_event(app->view_dispatcher, PCSGCustomEventSceneCapcodeInputDone);
}

void pocsag_pager_scene_capcode_input_on_enter(void* context) {
    POCSAGPagerApp* app = context;
    const uint32_t field =
        scene_manager_get_scene_state(app->scene_manager, POCSAGPagerSceneCapcodeInput);
    const bool frame = field == PCSGNumberFieldFrameMin || field == PCSGNumberFieldFrameMax;
    const PCSGCapcodeFilter mode = frame ? app->filter.frame_filter : app->filter.capcode_filter;

    const char* header;
    if(field == PCSGNumberFieldCapcodeMax) {
        header = "Capcode max";
    } else if(field == PCSGNumberFieldFrameMax) {
        header = "Frame max (0-127)";
    } else if(frame) {
        header = mode == PCSGCapcodeFilterRange ? "Frame min (0-127)" : "Frame (0-127)";
    } else {
        header = mode == PCSGCapcodeFilterRange ? "Capcode min" : "Capcode";
    }

    number_input_set_header_text(app->number_input, header);
    number_input_set_result_callback(
        app->number_input,
        pocsag_pager_scene_capcode_input_callback,
        app,
        *pocsag_pager_scene_capcode_input_field(app),
        0,
        frame ? PCSG_FRAME_MAX : PCSG_CAPCODE_MAX);
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
