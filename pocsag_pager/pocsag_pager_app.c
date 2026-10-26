#include "pocsag_pager_app_i.h"

#include <furi.h>
#include <furi_hal.h>
#include <lib/flipper_format/flipper_format.h>
#include "protocols/protocol_items.h"
#include "protocols/flex.h"

static bool pocsag_pager_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool pocsag_pager_app_back_event_callback(void* context) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void pocsag_pager_app_tick_event_callback(void* context) {
    furi_assert(context);
    POCSAGPagerApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

#define PCSG_FREQUENCY_FILE EXT_PATH("pocsag/settings.txt")

// Paging channels added to the standard Sub-GHz list.
// POCSAG: DAPNET 439.9875 MHz (the app's default, missing from the standard
// list) and the German 448.425, 465.970 and 466.075 MHz networks
// (https://en.wikipedia.org/wiki/POCSAG).
// US FLEX: nationwide channels from the RadioReference paging wiki
// (https://wiki.radioreference.com/index.php/Paging), more from the Signal
// Identification Wiki (https://sigidwiki.com/wiki/FLEX, listed to the kHz and
// mapped to the 12.5 kHz channel offsets), plus 929.4625 MHz, a local test
// paging channel. These are above the CC1101's 928 MHz rating; Sub-GHz
// settings skips them unless the extended frequency range is enabled.
static const uint32_t pcsg_paging_frequencies[] = {
    439987500,
    448425000,
    465970000,
    466075000,
    929362500,
    929387500,
    929462500,
    929537500,
    929587500,
    929612500,
    929662500,
    929937500,
    931062500,
    931212500,
    931337500,
    931862500,
    931937500,
};

/*
 * Create the frequency list on first run: the standard Sub-GHz frequencies
 * plus common POCSAG and FLEX paging channels. An existing file is left
 * alone, so a user's own list is never overwritten.
 */
static void pocsag_pager_app_create_frequency_file(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage_file_exists(storage, PCSG_FREQUENCY_FILE)) {
        storage_simply_mkdir(storage, EXT_PATH("pocsag"));
        FlipperFormat* file = flipper_format_file_alloc(storage);
        bool ok = flipper_format_file_open_new(file, PCSG_FREQUENCY_FILE) &&
                  flipper_format_write_header_cstr(
                      file, SUBGHZ_SETTING_FILE_TYPE, SUBGHZ_SETTING_FILE_VERSION) &&
                  flipper_format_write_comment_cstr(
                      file, "Created by Flipper Pager: standard frequencies plus POCSAG and") &&
                  flipper_format_write_comment_cstr(
                      file, "FLEX paging channels. Above 928 MHz needs the extended range enabled.");
        bool add_standard = true;
        ok = ok && flipper_format_write_bool(file, "Add_standard_frequencies", &add_standard, 1);
        for(size_t i = 0; ok && i < COUNT_OF(pcsg_paging_frequencies); i++) {
            ok = flipper_format_write_uint32(file, "Frequency", &pcsg_paging_frequencies[i], 1);
        }
        if(!ok) FURI_LOG_E("POCSAGPager", "Unable to create %s", PCSG_FREQUENCY_FILE);
        flipper_format_free(file);
    }
    furi_record_close(RECORD_STORAGE);
}

