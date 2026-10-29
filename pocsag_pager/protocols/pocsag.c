#include "pocsag.h"
#include "flex_bch.h"

#include <inttypes.h>
#include <lib/flipper_format/flipper_format_i.h>
#include <furi/core/string.h>

#define TAG "POCSAG"

static const SubGhzBlockConst pocsag_const = {
    .te_short = 833,
    .te_delta = 100,
};
static const SubGhzBlockConst pocsag512_const = {
    .te_short = 1950,
    .te_long = 1950,
    .te_delta = 120,
};
static const SubGhzBlockConst pocsag2400_const = {
    .te_short = 417,
    .te_long = 417,
    .te_delta = 60,
};

// Minimal amount of sync bits (interleaving zeros and ones)
#define POCSAG_MIN_SYNC_BITS 24
#define POCSAG_CW_BITS 32
#define POCSAG_CW_MASK 0xFFFFFFFF
#define POCSAG_FRAME_SYNC_CODE 0x7CD215D8
#define POCSAG_IDLE_CODE_WORD 0x7A89C197

#define POCSAG_FUNC_NUM 0
#define POCSAG_FUNC_ALERT1 1
#define POCSAG_FUNC_ALERT2 2
#define POCSAG_FUNC_ALPHANUM 3
#define POCSAG_MAX_RUN_BITS 512U
#define POCSAG_MAX_MESSAGE_CHARS 1024U
#define POCSAG_BATCH_CODEWORDS 16U
#define POCSAG_SYNC_MAX_ERRORS 2U

static const char* func_msg[] = {"\e#Num:\e# ", "\e#Alert\e#", "\e#Alert:\e# ", "\e#Msg:\e# "};
static const char* bcd_chars = "*U -)(";

struct SubGhzProtocolDecoderPocsag {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    PCSGBlockGeneric generic;

    uint8_t codeword_idx;
    uint32_t ric;
    uint8_t func;

    // partially decoded character
    uint8_t char_bits;
    uint8_t char_data;
    bool char_erased;

    // message being decoded
    FuriString* msg;

    // Done messages, ready to be serialized/deserialized
    FuriString* done_msg;

    const SubGhzBlockConst* pocsag_timing;
    uint32_t timing_sum;
    uint16_t bit_period;
    uint8_t timing_samples;
    uint32_t version;
    bool overflowed;
    // Opposite polarity to the usual one, detected from the frame sync word.
    bool inverted;
};

typedef struct SubGhzProtocolDecoderPocsag SubGhzProtocolDecoderPocsag;

typedef enum {
    PocsagDecoderStepReset = 0,
    PocsagDecoderStepFoundSync,
    PocsagDecoderStepFoundPreamble,
    PocsagDecoderStepMessage,
} PocsagDecoderStep;

void* subghz_protocol_decoder_pocsag_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolDecoderPocsag* instance = calloc(1, sizeof(SubGhzProtocolDecoderPocsag));
    instance->base.protocol = &subghz_protocol_pocsag;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->msg = furi_string_alloc();
    instance->done_msg = furi_string_alloc();
    instance->pocsag_timing = NULL; //not synced yet
    instance->generic.result_msg = furi_string_alloc();
    instance->generic.result_ric = furi_string_alloc();
    instance->generic.serialize_scratch = furi_string_alloc();

    return instance;
}

void subghz_protocol_decoder_pocsag_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;
    furi_string_free(instance->msg);
    furi_string_free(instance->done_msg);
    if(instance->generic.result_msg != NULL) {
        furi_string_free(instance->generic.result_msg);
    }
    if(instance->generic.result_ric != NULL) {
        furi_string_free(instance->generic.result_ric);
    }
    furi_string_free(instance->generic.serialize_scratch);
    free(instance);
}

void subghz_protocol_decoder_pocsag_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;

    instance->decoder.parser_step = PocsagDecoderStepReset;
    instance->decoder.decode_data = 0UL;
    instance->decoder.decode_count_bit = 0;
    instance->codeword_idx = 0;
    instance->char_bits = 0;
    instance->char_data = 0;
    instance->char_erased = false;
    instance->timing_sum = 0;
    instance->timing_samples = 0;
    instance->bit_period = 0;
    instance->overflowed = false;
    instance->inverted = false;
    furi_string_reset(instance->msg);
    furi_string_reset(instance->done_msg);
    furi_string_reset(instance->generic.result_msg);
    furi_string_reset(instance->generic.result_ric);
}

