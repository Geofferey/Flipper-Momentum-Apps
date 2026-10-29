#pragma once

#include "helpers/pocsag_pager_types.h"
#include "helpers/radio_device_loader.h"

#include "scenes/pocsag_pager_scene.h"
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/modules/number_input.h>
#include <gui/modules/text_input.h>
#include <notification/notification_messages.h>
#include "views/pocsag_pager_receiver.h"
#include "views/pocsag_pager_receiver_info.h"
#include "pocsag_pager_history.h"
#include "flex_sampler.h"
#include "pocsag_pager_settings.h"
#include "pocsag_pager_log.h"
#include "pocsag_tx.h"
#include "flex_tx.h"

#include <lib/subghz/subghz_setting.h>
#include <lib/subghz/subghz_worker.h>
#include <lib/subghz/receiver.h>
#include <lib/subghz/transmitter.h>
#include <lib/subghz/registry.h>
#include <lib/subghz/devices/devices.h>

typedef struct POCSAGPagerApp POCSAGPagerApp;

struct POCSAGPagerTxRx {
    SubGhzWorker* worker;

    SubGhzEnvironment* environment;
    SubGhzReceiver* receiver;
    SubGhzRadioPreset* preset;
    PCSGHistory* history;
    FlexSampler* flex_sampler;
    uint16_t idx_menu_chosen;
    PCSGTxRxState txrx_state;
    PCSGHopperState hopper_state;
    uint8_t hopper_timeout;
    uint8_t hopper_idx_frequency;
    PCSGRxKeyState rx_key_state;
    // Set when Config changes frequency, preset or hopping; otherwise the
    // receiver keeps running when it is re-entered.
    bool rx_restart;
    // The speaker is held for the signal sound while receiving.
    bool speaker_on;

    const SubGhzDevice* radio_device;
};

typedef struct POCSAGPagerTxRx POCSAGPagerTxRx;

struct POCSAGPagerApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    POCSAGPagerTxRx* txrx;
    SceneManager* scene_manager;
    NotificationApp* notifications;
    VariableItemList* variable_item_list;
    Submenu* submenu;
    Widget* widget;
    NumberInput* number_input;
    TextInput* text_input;
    PCSGReceiver* pcsg_receiver;
    PCSGReceiverInfo* pcsg_receiver_info;
    PCSGLock lock;
    SubGhzSetting* setting;
    PCSGFilterSettings filter;
    uint8_t config_selected_index;
    // Config opened from the main menu rather than the receiver.
    bool config_standalone;

    PocsagTxEncoder* tx_encoder;
    FlexTxEncoder* flex_tx_encoder;
    PocsagTxConfig tx_config;
    uint8_t tx_config_selected_index;

    // RX frequency auto-calibration accumulation.
    int32_t freq_cal_sum_hz;
    uint16_t freq_cal_count;
    uint32_t freq_cal_tick;
    // Last FLEX4 calibration pause, which is rarer than the apply interval.
    uint32_t freq_cal_blip_tick;

    // Review Messages
    PCSGLog* log;
    FuriString* log_path;
    uint16_t review_idx;
};

void pcsg_preset_init(
    void* context,
    const char* preset_name,
    uint32_t frequency,
    uint8_t* preset_data,
    size_t preset_data_size);
void pcsg_get_frequency_modulation(
    POCSAGPagerApp* app,
    FuriString* frequency,
    FuriString* modulation);
bool pcsg_preset_is_flex4(const char* preset_name);
void pcsg_begin(POCSAGPagerApp* app, uint8_t* preset_data);
uint32_t pcsg_rx(POCSAGPagerApp* app, uint32_t frequency);
void pcsg_idle(POCSAGPagerApp* app);
void pcsg_rx_end(POCSAGPagerApp* app);
void pcsg_sleep(POCSAGPagerApp* app);
void pcsg_hopper_update(POCSAGPagerApp* app);
int32_t pcsg_freqest_read_hz(POCSAGPagerApp* app);
void pcsg_retune(POCSAGPagerApp* app);
void pcsg_freq_auto_tick(POCSAGPagerApp* app, float rssi);

typedef enum {
    PCSGTxStartOk,
    PCSGTxStartInvalid,
    PCSGTxStartRestricted,
} PCSGTxStartStatus;

PCSGTxStartStatus pcsg_tx_start(POCSAGPagerApp* app);
bool pcsg_tx_complete(POCSAGPagerApp* app);
void pcsg_tx_stop(POCSAGPagerApp* app);