POCSAGPagerApp* pocsag_pager_app_alloc() {
    POCSAGPagerApp* app = calloc(1, sizeof(POCSAGPagerApp));

    // GUI
    app->gui = furi_record_open(RECORD_GUI);

    // View Dispatcher
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&pocsag_pager_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, pocsag_pager_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, pocsag_pager_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, pocsag_pager_app_tick_event_callback, 100);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Open Notification record
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    // Variable Item List
    app->variable_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        POCSAGPagerViewVariableItemList,
        variable_item_list_get_view(app->variable_item_list));

    // SubMenu
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, POCSAGPagerViewSubmenu, submenu_get_view(app->submenu));

    // Widget
    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, POCSAGPagerViewWidget, widget_get_view(app->widget));

    // Number Input
    app->number_input = number_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        POCSAGPagerViewNumberInput,
        number_input_get_view(app->number_input));

    // Text Input
    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        POCSAGPagerViewTextInput,
        text_input_get_view(app->text_input));

    // Receiver
    app->pcsg_receiver = pcsg_view_receiver_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        POCSAGPagerViewReceiver,
        pcsg_view_receiver_get_view(app->pcsg_receiver));

    // Receiver Info
    app->pcsg_receiver_info = pcsg_view_receiver_info_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        POCSAGPagerViewReceiverInfo,
        pcsg_view_receiver_info_get_view(app->pcsg_receiver_info));

    //init setting
    app->setting = subghz_setting_alloc();

    //ToDo FIX  file name setting

    pocsag_pager_app_create_frequency_file();
    subghz_setting_load(app->setting, PCSG_FREQUENCY_FILE);

    //init Worker & Protocol & History
    app->lock = PCSGLockOff;
    app->txrx = calloc(1, sizeof(POCSAGPagerTxRx));
    app->txrx->preset = calloc(1, sizeof(SubGhzRadioPreset));
    app->txrx->preset->name = furi_string_alloc();

    furi_hal_power_suppress_charge_enter();

    // Radio Devices init & load
    subghz_devices_init();
    app->txrx->radio_device =
        radio_device_loader_set(app->txrx->radio_device, SubGhzRadioDeviceTypeExternalCC1101);

    subghz_devices_reset(app->txrx->radio_device);
    subghz_devices_idle(app->txrx->radio_device);

    // Custom Presets load without using config file

    FlipperFormat* temp_fm_preset = flipper_format_string_alloc();
    flipper_format_write_string_cstr(
        temp_fm_preset,
        (const char*)"Custom_preset_data",
        (const char*)"02 0D 0B 06 08 32 07 04 14 00 13 02 12 04 11 83 10 67 15 24 18 18 19 16 1D 91 1C 00 1B 07 20 FB 22 10 21 56 00 00 C0 00 00 00 00 00 00 00");
    flipper_format_rewind(temp_fm_preset);
    subghz_setting_load_custom_preset(app->setting, (const char*)"FM95", temp_fm_preset);

    flipper_format_free(temp_fm_preset);

    // POCSAG: two-level FSK with about +/-4.5 kHz deviation. Unlike FM95, no
    // carrier-sense gating of the data output (MDMCFG2 0x00) and a 58 kHz
    // channel filter instead of 270 kHz (MDMCFG4 0xF6, which also sets the
    // data rate to 2400 baud, the fastest POCSAG rate). POCSAG-W widens the
    // filter to 116 kHz (MDMCFG4 0xB6) for the crystal offset at 900 MHz.
    FlipperFormat* temp_pocsag_preset = flipper_format_string_alloc();
    flipper_format_write_string_cstr(
        temp_pocsag_preset,
        (const char*)"Custom_preset_data",
        (const char*)"02 0D 0B 06 08 32 07 04 14 00 13 02 12 00 11 83 10 F6 15 1C 18 18 19 16 1D 91 1C 00 1B 07 20 FB 22 10 21 56 00 00 C0 00 00 00 00 00 00 00");
    flipper_format_rewind(temp_pocsag_preset);
    subghz_setting_load_custom_preset(app->setting, (const char*)"POCSAG", temp_pocsag_preset);
    flipper_format_free(temp_pocsag_preset);

    FlipperFormat* temp_pocsag_wide_preset = flipper_format_string_alloc();
    flipper_format_write_string_cstr(
        temp_pocsag_wide_preset,
        (const char*)"Custom_preset_data",
        (const char*)"02 0D 0B 06 08 32 07 04 14 00 13 02 12 00 11 83 10 B6 15 1C 18 18 19 16 1D 91 1C 00 1B 07 20 FB 22 10 21 56 00 00 C0 00 00 00 00 00 00 00");
    flipper_format_rewind(temp_pocsag_wide_preset);
    subghz_setting_load_custom_preset(
        app->setting, (const char*)"POCSAG-W", temp_pocsag_wide_preset);
    flipper_format_free(temp_pocsag_wide_preset);

    FlipperFormat* temp_flex_preset = flipper_format_string_alloc();
    flipper_format_write_string_cstr(
        temp_flex_preset,
        (const char*)"Custom_preset_data",
        (const char*)"02 0D 0B 06 08 32 07 04 14 00 13 02 12 00 11 83 10 F7 15 1C 18 18 19 16 1D 91 1C 00 1B 07 20 FB 22 10 21 56 00 00 C0 00 00 00 00 00 00 00");
    flipper_format_rewind(temp_flex_preset);
    subghz_setting_load_custom_preset(app->setting, (const char*)"FLEX", temp_flex_preset);
    flipper_format_free(temp_flex_preset);

    FlipperFormat* temp_flex4_preset = flipper_format_string_alloc();
    flipper_format_write_string_cstr(
        temp_flex4_preset,
        (const char*)"Custom_preset_data",
        (const char*)"02 16 01 17 0B 06 08 32 07 04 14 00 13 02 12 40 11 02 10 F7 15 1C 18 18 19 16 1D 91 1C 00 1B 07 20 FB 22 10 21 56 00 00 C0 00 00 00 00 00 00 00");
    flipper_format_rewind(temp_flex4_preset);
    subghz_setting_load_custom_preset(
        app->setting, (const char*)"FLEX4-3200", temp_flex4_preset);
    flipper_format_free(temp_flex4_preset);

    FlipperFormat* temp_flex4_6400_preset = flipper_format_string_alloc();
    flipper_format_write_string_cstr(
        temp_flex4_6400_preset,
        (const char*)"Custom_preset_data",
        (const char*)"02 16 01 17 0B 06 08 32 07 04 14 00 13 02 12 40 11 02 10 F8 15 1C 18 18 19 16 1D 91 1C 00 1B 07 20 FB 22 10 21 56 00 00 C0 00 00 00 00 00 00 00");
    flipper_format_rewind(temp_flex4_6400_preset);
    subghz_setting_load_custom_preset(
        app->setting, (const char*)"FLEX4-6400", temp_flex4_6400_preset);
    flipper_format_free(temp_flex4_6400_preset);

    // custom presets loading - end

    pcsg_preset_init(app, "POCSAG", 439987500, NULL, 0);

    app->txrx->hopper_state = PCSGHopperStateOFF;
    app->filter.type_filter = PCSGTypeFilterAll;
    app->filter.show_binary = true;
    app->filter.capcode_filter = PCSGCapcodeFilterOff;
    app->filter.hide_duplicates = true;
    // Received messages are third-party traffic: keep nothing on SD unless
    // the user chooses Save.
    app->filter.log_temp = true;
    app->filter.tone_volume = PCSGToneVolumeMedium;
    app->filter.tone_pitch = PCSGTonePitchMedium;
    app->filter.tone_spread = PCSGToneSpreadNormal;
    app->tx_config.protocol = PagerTxProtocolPocsag;
    app->tx_config.frequency = app->txrx->preset->frequency;
    app->tx_config.ric = 0;
    app->tx_config.function = 3;
    app->tx_config.baud = 1200;
    app->tx_config.inverted = false;
    app->tx_config.frequency_correction_hz = 0;
    pcsg_settings_load(app);

    app->tx_encoder = pocsag_tx_encoder_alloc();
    app->flex_tx_encoder = flex_tx_encoder_alloc();

    app->txrx->history = pcsg_history_alloc();
    app->log = pcsg_log_alloc();
    app->log_path = furi_string_alloc();
    app->txrx->worker = subghz_worker_alloc();
    app->txrx->environment = subghz_environment_alloc();
    subghz_environment_set_protocol_registry(
        app->txrx->environment, (void*)&pocsag_pager_protocol_registry);
    app->txrx->receiver = subghz_receiver_alloc_init(app->txrx->environment);
    SubGhzProtocolDecoderBase* flex_decoder = subghz_receiver_search_decoder_base_by_name(
        app->txrx->receiver, SUBGHZ_PROTOCOL_FLEX_NAME);
    furi_check(flex_decoder);
    app->txrx->flex_sampler = flex_sampler_alloc(
        flex_decoder,
        app->txrx->radio_device,
        radio_device_loader_is_external(app->txrx->radio_device));

    subghz_receiver_set_filter(app->txrx->receiver, SubGhzProtocolFlag_Decodable);
    subghz_worker_set_overrun_callback(
        app->txrx->worker, (SubGhzWorkerOverrunCallback)subghz_receiver_reset);
    subghz_worker_set_pair_callback(
        app->txrx->worker, (SubGhzWorkerPairCallback)subghz_receiver_decode);
    subghz_worker_set_context(app->txrx->worker, app->txrx->receiver);

    scene_manager_next_scene(app->scene_manager, POCSAGPagerSceneStart);

    return app;
}

