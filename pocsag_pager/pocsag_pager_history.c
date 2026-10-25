#include "pocsag_pager_history.h"
#include <flipper_format/flipper_format_i.h>
#include <lib/toolbox/stream/stream.h>
#include <lib/subghz/receiver.h>
#include "protocols/pcsg_generic.h"

#include <furi.h>

#define PCSG_HISTORY_MAX 50
#define TAG "PCSGHistory"

typedef struct {
    FuriString* item_str;
    FlipperFormat* flipper_string;
    uint8_t type;
    SubGhzRadioPreset* preset;
} PCSGHistoryItem;

ARRAY_DEF(PCSGHistoryItemArray, PCSGHistoryItem, M_POD_OPLIST)

#define M_OPL_PCSGHistoryItemArray_t() ARRAY_OPLIST(PCSGHistoryItemArray, M_POD_OPLIST)

typedef struct {
    PCSGHistoryItemArray_t data;
} PCSGHistoryStruct;

struct PCSGHistory {
    uint32_t last_update_timestamp;
    uint16_t last_index_write;
    uint8_t code_last_hash_data;
    bool has_last_hash;
    FuriString* tmp_string;
    PCSGHistoryStruct* history;
};

static void pcsg_history_item_clear(PCSGHistoryItem* item) {
    if(item->item_str) furi_string_free(item->item_str);
    if(item->preset) {
        if(item->preset->name) furi_string_free(item->preset->name);
        free(item->preset);
    }
    if(item->flipper_string) flipper_format_free(item->flipper_string);
    memset(item, 0, sizeof(PCSGHistoryItem));
}

PCSGHistory* pcsg_history_alloc(void) {
    PCSGHistory* instance = calloc(1, sizeof(PCSGHistory));
    instance->tmp_string = furi_string_alloc();
    instance->history = calloc(1, sizeof(PCSGHistoryStruct));
    PCSGHistoryItemArray_init(instance->history->data);
    return instance;
}

void pcsg_history_free(PCSGHistory* instance) {
    furi_assert(instance);
    furi_string_free(instance->tmp_string);
    for
        M_EACH(item, instance->history->data, PCSGHistoryItemArray_t) {
            pcsg_history_item_clear(item);
        }
    PCSGHistoryItemArray_clear(instance->history->data);
    free(instance->history);
    free(instance);
}

uint32_t pcsg_history_get_frequency(PCSGHistory* instance, uint16_t idx) {
    furi_assert(instance);
    PCSGHistoryItem* item = PCSGHistoryItemArray_get(instance->history->data, idx);
    return item->preset->frequency;
}

SubGhzRadioPreset* pcsg_history_get_radio_preset(PCSGHistory* instance, uint16_t idx) {
    furi_assert(instance);
    PCSGHistoryItem* item = PCSGHistoryItemArray_get(instance->history->data, idx);
    return item->preset;
}

const char* pcsg_history_get_preset(PCSGHistory* instance, uint16_t idx) {
    furi_assert(instance);
    PCSGHistoryItem* item = PCSGHistoryItemArray_get(instance->history->data, idx);
    return furi_string_get_cstr(item->preset->name);
}

void pcsg_history_reset(PCSGHistory* instance) {
    furi_assert(instance);
    furi_string_reset(instance->tmp_string);
    for
        M_EACH(item, instance->history->data, PCSGHistoryItemArray_t) {
            pcsg_history_item_clear(item);
        }
    PCSGHistoryItemArray_reset(instance->history->data);
    instance->last_index_write = 0;
    instance->code_last_hash_data = 0;
    instance->has_last_hash = false;
}

uint16_t pcsg_history_get_item(PCSGHistory* instance) {
    furi_assert(instance);
    return instance->last_index_write;
}

uint8_t pcsg_history_get_type_protocol(PCSGHistory* instance, uint16_t idx) {
    furi_assert(instance);
    PCSGHistoryItem* item = PCSGHistoryItemArray_get(instance->history->data, idx);
    return item->type;
}

const char* pcsg_history_get_protocol_name(PCSGHistory* instance, uint16_t idx) {
    furi_assert(instance);
    PCSGHistoryItem* item = PCSGHistoryItemArray_get(instance->history->data, idx);
    flipper_format_rewind(item->flipper_string);
    if(!flipper_format_read_string(item->flipper_string, "Protocol", instance->tmp_string)) {
        FURI_LOG_E(TAG, "Missing Protocol");
        furi_string_reset(instance->tmp_string);
    }
    return furi_string_get_cstr(instance->tmp_string);
}