static uint32_t pocsag_reverse_bits(uint32_t value) {
    value = ((value >> 1) & 0x55555555U) | ((value & 0x55555555U) << 1);
    value = ((value >> 2) & 0x33333333U) | ((value & 0x33333333U) << 2);
    value = ((value >> 4) & 0x0F0F0F0FU) | ((value & 0x0F0F0F0FU) << 4);
    value = ((value >> 8) & 0x00FF00FFU) | ((value & 0x00FF00FFU) << 8);
    return (value >> 16) | (value << 16);
}

// Correct the 31 BCH bits in the same internal orientation used by FLEX, then
// use the extended parity bit to reject the ambiguous three-error case. A bad
// parity bit alone, or together with one BCH error, is still fully repairable.
static bool pocsag_correct_codeword(uint32_t* codeword) {
    uint32_t reversed = pocsag_reverse_bits(*codeword);
    const int corrected = flex_bch_correct(&reversed);
    if(corrected < 0) return false;

    const bool parity_bad = (__builtin_popcount(reversed) & 1U) != 0U;
    if(parity_bad && corrected > 1) return false;
    if(parity_bad) reversed ^= 0x80000000UL;
    *codeword = pocsag_reverse_bits(reversed);
    return true;
}

static bool pocsag_sync_match(uint32_t word, bool* inverted) {
    const uint8_t normal_errors = __builtin_popcount(word ^ POCSAG_FRAME_SYNC_CODE);
    const uint8_t inverted_errors =
        __builtin_popcount(word ^ (uint32_t)~POCSAG_FRAME_SYNC_CODE);
    if(normal_errors > POCSAG_SYNC_MAX_ERRORS && inverted_errors > POCSAG_SYNC_MAX_ERRORS) {
        return false;
    }
    *inverted = inverted_errors < normal_errors;
    return true;
}

static void pocsag_decode_address_word(SubGhzProtocolDecoderPocsag* instance, uint32_t data) {
    instance->ric = (data >> 13);
    instance->ric = (instance->ric << 3) | (instance->codeword_idx >> 1);
    instance->func = (data >> 11) & 0b11;
}

static bool decode_message_alphanumeric(SubGhzProtocolDecoderPocsag* instance, uint32_t data) {
    for(uint8_t i = 0; i < 20; i++) {
        instance->char_data >>= 1;
        if(data & (1 << 30)) {
            instance->char_data |= 1 << 6;
        }
        instance->char_bits++;
        if(instance->char_bits == 7) {
            if(!instance->char_erased && instance->char_data == 0) return false;
            // Past the limit keep parsing so the message still ends cleanly,
            // and mark it as truncated.
            if(furi_string_size(instance->msg) >= POCSAG_MAX_MESSAGE_CHARS) {
                instance->overflowed = true;
            } else {
                furi_string_push_back(
                    instance->msg, instance->char_erased ? '?' : instance->char_data);
            }
            instance->char_data = 0;
            instance->char_bits = 0;
            instance->char_erased = false;
        }
        data <<= 1;
    }
    return true;
}

static bool decode_message_numeric(SubGhzProtocolDecoderPocsag* instance, uint32_t data) {
    // 5 groups with 4 bits each
    uint8_t val;
    for(uint8_t i = 0; i < 5; i++) {
        val = (data >> (27 - i * 4)) & 0b1111;
        // reverse the order of 4 bits
        val = (val & 0x5) << 1 | (val & 0xA) >> 1;
        val = (val & 0x3) << 2 | (val & 0xC) >> 2;

        if(val <= 9)
            val += '0';
        else
            val = bcd_chars[val - 10];
        if(furi_string_size(instance->msg) >= POCSAG_MAX_MESSAGE_CHARS) {
            instance->overflowed = true;
        } else {
            furi_string_push_back(instance->msg, val);
        }
    }
    return true;
}

// decode message word, maintaining instance state for partial decoding. Return true if more data
// might follow or false if end of message reached.
static bool pocsag_decode_message_word(SubGhzProtocolDecoderPocsag* instance, uint32_t data) {
    switch(instance->func) {
    case POCSAG_FUNC_ALERT2:
    case POCSAG_FUNC_ALPHANUM:
        return decode_message_alphanumeric(instance, data);

    case POCSAG_FUNC_NUM:
        return decode_message_numeric(instance, data);
    }
    return false;
}

