#include "../pocsag_pager_app_i.h"

static void pocsag_pager_scene_tx_ric_input_callback(void* context, int32_t number) {
    POCSAGPagerApp* app = context;
    app->tx_config.ric = number;
    view_dispatcher_send_custom_event(app->view_dispatcher, PCSGCustomEventTxRicInputDone);
}

void pocsag_pager_scene_tx_ric_input_on_enter(void* context) {
    POCSAGPagerApp* app = context;
    const bool flex = app->tx_config.protocol == PagerTxProtocolFlex;
    number_input_set_header_text(
        app->number_input, flex ? "FLEX short capcode" : "Destination RIC");
    number_input_set_result_callback(
        app->number_input,
        pocsag_pager_scene_tx_ric_input_callback,
        app,
        app->tx_config.ric,
        flex ? FLEX_TX_SHORT_CAPCODE_MIN : 0,
        flex ? FLEX_TX_SHORT_CAPCODE_MAX : PCSG_TX_RIC_MAX);
    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewNumberInput);
}

bool pocsag_pager_scene_tx_ric_input_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == PCSGCustomEventTxRicInputDone) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

void pocsag_pager_scene_tx_ric_input_on_exit(void* context) {
    UNUSED(context);
}
