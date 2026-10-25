/*
 * FLEX pager decoder for the Flipper Zero asynchronous CC1101 input.
 *
 * Framing and page parsing are adapted from multimon-ng's demod_flex.c,
 * Copyright 2004, 2006, 2010 Free Software Foundation, Inc. and contributors.
 * multimon-ng is licensed under GNU GPL version 3 or later.
 */

#include "flex.h"

#include "flex_bch.h"
#include "pcsg_generic.h"

#include <inttypes.h>
#include <string.h>
#include <lib/flipper_format/flipper_format_i.h>

#define TAG "FLEX"

#define FLEX_SYNC_MARKER 0xA6C6AAAAUL
#define FLEX_PHASE_WORDS 88U
#define FLEX_SYNC_BAUD 1600U
#define FLEX_SYNC2_DURATION_MS 25U
#define FLEX_DATA_DURATION_MS 1760U
#define FLEX_MAX_RUN_SYMBOLS 512U

typedef enum {
    FlexDecoderStateSync1,
    FlexDecoderStateFiw,
    FlexDecoderStateSync2,
    FlexDecoderStateData,
} FlexDecoderState;

typedef enum {
    FlexPageTypeSecure,
    FlexPageTypeShortInstruction,
    FlexPageTypeTone,
    FlexPageTypeStandardNumeric,
    FlexPageTypeSpecialNumeric,
    FlexPageTypeAlphanumeric,
    FlexPageTypeBinary,
    FlexPageTypeNumberedNumeric,
} FlexPageType;

typedef struct {
    uint16_t sync;
    uint16_t baud;
    uint8_t levels;
} FlexMode;

static const FlexMode flex_modes[] = {
    {.sync = 0x870C, .baud = 1600, .levels = 2},
    {.sync = 0xB068, .baud = 1600, .levels = 4},
    {.sync = 0x7B18, .baud = 3200, .levels = 2},
    {.sync = 0xDEA0, .baud = 3200, .levels = 4},
    {.sync = 0x4C7C, .baud = 3200, .levels = 4},
};

struct SubGhzProtocolDecoderFlex {
    SubGhzProtocolDecoderBase base;
    PCSGBlockGeneric generic;

    FlexDecoderState state;
    uint64_t sync_buffer;
    bool inverted;
    uint16_t baud;
    uint8_t levels;

    uint8_t fiw_count;
    uint32_t fiw_raw;
    uint8_t cycle;
    uint8_t frame;

    uint16_t sync2_count;
    uint16_t data_count;
    uint16_t data_bit_count;
    bool phase_toggle;
    uint32_t phase_a[FLEX_PHASE_WORDS];
    uint32_t phase_c[FLEX_PHASE_WORDS];

    bool page_ready;
    uint32_t capcode;
    uint8_t page_type;
    uint8_t fragment;
    char phase;
};

typedef struct SubGhzProtocolDecoderFlex SubGhzProtocolDecoderFlex;

static uint8_t flex_popcount(uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return (uint8_t)__builtin_popcount(value);
#else
    uint8_t count = 0;
    while(value) {
        value &= value - 1U;
        count++;
    }
    return count;
#endif
}

static void flex_clear_frame(SubGhzProtocolDecoderFlex* instance) {
    instance->fiw_count = 0;
    instance->fiw_raw = 0;
    instance->sync2_count = 0;
    instance->data_count = 0;
    instance->data_bit_count = 0;
    instance->phase_toggle = false;
}

static void flex_return_to_sync(SubGhzProtocolDecoderFlex* instance) {
    instance->state = FlexDecoderStateSync1;
    instance->sync_buffer = 0;
    instance->baud = FLEX_SYNC_BAUD;
    instance->levels = 2;
    instance->inverted = false;
    flex_clear_frame(instance);
}

void* subghz_protocol_decoder_flex_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolDecoderFlex* instance = malloc(sizeof(SubGhzProtocolDecoderFlex));
    memset(instance, 0, sizeof(SubGhzProtocolDecoderFlex));
    instance->base.protocol = &subghz_protocol_flex;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->generic.result_ric = furi_string_alloc();
    instance->generic.result_msg = furi_string_alloc();
    flex_return_to_sync(instance);
    return instance;
}

void subghz_protocol_decoder_flex_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    furi_string_free(instance->generic.result_ric);
    furi_string_free(instance->generic.result_msg);
    free(instance);
}