// Preserve the position of an uncorrectable message word so later valid words
// can still be decoded. Unknown characters are explicit rather than silently
// turning a damaged codeword into plausible-looking text.
static void pocsag_decode_erasure(SubGhzProtocolDecoderPocsag* instance) {
    uint8_t unknown = 0;
    if(instance->func == POCSAG_FUNC_NUM) {
        unknown = 5;
    } else if(instance->func == POCSAG_FUNC_ALERT2 ||
              instance->func == POCSAG_FUNC_ALPHANUM) {
        const uint8_t bits = instance->char_bits + 20U;
        unknown = bits / 7U;
        instance->char_bits = bits % 7U;
        instance->char_data = 0;
        instance->char_erased = instance->char_bits != 0U;
    }
    while(unknown-- > 0U) {
        if(furi_string_size(instance->msg) >= POCSAG_MAX_MESSAGE_CHARS) {
            instance->overflowed = true;
        } else {
            furi_string_push_back(instance->msg, '?');
        }
    }
}

// Function called when current message got decoded, but other messages might follow
// Emit the message for the current address as its own entry, so each entry
// has exactly one RIC; a transmission can carry messages for many pagers.
static bool pocsag_message_done(SubGhzProtocolDecoderPocsag* instance) {
    if(furi_string_size(instance->msg) > POCSAG_MAX_MESSAGE_CHARS - 3U) {
        furi_string_left(instance->msg, POCSAG_MAX_MESSAGE_CHARS - 3U);
        instance->overflowed = true;
    }
    if(instance->overflowed) {
        furi_string_cat_str(instance->msg, "...");
        instance->overflowed = false;
    }

    furi_string_printf(
        instance->generic.result_ric,
        "[P%lu]\e#RIC: %" PRIu32 "\e# | ",
        instance->version,
        instance->ric);
    furi_string_cat_str(instance->generic.result_ric, func_msg[instance->func]);
    if(instance->func != POCSAG_FUNC_ALERT1) {
        furi_string_set(instance->done_msg, instance->msg);
    } else {
        furi_string_reset(instance->done_msg);
    }
    furi_string_set(instance->generic.result_msg, instance->done_msg);

    // reset the state
    instance->char_bits = 0;
    instance->char_data = 0;
    instance->char_erased = false;
    furi_string_reset(instance->msg);

    if(instance->base.callback) instance->base.callback(&instance->base, instance->base.context);
    return true;
}

static void pocsag_add_preamble_bit(
    SubGhzProtocolDecoderPocsag* instance,
    bool level,
    uint32_t duration,
    const SubGhzBlockConst* timing,
    uint32_t version) {
    if(instance->pocsag_timing != timing) {
        subghz_protocol_decoder_pocsag_reset(instance);
        instance->pocsag_timing = timing;
        instance->version = version;
    }

    instance->timing_sum += duration;
    instance->timing_samples++;
    // POCSAG signals are normally inverted at the asynchronous data pin.
    subghz_protocol_blocks_add_bit(&instance->decoder, !level);
    if(instance->decoder.decode_count_bit == POCSAG_MIN_SYNC_BITS) {
        instance->bit_period = (uint16_t)((instance->timing_sum +
                                           instance->timing_samples / 2U) /
                                          instance->timing_samples);
        instance->decoder.parser_step = PocsagDecoderStepFoundSync;
        FURI_LOG_D(
            TAG, "Preamble, %lu bps, %u us", instance->version, instance->bit_period);
    }
}

