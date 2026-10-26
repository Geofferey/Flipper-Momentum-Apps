#include "../pocsag_pager_app_i.h"

static void pocsag_pager_scene_tx_message_input_callback(void* context) {
    POCSAGPagerApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, PCSGCustomEventTxMessageInputDone);
}

void pocsag_pager_scene_tx_message_input_on_enter(void* context) {
    POCSAGPagerApp* app = context;
    text_input_set_header_text(
        app->text_input,
        app->tx_config.protocol == PagerTxProtocolFlex ? "FLEX message" : "POCSAG message");
    text_input_set_result_callback(
        app->text_input,
        pocsag_pager_scene_tx_message_input_callback,
        app,
        app->tx_config.message,
        sizeof(app->tx_config.message),
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewTextInput);
}

bool pocsag_pager_scene_tx_message_input_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == PCSGCustomEventTxMessageInputDone) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

void pocsag_pager_scene_tx_message_input_on_exit(void* context) {
    UNUSED(context);
}