void subghz_protocol_decoder_flex_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    flex_return_to_sync(instance);
    instance->page_ready = false;
    instance->capcode = 0;
    instance->page_type = 0;
    instance->fragment = 0;
    instance->phase = 'A';
    furi_string_reset(instance->generic.result_ric);
    furi_string_reset(instance->generic.result_msg);
}

static uint16_t flex_sync_check(uint64_t buffer) {
    const uint32_t marker = (uint32_t)((buffer >> 16) & 0xFFFFFFFFULL);
    const uint16_t code_high = (uint16_t)(buffer >> 48);
    const uint16_t code_low = (uint16_t)~(uint16_t)buffer;

    if((flex_popcount(marker ^ FLEX_SYNC_MARKER) < 4U) &&
       (flex_popcount((uint16_t)(code_low ^ code_high)) < 4U)) {
        return code_high;
    }
    return 0;
}

static bool flex_select_mode(SubGhzProtocolDecoderFlex* instance, uint16_t sync_code) {
    for(size_t i = 0; i < COUNT_OF(flex_modes); i++) {
        if(flex_popcount((uint16_t)(flex_modes[i].sync ^ sync_code)) < 4U) {
            if(flex_modes[i].levels != 2U) {
                FURI_LOG_D(
                    TAG,
                    "Ignoring unsupported %u/%u FLEX frame",
                    flex_modes[i].baud,
                    flex_modes[i].levels);
                return false;
            }
            instance->baud = flex_modes[i].baud;
            instance->levels = flex_modes[i].levels;
            return true;
        }
    }
    return false;
}

static bool flex_decode_fiw(SubGhzProtocolDecoderFlex* instance) {
    uint32_t fiw = instance->fiw_raw;
    if(flex_bch_correct(&fiw) < 0) return false;

    fiw &= 0x001FFFFFUL;
    uint8_t checksum = fiw & 0x0F;
    checksum += (fiw >> 4) & 0x0F;
    checksum += (fiw >> 8) & 0x0F;
    checksum += (fiw >> 12) & 0x0F;
    checksum += (fiw >> 16) & 0x0F;
    checksum += (fiw >> 20) & 0x01;
    if((checksum & 0x0F) != 0x0F) return false;

    instance->cycle = (fiw >> 4) & 0x0F;
    instance->frame = (fiw >> 8) & 0x7F;
    return true;
}

static void flex_set_page_header(
    SubGhzProtocolDecoderFlex* instance,
    uint32_t capcode,
    uint8_t type,
    uint8_t fragment,
    char fragment_flag,
    char phase) {
    instance->capcode = capcode;
    instance->page_type = type;
    instance->fragment = fragment;
    instance->phase = phase;
    furi_string_printf(
        instance->generic.result_ric,
        "[F%u/%c S%u%c %02u.%03u]\n\e#CAP: %09" PRIu32 "\e#\n",
        instance->baud,
        phase,
        fragment,
        fragment_flag,
        instance->cycle,
        instance->frame,
        capcode);
}

static bool flex_word_range_valid(
    const bool valid[FLEX_PHASE_WORDS],
    uint8_t first,
    uint8_t last) {
    if(first >= FLEX_PHASE_WORDS || last >= FLEX_PHASE_WORDS || first > last) return false;
    for(uint8_t i = first; i <= last; i++) {
        if(!valid[i]) return false;
    }
    return true;
}

static void flex_append_character(FuriString* output, uint8_t character) {
    if(character == 0x03) return;
    if(character == '\r' || character == '\n' || character == '\t') {
        furi_string_push_back(output, ' ');
    } else if(character >= 0x20 && character <= 0x7E) {
        furi_string_push_back(output, (char)character);
    }
}

static bool flex_parse_alphanumeric(
    SubGhzProtocolDecoderFlex* instance,
    const uint32_t words[FLEX_PHASE_WORDS],
    const bool valid[FLEX_PHASE_WORDS],
    uint32_t capcode,
    uint8_t type,
    uint8_t first,
    uint8_t last,
    char phase) {
    if(!flex_word_range_valid(valid, first, last)) return false;

    const uint8_t fragment = (words[first] >> 11) & 0x03;
    const bool continuation = (words[first] >> 10) & 0x01;
    const char fragment_flag = continuation ? 'F' : (fragment == 3U ? 'K' : 'C');
    furi_string_set_str(instance->generic.result_msg, "ALN: ");
    const size_t message_start = furi_string_size(instance->generic.result_msg);

    first++;
    if(first > last) return false;
    for(uint8_t i = first; i <= last; i++) {
        const uint32_t word = words[i];
        if(i > first || fragment != 3U) {
            flex_append_character(instance->generic.result_msg, word & 0x7F);
        }
        flex_append_character(instance->generic.result_msg, (word >> 7) & 0x7F);
        flex_append_character(instance->generic.result_msg, (word >> 14) & 0x7F);
    }

    if(furi_string_size(instance->generic.result_msg) == message_start) return false;
    flex_set_page_header(instance, capcode, type, fragment, fragment_flag, phase);
    return true;
}

