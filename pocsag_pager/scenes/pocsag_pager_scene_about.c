#include "../pocsag_pager_app_i.h"
#include "../helpers/pocsag_pager_types.h"

void pocsag_pager_scene_about_widget_callback(GuiButtonType result, InputType type, void* context) {
    POCSAGPagerApp* app = context;
    if(type == InputTypeShort) {
        view_dispatcher_send_custom_event(app->view_dispatcher, result);
    }
}

void pocsag_pager_scene_about_on_enter(void* context) {
    POCSAGPagerApp* app = context;

    FuriString* temp_str;
    temp_str = furi_string_alloc();
    furi_string_printf(temp_str, "\e#%s\n", "Information");

    furi_string_cat_printf(temp_str, "Version: %s\n", PCSG_VERSION_APP);
    furi_string_cat_printf(temp_str, "Developed by:\n%s\n\n", PCSG_DEVELOPED);
    furi_string_cat_printf(temp_str, "Github: %s\n\n", PCSG_GITHUB);

    furi_string_cat_printf(temp_str, "\e#%s\n", "Description");
    furi_string_cat_printf(
        temp_str,
        "Receiving POCSAG and FLEX\npager messages.\nDefault frequency is set to\nDAPNET - 439987500\n\nSelect FLEX for 2-level signals and a FLEX4 preset for 4-level. RSSI and hopping are disabled with FLEX4.\n\nFrequencies are in\n/pocsag/settings.txt\nCustom frequencies are at the end of the list.\n\n");

    furi_string_cat_printf(
        temp_str,
        "Supported protocols:\nPOCSAG 512\nPOCSAG 1200\nPOCSAG 2400\nFLEX 1600/2\nFLEX 3200/2\nFLEX 3200/4\nFLEX 6400/4\n\n");

    furi_string_cat_printf(temp_str, "\e#%s\n", "Disclaimer");
    furi_string_cat_printf(
        temp_str,
        "Only receive traffic you are allowed to. Messages and logs can hold private info: do not share them.\n929-932 MHz is outside the CC1101's rated range: use at your own risk.\n");

    widget_add_text_box_element(
        app->widget,
        0,
        0,
        128,
        14,
        AlignCenter,
        AlignBottom,
        "\e#\e!                                                      \e!\n",
        false);
    widget_add_text_box_element(
        app->widget,
        0,
        2,
        128,
        14,
        AlignCenter,
        AlignBottom,
        "\e#\e!        Flipper Pager      \e!\n",
        false);
    widget_add_text_scroll_element(app->widget, 0, 16, 128, 50, furi_string_get_cstr(temp_str));
    furi_string_free(temp_str);

    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewWidget);
}

bool pocsag_pager_scene_about_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    bool consumed = false;
    UNUSED(app);
    UNUSED(event);

    return consumed;
}

void pocsag_pager_scene_about_on_exit(void* context) {
    POCSAGPagerApp* app = context;

    // Clear views
    widget_reset(app->widget);
}