FlipperFormat* pcsg_history_get_raw_data(PCSGHistory* instance, uint16_t idx) {
    furi_assert(instance);
    PCSGHistoryItem* item = PCSGHistoryItemArray_get(instance->history->data, idx);
    if(item->flipper_string) {
        return item->flipper_string;
    } else {
        return NULL;
    }
}
bool pcsg_history_get_text_space_left(PCSGHistory* instance, FuriString* output) {
    furi_assert(instance);
    if(instance->last_index_write >= PCSG_HISTORY_MAX) {
        if(output != NULL) furi_string_printf(output, "Memory is FULL");
        return true;
    }
    if(output != NULL)
        furi_string_printf(output, "%02u/%02u", instance->last_index_write, PCSG_HISTORY_MAX);
    return false;
}

void pcsg_history_get_text_item_menu(PCSGHistory* instance, FuriString* output, uint16_t idx) {
    PCSGHistoryItem* item = PCSGHistoryItemArray_get(instance->history->data, idx);
    furi_string_set(output, item->item_str);
}

PCSGHistoryStateAddKey
    pcsg_history_add_to_history(PCSGHistory* instance, void* context, SubGhzRadioPreset* preset) {
    furi_assert(instance);
    furi_assert(context);

    if(instance->last_index_write >= PCSG_HISTORY_MAX) return PCSGHistoryStateAddKeyOverflow;

    SubGhzProtocolDecoderBase* decoder_base = context;
    const uint8_t hash = subghz_protocol_decoder_base_get_hash_data(decoder_base);
    if(instance->has_last_hash && (instance->code_last_hash_data == hash) &&
       ((furi_get_tick() - instance->last_update_timestamp) < 500)) {
        instance->last_update_timestamp = furi_get_tick();
        return PCSGHistoryStateAddKeyTimeOut;
    }

    PCSGHistoryItem pending = {0};
    FuriString* temp_ric = furi_string_alloc();
    FuriString* temp_message = furi_string_alloc();
    bool valid = false;

    pending.preset = calloc(1, sizeof(SubGhzRadioPreset));
    pending.type = decoder_base->protocol->type;
    pending.preset->frequency = preset->frequency;
    pending.preset->name = furi_string_alloc_set(preset->name);
    pending.preset->data = preset->data;
    pending.preset->data_size = preset->data_size;
    pending.item_str = furi_string_alloc();
    pending.flipper_string = flipper_format_string_alloc();

    do {
        if(subghz_protocol_decoder_base_serialize(
               decoder_base, pending.flipper_string, pending.preset) !=
           SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Serialize error");
            break;
        }
        if(!flipper_format_rewind(pending.flipper_string)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        if(!flipper_format_read_string(
               pending.flipper_string, "Protocol", instance->tmp_string)) {
            FURI_LOG_E(TAG, "Missing Protocol");
            break;
        }

        if(!flipper_format_rewind(pending.flipper_string)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        if(!flipper_format_read_string(pending.flipper_string, "Ric", temp_ric)) {
            FURI_LOG_E(TAG, "Missing Ric");
            break;
        }

        if(!flipper_format_read_string(pending.flipper_string, "Message", temp_message)) {
            FURI_LOG_E(TAG, "Missing Message");
            break;
        }

        furi_string_printf(
            pending.item_str,
            "%s%s",
            furi_string_get_cstr(temp_ric),
            furi_string_get_cstr(temp_message));
        valid = true;
    } while(false);

    furi_string_free(temp_message);
    furi_string_free(temp_ric);
    if(!valid) {
        pcsg_history_item_clear(&pending);
        return PCSGHistoryStateAddKeyUnknown;
    }

    PCSGHistoryItem* item = PCSGHistoryItemArray_push_raw(instance->history->data);
    *item = pending;
    instance->code_last_hash_data = hash;
    instance->has_last_hash = true;
    instance->last_update_timestamp = furi_get_tick();
    instance->last_index_write++;
    return PCSGHistoryStateAddKeyNewDada;
}
