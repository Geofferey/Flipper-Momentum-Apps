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
    case PCSGNumberFieldRicMin:
        return &app->filter.ric_min;
    case PCSGNumberFieldRicMax:
        return &app->filter.ric_max;
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
    const bool ric = field == PCSGNumberFieldRicMin || field == PCSGNumberFieldRicMax;
    const PCSGCapcodeFilter mode = frame ? app->filter.frame_filter :
                                   ric   ? app->filter.ric_filter :
                                           app->filter.capcode_filter;
    const bool range = mode == PCSGCapcodeFilterRange;

    const char* header;
    uint32_t max = PCSG_CAPCODE_MAX;
    switch(field) {
    case PCSGNumberFieldCapcodeMax:
        header = "Capcode max";
        break;
    case PCSGNumberFieldFrameMin:
        header = range ? "Frame min (0-127)" : "Frame (0-127)";
        max = PCSG_FRAME_MAX;
        break;
    case PCSGNumberFieldFrameMax:
        header = "Frame max (0-127)";
        max = PCSG_FRAME_MAX;
        break;
    case PCSGNumberFieldRicMin:
        header = range ? "RIC min" : "RIC";
        max = PCSG_RIC_MAX;
        break;
    case PCSGNumberFieldRicMax:
        header = "RIC max";
        max = PCSG_RIC_MAX;
        break;
    default:
        header = range ? "Capcode min" : "Capcode";
        break;
    }

    number_input_set_header_text(app->number_input, header);
    number_input_set_result_callback(
        app->number_input,
        pocsag_pager_scene_capcode_input_callback,
        app,
        *pocsag_pager_scene_capcode_input_field(app),
        0,
        max);
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