static bool flex_parse_numeric(
    SubGhzProtocolDecoderFlex* instance,
    const uint32_t words[FLEX_PHASE_WORDS],
    const bool valid[FLEX_PHASE_WORDS],
    uint32_t capcode,
    uint8_t type,
    uint32_t vector,
    char phase) {
    static const char flex_bcd[] = "0123456789 U -][";
    const uint8_t first = (vector >> 7) & 0x7F;
    const uint16_t last = first + ((vector >> 14) & 0x07);
    if(last >= FLEX_PHASE_WORDS ||
       !flex_word_range_valid(valid, first, (uint8_t)last)) {
        return false;
    }

    furi_string_set_str(instance->generic.result_msg, "NUM: ");
    const size_t message_start = furi_string_size(instance->generic.result_msg);
    uint8_t digit = 0;
    uint8_t remaining = (type == FlexPageTypeNumberedNumeric) ? 14U : 6U;

    for(uint8_t word_index = first; word_index <= (uint8_t)last; word_index++) {
        uint32_t word = words[word_index];
        for(uint8_t bit = 0; bit < 21U; bit++) {
            digit = (digit >> 1) & 0x0F;
            if(word & 1U) digit ^= 0x08;
            word >>= 1;
            if(--remaining == 0) {
                if(digit != 0x0C) {
                    furi_string_push_back(instance->generic.result_msg, flex_bcd[digit]);
                }
                remaining = 4;
            }
        }
    }

    if(furi_string_size(instance->generic.result_msg) == message_start) return false;
    flex_set_page_header(instance, capcode, type, 0, 'K', phase);
    return true;
}

static bool flex_parse_tone(
    SubGhzProtocolDecoderFlex* instance,
    uint32_t capcode,
    uint8_t type,
    uint32_t vector,
    char phase) {
    static const char flex_bcd[] = "0123456789 U -][";
    furi_string_set_str(instance->generic.result_msg, "TON: ");

    if(((vector >> 7) & 0x03) == 0) {
        for(uint8_t shift = 9; shift <= 17; shift += 4) {
            furi_string_push_back(instance->generic.result_msg, flex_bcd[(vector >> shift) & 0x0F]);
        }
    } else {
        furi_string_cat_str(instance->generic.result_msg, "Tone only");
    }

    flex_set_page_header(instance, capcode, type, 0, 'K', phase);
    return true;
}

static bool flex_decode_phase(
    SubGhzProtocolDecoderFlex* instance,
    uint32_t words[FLEX_PHASE_WORDS],
    char phase) {
    bool valid[FLEX_PHASE_WORDS];
    for(uint8_t i = 0; i < FLEX_PHASE_WORDS; i++) {
        valid[i] = flex_bch_correct(&words[i]) >= 0;
        if(valid[i]) words[i] &= 0x001FFFFFUL;
    }

    if(!valid[0]) return false;
    const uint32_t biw = words[0];
    if(biw == 0 || biw == 0x001FFFFFUL) return false;

    const uint8_t vector_offset = (biw >> 10) & 0x3F;
    const uint8_t address_offset = ((biw >> 8) & 0x03) + 1U;
    if(vector_offset < address_offset ||
       (uint16_t)vector_offset + (uint16_t)(vector_offset - address_offset) >
           FLEX_PHASE_WORDS) {
        return false;
    }

    for(uint8_t address_index = address_offset; address_index < vector_offset; address_index++) {
        const uint8_t vector_index = vector_offset + address_index - address_offset;
        if(!valid[address_index] || !valid[vector_index]) continue;

        const uint32_t address = words[address_index];
        if(address == 0 || address == 0x001FFFFFUL) continue;

        const bool long_address = address < 0x008001UL || address > 0x1E0000UL;
        if(long_address || address < 0x8000UL) continue;
        const uint32_t capcode = address - 0x8000UL;

        const uint32_t vector = words[vector_index];
        const uint8_t type = (vector >> 4) & 0x07;
        if(type == FlexPageTypeShortInstruction) continue;

        if(type == FlexPageTypeAlphanumeric || type == FlexPageTypeSecure) {
            const uint8_t first = (vector >> 7) & 0x7F;
            const uint8_t length = (vector >> 14) & 0x7F;
            if(length == 0) continue;
            const uint16_t last = first + length - 1U;
            if(last >= FLEX_PHASE_WORDS) continue;
            if(flex_parse_alphanumeric(
                   instance,
                   words,
                   valid,
                   capcode,
                   type,
                   first,
                   (uint8_t)last,
                   phase)) {
                return true;
            }
        } else if(
            type == FlexPageTypeStandardNumeric || type == FlexPageTypeSpecialNumeric ||
            type == FlexPageTypeNumberedNumeric) {
            if(flex_parse_numeric(instance, words, valid, capcode, type, vector, phase)) {
                return true;
            }
        } else if(type == FlexPageTypeTone) {
            if(flex_parse_tone(instance, capcode, type, vector, phase)) return true;
        }
    }
    return false;
}