void subghz_protocol_decoder_pocsag_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;

    // reset state - waiting for 32 bits of interleaving 1s and 0s
    if(instance->decoder.parser_step == PocsagDecoderStepReset) {
        if(DURATION_DIFF(duration, pocsag_const.te_short) < pocsag_const.te_delta) {
            pocsag_add_preamble_bit(instance, level, duration, &pocsag_const, 1200U);
        } else if(DURATION_DIFF(duration, pocsag512_const.te_short) < pocsag512_const.te_delta) {
            pocsag_add_preamble_bit(instance, level, duration, &pocsag512_const, 512U);
        } else if(DURATION_DIFF(duration, pocsag2400_const.te_short) < pocsag2400_const.te_delta) {
            pocsag_add_preamble_bit(instance, level, duration, &pocsag2400_const, 2400U);
        } else if(instance->decoder.decode_count_bit > 0) {
            subghz_protocol_decoder_pocsag_reset(context);
        }
        return;
    }

    const uint32_t bit_period =
        instance->bit_period ? instance->bit_period : instance->pocsag_timing->te_short;
    const uint32_t bits_count_rounded = (duration + bit_period / 2U) / bit_period;
    const uint32_t expected_duration = bits_count_rounded * bit_period;
    const uint32_t timing_error = DURATION_DIFF(duration, expected_duration);
    uint32_t bits_count = bits_count_rounded;

    if(bits_count == 0U || timing_error > instance->pocsag_timing->te_delta) {
        // in non-reset state we faced the error signal - we reached the end of the packet, flush data
        FURI_LOG_D(
            TAG,
            "End: step %d, %s, %u chars pending",
            (int)instance->decoder.parser_step,
            instance->inverted ? "inverted" : "normal",
            (unsigned)furi_string_size(instance->msg));
        // Earlier messages were emitted as they completed; keep a message
        // that the end of the signal cut off.
        if(instance->decoder.parser_step == PocsagDecoderStepMessage) {
            pocsag_message_done(instance);
        }
        subghz_protocol_decoder_pocsag_reset(context);
        return;
    }

    if(bits_count > POCSAG_MAX_RUN_BITS) {
        subghz_protocol_decoder_pocsag_reset(context);
        return;
    }

    uint32_t codeword;

    // handle state machine for every incoming bit
    while(bits_count-- > 0) {
        // POCSAG is usually received inverted; the sync word tells otherwise.
        subghz_protocol_blocks_add_bit(&instance->decoder, instance->inverted ? level : !level);

        switch(instance->decoder.parser_step) {
        case PocsagDecoderStepFoundSync: {
            // The preamble alternates, so it locks at either polarity. A
            // complemented sync word means the whole transmission is inverted.
            const uint32_t word = instance->decoder.decode_data & POCSAG_CW_MASK;
            bool complemented;
            if(pocsag_sync_match(word, &complemented)) {
                instance->inverted = complemented;
                FURI_LOG_D(
                    TAG,
                    "Sync %s, %lu bps",
                    complemented ? "inverted" : "normal",
                    instance->version);
                instance->decoder.parser_step = PocsagDecoderStepFoundPreamble;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.decode_data = 0UL;
            }
            break;
        }
        case PocsagDecoderStepFoundPreamble:
            // handle codewords
            if(instance->decoder.decode_count_bit == POCSAG_CW_BITS) {
                codeword = (uint32_t)(instance->decoder.decode_data & POCSAG_CW_MASK);
                instance->decoder.decode_count_bit = 0;
                instance->decoder.decode_data = 0UL;
                // A batch sync follows every 16 codewords. Even when it is too
                // damaged to repair, the known position preserves framing.
                if(instance->codeword_idx >= POCSAG_BATCH_CODEWORDS) {
                    instance->codeword_idx = 0;
                    break;
                }
                if(!pocsag_correct_codeword(&codeword)) {
                    instance->codeword_idx++;
                    break;
                }
                switch(codeword) {
                case POCSAG_IDLE_CODE_WORD:
                    instance->codeword_idx++;
                    break;
                case POCSAG_FRAME_SYNC_CODE:
                    instance->codeword_idx = 0;
                    break;
                default:
                    // Here we expect only address messages
                    if(codeword >> 31 == 0) {
                        pocsag_decode_address_word(instance, codeword);
                        instance->decoder.parser_step = PocsagDecoderStepMessage;
                    }
                    instance->codeword_idx++;
                }
            }
            break;

        case PocsagDecoderStepMessage:
            if(instance->decoder.decode_count_bit == POCSAG_CW_BITS) {
                codeword = (uint32_t)(instance->decoder.decode_data & POCSAG_CW_MASK);
                instance->decoder.decode_count_bit = 0;
                instance->decoder.decode_data = 0UL;
                // Messages may continue into the next batch; ignore its sync
                // slot even if the sync word itself has more than two errors.
                if(instance->codeword_idx >= POCSAG_BATCH_CODEWORDS) {
                    instance->codeword_idx = 0;
                    break;
                }
                if(!pocsag_correct_codeword(&codeword)) {
                    pocsag_decode_erasure(instance);
                    instance->codeword_idx++;
                    break;
                }
                switch(codeword) {
                case POCSAG_IDLE_CODE_WORD:
                    // Idle during the message stops the message
                    instance->codeword_idx++;
                    instance->decoder.parser_step = PocsagDecoderStepFoundPreamble;
                    if(!pocsag_message_done(instance)) {
                        subghz_protocol_decoder_pocsag_reset(context);
                        return;
                    }
                    break;
                case POCSAG_FRAME_SYNC_CODE:
                    instance->codeword_idx = 0;
                    break;
                default:
                    // In this state, both address and message words can arrive
                    if(codeword >> 31 == 0) {
                        if(!pocsag_message_done(instance)) {
                            subghz_protocol_decoder_pocsag_reset(context);
                            return;
                        }
                        pocsag_decode_address_word(instance, codeword);
                    } else {
                        if(!pocsag_decode_message_word(instance, codeword)) {
                            instance->decoder.parser_step = PocsagDecoderStepFoundPreamble;
                            if(!pocsag_message_done(instance)) {
                                subghz_protocol_decoder_pocsag_reset(context);
                                return;
                            }
                        }
                    }
                    instance->codeword_idx++;
                }
            }
            break;
        }
    }
}

