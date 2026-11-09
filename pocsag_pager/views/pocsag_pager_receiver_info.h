#pragma once

#include <gui/view.h>
#include "../helpers/pocsag_pager_types.h"
#include "../helpers/pocsag_pager_event.h"

typedef struct PCSGReceiverInfo PCSGReceiverInfo;

typedef void (*PCSGReceiverInfoCallback)(PCSGCustomEvent event, void* context);

/**
 * Called when the reader asks for a message the view cannot supply itself.
 * The view holds no list, so the owning scene decides what comes next.
 */
void pcsg_view_receiver_info_set_callback(
    PCSGReceiverInfo* pcsg_receiver_info,
    PCSGReceiverInfoCallback callback,
    void* context);

void pcsg_view_receiver_info_update(
    PCSGReceiverInfo* pcsg_receiver_info,
    const char* ric,
    const char* message);

/**
 * Open the message at its end rather than its start, for one reached by
 * scrolling backwards. Call after pcsg_view_receiver_info_update(), which
 * resets the position; it takes effect on the next draw, once wrapping is
 * known.
 */
void pcsg_view_receiver_info_scroll_to_end(PCSGReceiverInfo* pcsg_receiver_info);

PCSGReceiverInfo* pcsg_view_receiver_info_alloc();

void pcsg_view_receiver_info_free(PCSGReceiverInfo* pcsg_receiver_info);

View* pcsg_view_receiver_info_get_view(PCSGReceiverInfo* pcsg_receiver_info);