void pocsag_pager_app_free(POCSAGPagerApp* app) {
    furi_assert(app);

    // Save both receiver and transmitter configuration on every clean exit.
    pcsg_settings_save(app);

    // Radio Devices sleep & off
    if(app->txrx->txrx_state == PCSGTxRxStateRx) pcsg_rx_end(app);
    if(app->txrx->txrx_state == PCSGTxRxStateTx) pcsg_tx_stop(app);
    if(app->txrx->txrx_state != PCSGTxRxStateSleep) pcsg_sleep(app);
    radio_device_loader_end(app->txrx->radio_device);

    subghz_devices_deinit();

    // Submenu
    view_dispatcher_remove_view(app->view_dispatcher, POCSAGPagerViewSubmenu);
    submenu_free(app->submenu);

    // Variable Item List
    view_dispatcher_remove_view(app->view_dispatcher, POCSAGPagerViewVariableItemList);
    variable_item_list_free(app->variable_item_list);

    //  Widget
    view_dispatcher_remove_view(app->view_dispatcher, POCSAGPagerViewWidget);
    widget_free(app->widget);

    // Number Input
    view_dispatcher_remove_view(app->view_dispatcher, POCSAGPagerViewNumberInput);
    number_input_free(app->number_input);

    // Text Input
    view_dispatcher_remove_view(app->view_dispatcher, POCSAGPagerViewTextInput);
    text_input_free(app->text_input);

    // Receiver
    view_dispatcher_remove_view(app->view_dispatcher, POCSAGPagerViewReceiver);
    pcsg_view_receiver_free(app->pcsg_receiver);

    // Receiver Info
    view_dispatcher_remove_view(app->view_dispatcher, POCSAGPagerViewReceiverInfo);
    pcsg_view_receiver_info_free(app->pcsg_receiver_info);

    //setting
    subghz_setting_free(app->setting);

    //Worker & Protocol & History
    flex_sampler_free(app->txrx->flex_sampler);
    subghz_receiver_free(app->txrx->receiver);
    subghz_environment_free(app->txrx->environment);
    pcsg_history_free(app->txrx->history);
    pcsg_log_free(app->log);
    furi_string_free(app->log_path);
    subghz_worker_free(app->txrx->worker);
    pocsag_tx_encoder_free(app->tx_encoder);
    flex_tx_encoder_free(app->flex_tx_encoder);
    furi_string_free(app->txrx->preset->name);
    free(app->txrx->preset);
    free(app->txrx);

    // View dispatcher
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    // Notifications
    furi_record_close(RECORD_NOTIFICATION);
    app->notifications = NULL;

    // Close records
    furi_record_close(RECORD_GUI);

    furi_hal_power_suppress_charge_exit();

    free(app);
}

int32_t pocsag_pager_app(void* p) {
    UNUSED(p);
    POCSAGPagerApp* pocsag_pager_app = pocsag_pager_app_alloc();

    view_dispatcher_run(pocsag_pager_app->view_dispatcher);

    pocsag_pager_app_free(pocsag_pager_app);

    return 0;
}
