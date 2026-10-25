#pragma once

#include <gui/view.h>
#include "../helpers/pocsag_pager_types.h"
#include "../helpers/pocsag_pager_event.h"

typedef struct PCSGReceiver PCSGReceiver;

typedef void (*PCSGReceiverCallback)(PCSGCustomEvent event, void* context);

void pcsg_receiver_rssi(PCSGReceiver* instance, float rssi);

void pcsg_view_receiver_set_lock(PCSGReceiver* pcsg_receiver, PCSGLock keyboard);

void pcsg_view_receiver_set_ext_module_state(PCSGReceiver* pcsg_receiver, bool is_external);

void pcsg_view_receiver_set_callback(
    PCSGReceiver* pcsg_receiver,
    PCSGReceiverCallback callback,
    void* context);

PCSGReceiver* pcsg_view_receiver_alloc();

void pcsg_view_receiver_free(PCSGReceiver* pcsg_receiver);

View* pcsg_view_receiver_get_view(PCSGReceiver* pcsg_receiver);

void pcsg_view_receiver_add_data_statusbar(
    PCSGReceiver* pcsg_receiver,
    const char* frequency_str,
    const char* preset_str,
    const char* history_stat_str);

/** Fills a list row's label and icon type; the view keeps no copies. */
typedef void (*PCSGReceiverItemCallback)(
    void* context,
    uint16_t idx,
    FuriString* label,
    uint8_t* type);

void pcsg_view_receiver_set_item_callback(
    PCSGReceiver* pcsg_receiver,
    PCSGReceiverItemCallback callback,
    void* context);

/** Set the number of list rows, e.g. when re-entering the receiver. */
void pcsg_view_receiver_set_item_count(PCSGReceiver* pcsg_receiver, uint16_t count);

/**
 * Account for one new row at the end of the list. With dropped_oldest the
 * first row went away to make room, so the count stays the same.
 */
void pcsg_view_receiver_add_item(PCSGReceiver* pcsg_receiver, bool dropped_oldest);

uint16_t pcsg_view_receiver_get_idx_menu(PCSGReceiver* pcsg_receiver);

void pcsg_view_receiver_set_idx_menu(PCSGReceiver* pcsg_receiver, uint16_t idx);

void pcsg_view_receiver_exit(void* context);