uint32_t subghz_protocol_decoder_pocsag_get_ric(void* context) {
    furi_assert(context);
    const SubGhzProtocolDecoderPocsag* instance = context;
    return instance->ric;
}

uint8_t subghz_protocol_decoder_pocsag_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;
    // Include the RIC: one message is often sent to several pagers in a row.
    uint8_t hash = instance->ric ^ (instance->ric >> 8) ^ (instance->ric >> 16);
    for(size_t i = 0; i < furi_string_size(instance->done_msg); i++)
        hash ^= furi_string_get_char(instance->done_msg, i);
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_pocsag_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;
    uint32_t msg_len;

    if(SubGhzProtocolStatusOk !=
       pcsg_block_generic_serialize(&instance->generic, flipper_format, preset))
        return SubGhzProtocolStatusError;

    msg_len = furi_string_size(instance->done_msg);
    if(!flipper_format_write_uint32(flipper_format, "MsgLen", &msg_len, 1)) {
        FURI_LOG_E(TAG, "Error adding MsgLen");
        return SubGhzProtocolStatusError;
    }

    if(!flipper_format_write_uint32(flipper_format, "PocsagVer", &instance->version, 1)) {
        FURI_LOG_E(TAG, "Error adding PocsagVer");
        return SubGhzProtocolStatusError;
    }

    uint8_t* s = (uint8_t*)furi_string_get_cstr(instance->done_msg);
    if(!flipper_format_write_hex(flipper_format, "Msg", s, msg_len)) {
        FURI_LOG_E(TAG, "Error adding Msg");
        return SubGhzProtocolStatusError;
    }
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_pocsag_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    uint32_t msg_len;
    uint8_t* buf;

    do {
        if(SubGhzProtocolStatusOk !=
           pcsg_block_generic_deserialize(&instance->generic, flipper_format)) {
            break;
        }

        if(!flipper_format_read_uint32(flipper_format, "MsgLen", &msg_len, 1)) {
            FURI_LOG_E(TAG, "Missing MsgLen");
            break;
        }
        if(msg_len > POCSAG_MAX_MESSAGE_CHARS) {
            FURI_LOG_E(TAG, "Message too large");
            break;
        }
        //optional, so compatible backwards
        instance->version = 1200;
        flipper_format_read_uint32(flipper_format, "PocsagVer", &instance->version, 1);

        buf = msg_len ? malloc(msg_len) : NULL;
        if(msg_len && !flipper_format_read_hex(flipper_format, "Msg", buf, msg_len)) {
            FURI_LOG_E(TAG, "Missing Msg");
            free(buf);
            break;
        }
        if(msg_len) {
            furi_string_set_strn(instance->done_msg, (const char*)buf, msg_len);
        } else {
            furi_string_reset(instance->done_msg);
        }
        free(buf);

        ret = SubGhzProtocolStatusOk;
    } while(false);
    return ret;
}

void subhz_protocol_decoder_pocsag_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;
    furi_string_cat_printf(
        output, "%s %lu\r\n", instance->generic.protocol_name, instance->version);
    furi_string_cat_printf(output, "Addr: %lu\r\n", instance->ric);
    furi_string_cat(output, instance->done_msg);
}

const SubGhzProtocolDecoder subghz_protocol_pocsag_decoder = {
    .alloc = subghz_protocol_decoder_pocsag_alloc,
    .free = subghz_protocol_decoder_pocsag_free,
    .reset = subghz_protocol_decoder_pocsag_reset,
    .feed = subghz_protocol_decoder_pocsag_feed,
    .get_hash_data = subghz_protocol_decoder_pocsag_get_hash_data,
    .serialize = subghz_protocol_decoder_pocsag_serialize,
    .deserialize = subghz_protocol_decoder_pocsag_deserialize,
    .get_string = subhz_protocol_decoder_pocsag_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_pocsag_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_pocsag = {
    .name = SUBGHZ_PROTOCOL_POCSAG_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Load,

    .decoder = &subghz_protocol_pocsag_decoder,
    .encoder = &subghz_protocol_pocsag_encoder,
};
