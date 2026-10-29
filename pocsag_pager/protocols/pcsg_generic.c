#include "pcsg_generic.h"
#include <lib/toolbox/stream/stream.h>
#include <lib/flipper_format/flipper_format_i.h>
#include "../helpers/pocsag_pager_types.h"

#define TAG "PCSGBlockGeneric"

void pcsg_block_generic_get_preset_name(const char* preset_name, FuriString* preset_str) {
    const char* preset_name_temp;
    if(!strcmp(preset_name, "AM270")) {
        preset_name_temp = "FuriHalSubGhzPresetOok270Async";
    } else if(!strcmp(preset_name, "AM650")) {
        preset_name_temp = "FuriHalSubGhzPresetOok650Async";
    } else if(!strcmp(preset_name, "FM238")) {
        preset_name_temp = "FuriHalSubGhzPreset2FSKDev238Async";
    } else if(!strcmp(preset_name, "FM12K")) {
        preset_name_temp = "FuriHalSubGhzPreset2FSKDev12KAsync";
    } else if(!strcmp(preset_name, "FM476")) {
        preset_name_temp = "FuriHalSubGhzPreset2FSKDev476Async";
    } else {
        preset_name_temp = "FuriHalSubGhzPresetCustom";
    }
    furi_string_set(preset_str, preset_name_temp);
}

void pcsg_block_generic_escape(FuriString* output, const char* input) {
    furi_string_reset(output);
    for(; *input; input++) {
        switch(*input) {
        case '\\':
            furi_string_cat_str(output, "\\\\");
            break;
        case '\n':
            furi_string_cat_str(output, "\\n");
            break;
        case '\r':
            furi_string_cat_str(output, "\\r");
            break;
        case '\t':
            furi_string_cat_str(output, "\\t");
            break;
        case '\e':
            furi_string_cat_str(output, "\\e");
            break;
        default:
            furi_string_push_back(output, *input);
            break;
        }
    }
}

void pcsg_block_generic_unescape(FuriString* output, const char* input) {
    furi_string_reset(output);
    for(; *input; input++) {
        if(*input != '\\' || !input[1]) {
            furi_string_push_back(output, *input);
            continue;
        }
        input++;
        switch(*input) {
        case 'n':
            furi_string_push_back(output, '\n');
            break;
        case 'r':
            furi_string_push_back(output, '\r');
            break;
        case 't':
            furi_string_push_back(output, '\t');
            break;
        case 'e':
            furi_string_push_back(output, '\e');
            break;
        default:
            furi_string_push_back(output, *input);
            break;
        }
    }
}

SubGhzProtocolStatus pcsg_block_generic_serialize(
    PCSGBlockGeneric* instance,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(instance);
    furi_assert(instance->serialize_scratch);
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    FuriString* temp_str = instance->serialize_scratch;
    do {
        if(furi_string_size(instance->result_ric) > PCSG_GENERIC_MAX_FIELD_LENGTH ||
           furi_string_size(instance->result_msg) > PCSG_GENERIC_MAX_FIELD_LENGTH) {
            FURI_LOG_E(TAG, "Message fields exceed size limit");
            break;
        }
        stream_clean(flipper_format_get_raw_stream(flipper_format));
        if(!flipper_format_write_header_cstr(
               flipper_format, PCSG_KEY_FILE_TYPE, PCSG_KEY_FILE_VERSION)) {
            FURI_LOG_E(TAG, "Unable to add header");
            break;
        }

        if(!flipper_format_write_uint32(flipper_format, "Frequency", &preset->frequency, 1)) {
            FURI_LOG_E(TAG, "Unable to add Frequency");
            break;
        }

        pcsg_block_generic_get_preset_name(furi_string_get_cstr(preset->name), temp_str);
        if(!flipper_format_write_string_cstr(
               flipper_format, "Preset", furi_string_get_cstr(temp_str))) {
            FURI_LOG_E(TAG, "Unable to add Preset");
            break;
        }
        if(!strcmp(furi_string_get_cstr(temp_str), "FuriHalSubGhzPresetCustom")) {
            if(!flipper_format_write_string_cstr(
                   flipper_format, "Custom_preset_module", "CC1101")) {
                FURI_LOG_E(TAG, "Unable to add Custom_preset_module");
                break;
            }
            if(!flipper_format_write_hex(
                   flipper_format, "Custom_preset_data", preset->data, preset->data_size)) {
                FURI_LOG_E(TAG, "Unable to add Custom_preset_data");
                break;
            }
        }
        if(!flipper_format_write_string_cstr(flipper_format, "Protocol", instance->protocol_name)) {
            FURI_LOG_E(TAG, "Unable to add Protocol");
            break;
        }

        // FlipperFormat strings end at a newline, and FLEX headers contain one.
        pcsg_block_generic_escape(temp_str, furi_string_get_cstr(instance->result_ric));
        if(!flipper_format_write_string(flipper_format, "Ric", temp_str)) {
            FURI_LOG_E(TAG, "Unable to add Ric");
            break;
        }

        pcsg_block_generic_escape(temp_str, furi_string_get_cstr(instance->result_msg));
        if(!flipper_format_write_string(flipper_format, "Message", temp_str)) {
            FURI_LOG_E(TAG, "Unable to add Message");
            break;
        }

        res = SubGhzProtocolStatusOk;
    } while(false);
    furi_string_reset(temp_str);
    return res;
}

SubGhzProtocolStatus
    pcsg_block_generic_deserialize(PCSGBlockGeneric* instance, FlipperFormat* flipper_format) {
    furi_assert(instance);
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    FuriString* temp_data = furi_string_alloc();
    FuriString* temp_data2 = furi_string_alloc();
    FuriString* raw = furi_string_alloc();

    do {
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }

        if(!flipper_format_read_string(flipper_format, "Ric", raw)) {
            FURI_LOG_E(TAG, "Missing Ric");
            break;
        }
        pcsg_block_generic_unescape(temp_data2, furi_string_get_cstr(raw));
        if(furi_string_size(temp_data2) > PCSG_GENERIC_MAX_FIELD_LENGTH) {
            FURI_LOG_E(TAG, "Ric is too large");
            break;
        }
        if(!flipper_format_read_string(flipper_format, "Message", raw)) {
            FURI_LOG_E(TAG, "Missing Message");
            break;
        }
        pcsg_block_generic_unescape(temp_data, furi_string_get_cstr(raw));
        if(furi_string_size(temp_data) > PCSG_GENERIC_MAX_FIELD_LENGTH) {
            FURI_LOG_E(TAG, "Message is too large");
            break;
        }

        if(instance->result_ric != NULL) {
            furi_string_set(instance->result_ric, temp_data2);
        } else {
            instance->result_ric = furi_string_alloc_set(temp_data2);
        }
        if(instance->result_msg != NULL) {
            furi_string_set(instance->result_msg, temp_data);
        } else {
            instance->result_msg = furi_string_alloc_set(temp_data);
        }

        res = SubGhzProtocolStatusOk;
    } while(0);

    furi_string_free(temp_data);
    furi_string_free(temp_data2);
    furi_string_free(raw);

    return res;
}