static void flex_decode_data(SubGhzProtocolDecoderFlex* instance) {
    instance->page_ready = flex_decode_phase(instance, instance->phase_a, 'A');
    if(!instance->page_ready && instance->baud == 3200U) {
        instance->page_ready = flex_decode_phase(instance, instance->phase_c, 'C');
    }
}

static void flex_read_data_bit(SubGhzProtocolDecoderFlex* instance, bool bit) {
    const uint16_t index =
        ((instance->data_bit_count >> 5) & 0xFFF8U) | (instance->data_bit_count & 0x0007U);
    if(index >= FLEX_PHASE_WORDS) return;

    if(instance->baud == 1600U || !instance->phase_toggle) {
        instance->phase_a[index] =
            (instance->phase_a[index] >> 1) | (bit ? 0x80000000UL : 0);
        instance->phase_toggle = instance->baud == 3200U;
        if(instance->baud == 1600U) instance->data_bit_count++;
    } else {
        instance->phase_c[index] =
            (instance->phase_c[index] >> 1) | (bit ? 0x80000000UL : 0);
        instance->phase_toggle = false;
        instance->data_bit_count++;
    }
}

static void flex_process_symbol(SubGhzProtocolDecoderFlex* instance, bool level) {
    if(instance->state == FlexDecoderStateSync1) {
        instance->sync_buffer = (instance->sync_buffer << 1) | (level ? 0U : 1U);
        uint16_t sync_code = flex_sync_check(instance->sync_buffer);
        bool inverted = false;
        if(sync_code == 0) {
            sync_code = flex_sync_check(~instance->sync_buffer);
            inverted = sync_code != 0;
        }

        if(sync_code != 0 && flex_select_mode(instance, sync_code)) {
            instance->inverted = inverted;
            instance->fiw_count = 0;
            instance->fiw_raw = 0;
            instance->state = FlexDecoderStateFiw;
            FURI_LOG_D(
                TAG,
                "Sync %04X %u/%u %s",
                sync_code,
                instance->baud,
                instance->levels,
                inverted ? "INV" : "NOR");
        }
        return;
    }

    const bool bit = instance->inverted ? !level : level;
    switch(instance->state) {
    case FlexDecoderStateFiw:
        instance->fiw_count++;
        if(instance->fiw_count > 16U) {
            instance->fiw_raw = (instance->fiw_raw >> 1) | (bit ? 0x80000000UL : 0);
        }
        if(instance->fiw_count == 48U) {
            if(flex_decode_fiw(instance)) {
                instance->sync2_count = 0;
                instance->state = FlexDecoderStateSync2;
            } else {
                flex_return_to_sync(instance);
            }
        }
        break;

    case FlexDecoderStateSync2:
        instance->sync2_count++;
        if(instance->sync2_count >= (instance->baud * FLEX_SYNC2_DURATION_MS) / 1000U) {
            instance->data_count = 0;
            instance->data_bit_count = 0;
            instance->phase_toggle = false;
            memset(instance->phase_a, 0, sizeof(instance->phase_a));
            memset(instance->phase_c, 0, sizeof(instance->phase_c));
            instance->state = FlexDecoderStateData;
        }
        break;

    case FlexDecoderStateData:
        flex_read_data_bit(instance, bit);
        instance->data_count++;
        if(instance->data_count >= (instance->baud * FLEX_DATA_DURATION_MS) / 1000U) {
            flex_decode_data(instance);
            instance->state = FlexDecoderStateSync1;
            instance->sync_buffer = 0;
            instance->data_count = 0;
            if(instance->page_ready && instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
        }
        break;

    case FlexDecoderStateSync1:
        break;
    }
}

void subghz_protocol_decoder_flex_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;

    int32_t remaining_half_us = (int32_t)duration * 2;
    uint16_t processed = 0;
    while(processed < FLEX_MAX_RUN_SYMBOLS) {
        const uint16_t symbol_period =
            (instance->state == FlexDecoderStateSync1 ||
             instance->state == FlexDecoderStateFiw || instance->baud == 1600U) ?
                1250U :
                625U;
        if(remaining_half_us < (int32_t)(symbol_period / 2U)) break;

        flex_process_symbol(instance, level);
        remaining_half_us -= symbol_period;
        processed++;
    }

    const int32_t timing_error = remaining_half_us < 0 ? -remaining_half_us : remaining_half_us;
    const int32_t timing_tolerance =
        (instance->state == FlexDecoderStateSync1 ||
         instance->state == FlexDecoderStateFiw || instance->baud == 1600U) ?
            200 :
            120;
    if(processed == FLEX_MAX_RUN_SYMBOLS || timing_error > timing_tolerance) {
        flex_return_to_sync(instance);
    }
}

uint8_t subghz_protocol_decoder_flex_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    uint8_t hash = 0;
    for(size_t i = 0; i < furi_string_size(instance->generic.result_ric); i++) {
        hash ^= furi_string_get_char(instance->generic.result_ric, i);
    }
    for(size_t i = 0; i < furi_string_size(instance->generic.result_msg); i++) {
        hash ^= furi_string_get_char(instance->generic.result_msg, i);
    }
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_flex_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    if(pcsg_block_generic_serialize(&instance->generic, flipper_format, preset) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    uint32_t value = instance->baud;
    if(!flipper_format_write_uint32(flipper_format, "FlexBaud", &value, 1)) {
        return SubGhzProtocolStatusError;
    }
    value = instance->cycle;
    if(!flipper_format_write_uint32(flipper_format, "FlexCycle", &value, 1)) {
        return SubGhzProtocolStatusError;
    }
    value = instance->frame;
    if(!flipper_format_write_uint32(flipper_format, "FlexFrame", &value, 1)) {
        return SubGhzProtocolStatusError;
    }
    value = instance->fragment;
    if(!flipper_format_write_uint32(flipper_format, "FlexSequence", &value, 1)) {
        return SubGhzProtocolStatusError;
    }
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_flex_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    if(pcsg_block_generic_deserialize(&instance->generic, flipper_format) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    uint32_t value;
    if(flipper_format_read_uint32(flipper_format, "FlexBaud", &value, 1)) {
        instance->baud = value;
    }
    if(flipper_format_read_uint32(flipper_format, "FlexCycle", &value, 1)) {
        instance->cycle = value;
    }
    if(flipper_format_read_uint32(flipper_format, "FlexFrame", &value, 1)) {
        instance->frame = value;
    }
    if(flipper_format_read_uint32(flipper_format, "FlexSequence", &value, 1)) {
        instance->fragment = value;
    }
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_flex_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    furi_string_cat_printf(
        output,
        "FLEX %u/2 %02u.%03u %c\r\nCAP: %09" PRIu32 "\r\n",
        instance->baud,
        instance->cycle,
        instance->frame,
        instance->phase,
        instance->capcode);
    furi_string_cat(output, instance->generic.result_msg);
}

const SubGhzProtocolDecoder subghz_protocol_flex_decoder = {
    .alloc = subghz_protocol_decoder_flex_alloc,
    .free = subghz_protocol_decoder_flex_free,
    .reset = subghz_protocol_decoder_flex_reset,
    .feed = subghz_protocol_decoder_flex_feed,
    .get_hash_data = subghz_protocol_decoder_flex_get_hash_data,
    .serialize = subghz_protocol_decoder_flex_serialize,
    .deserialize = subghz_protocol_decoder_flex_deserialize,
    .get_string = subghz_protocol_decoder_flex_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_flex_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_flex = {
    .name = SUBGHZ_PROTOCOL_FLEX_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Load,
    .decoder = &subghz_protocol_flex_decoder,
    .encoder = &subghz_protocol_flex_encoder,
};
