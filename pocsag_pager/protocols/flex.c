/*
 * FLEX pager decoder for the Flipper Zero CC1101 input.
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
#include <furi_hal_speaker.h>
#include <lib/flipper_format/flipper_format_i.h>

#define TAG "FLEX"

#define FLEX_SYNC_MARKER 0xA6C6AAAAUL
#define FLEX_PHASE_WORDS 88U
#define FLEX_SYNC_BAUD 1600U
#define FLEX_SYNC2_DURATION_MS 25U
#define FLEX_DATA_DURATION_MS 1760U
#define FLEX_MAX_RUN_SYMBOLS 512U
// A single fragment carries at most about 264 characters (88 words x 3).
#define FLEX_MAX_MESSAGE_CHARS 512U
#define FLEX_TRUNCATION_MARK   "..."
#define FLEX_NO_WORD           0xFFU
#define FLEX_NUMERIC_MAX_WORDS 9U

// Alphanumeric header word: checksum (bits 0-9), continuation (10), fragment
// (11-12), message number (13-18), retrieval flag (19). A retransmitted copy
// clears the retrieval flag, which also changes the checksum, so copies are
// compared on the remaining fields. Layout as built by PortaPack Mayhem's
// FLEX TX (flex_encode_alpha in firmware/application/external/flex_tx).
#define FLEX_ALPHA_HEADER_IDENTITY 0x0017FC00UL

// Long alphanumeric messages are sent as fragments: the first has fragment
// number 3, the rest 0, 1, 2, 0, ...; all but the last set continuation.
// Fragments are joined per capcode and message number.
// Group sends deliver the same split message to several capcodes at once,
// each needing a slot; idle slots hold no text.
#define FLEX_FRAGMENT_SLOTS      8U
#define FLEX_FRAGMENT_TIMEOUT_MS (4U * 60U * 1000U)
#define FLEX_ENCRYPTED_BASE64_RUN 28U
#define FLEX_ENCRYPTED_HEX_RUN    32U
#define FLEX_ENCRYPTED_GENERIC_RUN 40U
#define FLEX_ENCRYPTED_MIN_UNIQUE  16U
#define FLEX_JOINED_MAX_CHARS    1024U
#define FLEX_GAP_MARK            "[...]"

#define FLEX_PARTIAL_PAGES        4U
#define FLEX_PARTIAL_TIMEOUT_MS   (10U * 60U * 1000U)
#define FLEX_SEEN_PAGES           32U
#define FLEX_DUPLICATE_WINDOW_MS  (5U * 60U * 1000U)

// 4-FSK clock phase is a 16-bit fraction of one symbol; 0 is the boundary.
#define FLEX_CLOCK_QUARTER  0x4000U
#define FLEX_CLOCK_WINDOW   0x6000U
#define FLEX_CLOCK_CENTER   0x8000U
#define FLEX_CLOCK_DECISION 0xC000U

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

typedef struct {
    bool used;
    uint32_t tick;
    uint32_t capcode;
    uint8_t type;
    uint8_t length;
    bool valid[FLEX_PHASE_WORDS];
    // Raw copies seen of each unrecovered word; 0 once recovered.
    uint8_t copies[FLEX_PHASE_WORDS];
    // Decoded data once valid, otherwise the latest raw copy.
    uint32_t words[FLEX_PHASE_WORDS];
    // The raw copy before the latest, for majority voting.
    uint32_t previous[FLEX_PHASE_WORDS];
} FlexPartialPage;

typedef struct {
    uint32_t hash;
    uint32_t tick;
} FlexSeenPage;

typedef struct {
    bool used;
    uint32_t tick;
    uint32_t capcode;
    uint8_t type;
    uint8_t message_number;
    uint8_t next_fragment;
    uint8_t parts;
    uint8_t signature;
    uint8_t last_fragment;
    bool signature_present;
    bool last_continuation;
    bool chase;
    bool repaired;
    bool suspect;
    uint32_t signature_sum;
    uint32_t last_part_hash;
    size_t last_part_length;
    // Phase and frame of the first part received, where the pager listens.
    char phase;
    uint8_t frame;
    // Allocated while the message is being joined.
    FuriString* text;
} FlexFragmentSlot;

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
    uint8_t fiw_rel[32];
    bool frame_chase;
    uint8_t cycle;
    uint8_t frame;

    uint16_t sync2_count;
    uint16_t data_count;
    uint16_t data_bit_count;
    bool phase_toggle;
    uint32_t phase_a[FLEX_PHASE_WORDS];
    uint32_t phase_b[FLEX_PHASE_WORDS];
    uint32_t phase_c[FLEX_PHASE_WORDS];
    uint32_t phase_d[FLEX_PHASE_WORDS];

    bool four_level_path;
    bool clock_seen;
    bool clock_decided;
    uint16_t clock_baud;
    uint16_t clock_phase;
    uint8_t clock_last_level;
    uint8_t clock_center_level;
    uint8_t clock_votes[4];

    FlexDecoderStats stats;

    // Incomplete pages waiting for another copy (allocated on the first one,
    // about 3.5 KB) and pages emitted recently. Both survive decoder resets.
    FlexPartialPage* partial;
    FlexSeenPage seen[FLEX_SEEN_PAGES];
    uint8_t seen_next;
    FlexFragmentSlot fragments[FLEX_FRAGMENT_SLOTS];
    uint32_t merged_words[FLEX_PHASE_WORDS];
    bool merged_valid[FLEX_PHASE_WORDS];
    // Alphanumeric page gathered into one run: header word, then text.
    uint32_t page_words[FLEX_PHASE_WORDS];
    bool page_valid[FLEX_PHASE_WORDS];
    bool page_repaired;
    bool page_suspect;
    bool page_partial;
    bool page_duplicate;
    bool page_encrypted;
    // Where the page (or a joined message's first part) was received.
    char page_phase;
    uint8_t page_frame;

    uint32_t capcode;
    uint8_t page_type;
    uint8_t fragment;
    char phase;

    // Synthetic sound: one tone per recovered symbol while locked. The decoder
    // runs on the sampler thread, which owns the speaker in this mode.
    bool synth_enable;
    float synth_tones[4];
    float synth_volume;
    uint8_t synth_level;

    // Weak Decode (Chase): per-bit reliability from the DPLL vote margins,
    // allocated only while enabled (4 phases x 88 words x 32 bits).
    bool weak_decode;
    uint8_t* phase_rel;
    uint8_t cur_rel_a;
    uint8_t cur_rel_b;
    bool page_chase;
    // Signature from an initial alpha fragment and sum of this fragment's characters.
    uint8_t alpha_signature;
    uint32_t alpha_signature_sum;
};

#define FLEX_REL_WORD   32U
#define FLEX_REL_SIZE   (4U * FLEX_PHASE_WORDS * FLEX_REL_WORD)
#define FLEX_REL_HIGH   0xFFU

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
    memset(instance->fiw_rel, FLEX_REL_HIGH, sizeof(instance->fiw_rel));
    instance->frame_chase = false;
    instance->sync2_count = 0;
    instance->data_count = 0;
    instance->data_bit_count = 0;
    instance->phase_toggle = false;
}

static void flex_reset_clock(SubGhzProtocolDecoderFlex* instance) {
    instance->clock_seen = false;
    instance->clock_decided = false;
    instance->clock_baud = 0;
    instance->clock_phase = 0;
    instance->clock_last_level = 0;
    instance->clock_center_level = 0;
    memset(instance->clock_votes, 0, sizeof(instance->clock_votes));
}

// The 4-FSK symbol clock keeps running across frame boundaries and failed
// headers; only a full decoder reset discards it.
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
    free(instance->partial);
    free(instance->phase_rel);
    for(size_t i = 0; i < FLEX_FRAGMENT_SLOTS; i++) {
        if(instance->fragments[i].text) furi_string_free(instance->fragments[i].text);
    }
    free(instance);
}

void subghz_protocol_decoder_flex_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    flex_return_to_sync(instance);
    flex_reset_clock(instance);
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
            if(flex_modes[i].levels == 4U && !instance->four_level_path) {
                FURI_LOG_D(
                    TAG,
                    "Ignoring %u/%u FLEX frame on two-level input",
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

static bool flex_fiw_fields_valid(uint32_t fiw, uint8_t* cycle, uint8_t* frame) {
    fiw &= 0x001FFFFFUL;
    uint8_t checksum = fiw & 0x0F;
    checksum += (fiw >> 4) & 0x0F;
    checksum += (fiw >> 8) & 0x0F;
    checksum += (fiw >> 12) & 0x0F;
    checksum += (fiw >> 16) & 0x0F;
    checksum += (fiw >> 20) & 0x01;
    if((checksum & 0x0F) != 0x0F) return false;

    *cycle = (fiw >> 4) & 0x0F;
    *frame = (fiw >> 8) & 0x7F;
    if(*cycle > 14U) return false;
    return true;
}

static bool flex_decode_fiw(SubGhzProtocolDecoderFlex* instance) {
    uint8_t cycle;
    uint8_t frame;
    uint32_t fiw = instance->fiw_raw;
    if(flex_bch_correct(&fiw) >= 0 && flex_fiw_fields_valid(fiw, &cycle, &frame)) {
        instance->cycle = cycle;
        instance->frame = frame;
        instance->frame_chase = false;
        return true;
    }

    // The FIW gates the entire frame. If hard BCH or the FIW checksum fails,
    // give Weak Decode the original samples rather than discarding all phases.
    fiw = instance->fiw_raw;
    if(instance->weak_decode && instance->four_level_path &&
       flex_bch_chase(&fiw, instance->fiw_rel) &&
       flex_fiw_fields_valid(fiw, &cycle, &frame)) {
        instance->cycle = cycle;
        instance->frame = frame;
        instance->frame_chase = true;
        instance->stats.fiw_chase++;
        return true;
    }
    return false;
}

static bool flex_word_checksum_valid(uint32_t word) {
    uint8_t checksum = word & 0x0F;
    checksum += (word >> 4) & 0x0F;
    checksum += (word >> 8) & 0x0F;
    checksum += (word >> 12) & 0x0F;
    checksum += (word >> 16) & 0x0F;
    checksum += (word >> 20) & 0x01;
    return (checksum & 0x0F) == 0x0F;
}

static uint16_t flex_bit_rate(const SubGhzProtocolDecoderFlex* instance) {
    return instance->baud * (instance->levels == 4U ? 2U : 1U);
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
        "[F%u/%c S%u%c %02u.%03u%s%s%s]\n\e#CAP: %09" PRIu32 "\e#\n",
        flex_bit_rate(instance),
        phase,
        fragment,
        fragment_flag,
        instance->cycle,
        instance->frame,
        instance->page_repaired ? " R" : "",
        instance->page_chase ? " W" : "", // Weak Decode (Chase); not the 'C' fragment flag
        instance->page_suspect ? " ?" : "",
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

static bool flex_append_character(FuriString* output, uint8_t character) {
    if(character == 0x03) return true;
    if(furi_string_size(output) >= FLEX_MAX_MESSAGE_CHARS) return false;
    if(character == '\r' || character == '\n' || character == '\t') {
        furi_string_push_back(output, ' ');
    } else if(character >= 0x20 && character <= 0x7E) {
        furi_string_push_back(output, (char)character);
    }
    return true;
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
    instance->alpha_signature = 0;
    instance->alpha_signature_sum = 0;

    first++;
    if(first > last) return false;
    bool truncated = false;
    for(uint8_t i = first; i <= last && !truncated; i++) {
        const uint32_t word = words[i];
        if(i > first || fragment != 3U) {
            const uint8_t character = word & 0x7F;
            if(character != 0x03U) instance->alpha_signature_sum += character;
            truncated |= !flex_append_character(instance->generic.result_msg, character);
        } else {
            instance->alpha_signature = word & 0x7FU;
        }
        const uint8_t second = (word >> 7) & 0x7F;
        const uint8_t third = (word >> 14) & 0x7F;
        if(second != 0x03U) instance->alpha_signature_sum += second;
        if(third != 0x03U) instance->alpha_signature_sum += third;
        truncated |= !flex_append_character(instance->generic.result_msg, second);
        truncated |= !flex_append_character(instance->generic.result_msg, third);
    }

    if(furi_string_size(instance->generic.result_msg) == message_start) return false;
    if(truncated) furi_string_cat_str(instance->generic.result_msg, FLEX_TRUNCATION_MARK);
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
    uint8_t long_data_index,
    char phase) {
    static const char flex_bcd[] = "0123456789 U -][";
    const uint8_t first = (vector >> 7) & 0x7F;
    const uint16_t last = first + ((vector >> 14) & 0x07);
    if(last >= FLEX_PHASE_WORDS) return false;

    // As in multimon-ng's parse_numeric(), a long address carries the first
    // data word in its second vector slot, followed by the message words
    // except the last.
    uint8_t order[FLEX_NUMERIC_MAX_WORDS];
    uint8_t count = 0;
    if(long_data_index != FLEX_NO_WORD) {
        order[count++] = long_data_index;
        for(uint8_t i = first; i < last; i++) order[count++] = i;
    } else {
        for(uint8_t i = first; i <= last; i++) order[count++] = i;
    }
    for(uint8_t i = 0; i < count; i++) {
        if(!valid[order[i]]) return false;
    }

    furi_string_set_str(instance->generic.result_msg, "NUM: ");
    const size_t message_start = furi_string_size(instance->generic.result_msg);
    uint8_t digit = 0;
    uint8_t remaining = (type == FlexPageTypeNumberedNumeric) ? 14U : 6U;
    bool truncated = false;

    for(uint8_t i = 0; i < count && !truncated; i++) {
        uint32_t word = words[order[i]];
        for(uint8_t bit = 0; bit < 21U; bit++) {
            digit = (digit >> 1) & 0x0F;
            if(word & 1U) digit ^= 0x08;
            word >>= 1;
            if(--remaining == 0) {
                if(digit != 0x0C) {
                    if(furi_string_size(instance->generic.result_msg) >=
                       FLEX_MAX_MESSAGE_CHARS) {
                        truncated = true;
                        break;
                    }
                    furi_string_push_back(instance->generic.result_msg, flex_bcd[digit]);
                }
                remaining = 4;
            }
        }
    }

    if(furi_string_size(instance->generic.result_msg) == message_start) return false;
    if(truncated) furi_string_cat_str(instance->generic.result_msg, FLEX_TRUNCATION_MARK);
    flex_set_page_header(instance, capcode, type, 0, 'K', phase);
    return true;
}

static bool flex_parse_tone(
    SubGhzProtocolDecoderFlex* instance,
    uint32_t capcode,
    uint8_t type,
    uint32_t vector,
    const uint32_t* long_data,
    char phase) {
    static const char flex_bcd[] = "0123456789 U -][";
    furi_string_set_str(instance->generic.result_msg, "TON: ");

    if(((vector >> 7) & 0x03) == 0) {
        for(uint8_t shift = 9; shift <= 17; shift += 4) {
            furi_string_push_back(instance->generic.result_msg, flex_bcd[(vector >> shift) & 0x0F]);
        }
        // Long addresses add five digits from the second vector slot, as in
        // multimon-ng's parse_tone_only().
        if(long_data) {
            for(uint8_t shift = 0; shift <= 16; shift += 4) {
                furi_string_push_back(
                    instance->generic.result_msg, flex_bcd[(*long_data >> shift) & 0x0F]);
            }
        }
    } else {
        furi_string_cat_str(instance->generic.result_msg, "Tone only");
    }

    flex_set_page_header(instance, capcode, type, 0, 'K', phase);
    return true;
}

/*
 * Alphanumeric pages are parsed as one run: header word, then text words.
 * With a long address the header is carried in the second vector slot and
 * the message field holds only the text, although the vector length still
 * counts the header. Layout as built by PortaPack Mayhem's FLEX TX
 * (flex_encode_alpha) and confirmed by the header checksum on received pages.
 */
static void flex_gather_alpha_page(
    SubGhzProtocolDecoderFlex* instance,
    const uint32_t words[FLEX_PHASE_WORDS],
    const bool valid[FLEX_PHASE_WORDS],
    uint8_t first,
    uint8_t length,
    uint8_t header_index) {
    for(uint8_t i = 0; i < length; i++) {
        uint8_t source = first + i;
        if(header_index != FLEX_NO_WORD) source = i == 0 ? header_index : first + i - 1U;
        instance->page_words[i] = words[source];
        instance->page_valid[i] = valid[source];
    }
}

/*
 * Alphanumeric header bits 0-9 hold the ones' complement of the sum of bits
 * 0-7, 8-15 and 16-20 of every page word, taken with those header bits zero
 * (as built by PortaPack Mayhem's flex_encode_alpha; matches received pages).
 */
static bool flex_alpha_checksum_valid(const uint32_t* words, uint8_t first, uint8_t last) {
    uint32_t sum = 0;
    for(uint16_t i = first; i <= last; i++) {
        uint32_t word = words[i] & 0x001FFFFFUL;
        if(i == first) word &= ~0x3FFUL;
        sum += (word & 0xFFU) + ((word >> 8) & 0xFFU) + ((word >> 16) & 0x1FU);
    }
    return ((~sum) & 0x3FFU) == (words[first] & 0x3FFU);
}

// Binary pages are shown as raw 21-bit words, like multimon-ng's parse_unknown().
static bool flex_parse_binary(
    SubGhzProtocolDecoderFlex* instance,
    const uint32_t words[FLEX_PHASE_WORDS],
    const bool valid[FLEX_PHASE_WORDS],
    uint32_t capcode,
    uint8_t type,
    uint8_t first,
    uint8_t last,
    char phase) {
    if(!flex_word_range_valid(valid, first, last)) return false;

    furi_string_set_str(instance->generic.result_msg, "BIN:");
    for(uint8_t i = first; i <= last; i++) {
        if(furi_string_size(instance->generic.result_msg) + 7U > FLEX_MAX_MESSAGE_CHARS) {
            furi_string_cat_str(instance->generic.result_msg, FLEX_TRUNCATION_MARK);
            break;
        }
        furi_string_cat_printf(instance->generic.result_msg, " %06lX", words[i]);
    }

    flex_set_page_header(instance, capcode, type, 0, 'K', phase);
    return true;
}

// The part of a page word that is the same in every copy of the page.
static uint32_t flex_word_identity(uint8_t type, uint8_t index, uint32_t word) {
    word &= 0x001FFFFFUL;
    if(index == 0 && (type == FlexPageTypeAlphanumeric || type == FlexPageTypeSecure)) {
        word &= FLEX_ALPHA_HEADER_IDENTITY;
    }
    return word;
}

/*
 * Duplicate key for a message: capcode, type and text, ignoring a trailing
 * "[digits]" tag. Some senders repeat a message with only that tag counting
 * up ([81], [82]); any other difference, such as a new reading in a report,
 * keeps the messages apart. Comparing text rather than page words also
 * ignores header differences between copies, such as the retrieval flag.
 */
static uint32_t flex_text_hash(uint32_t capcode, uint8_t type, const char* text) {
    size_t length = strlen(text);
    while(length && text[length - 1] == ' ') length--;
    if(length && text[length - 1] == ']') {
        size_t start = length - 1;
        while(start && text[start - 1] >= '0' && text[start - 1] <= '9') start--;
        if(start < length - 1 && start && text[start - 1] == '[') {
            length = start - 1;
            while(length && text[length - 1] == ' ') length--;
        }
    }

    // FNV-1a; zero marks an empty slot in the seen list.
    uint32_t hash = 2166136261UL;
    hash = (hash ^ capcode) * 16777619UL;
    hash = (hash ^ type) * 16777619UL;
    for(size_t i = 0; i < length; i++) {
        hash = (hash ^ (uint8_t)text[i]) * 16777619UL;
    }
    return hash | 1U;
}

// Remember the page and report whether the same page was emitted recently.
static bool flex_seen_check(SubGhzProtocolDecoderFlex* instance, uint32_t hash) {
    const uint32_t now = furi_get_tick();
    const uint32_t window = furi_ms_to_ticks(FLEX_DUPLICATE_WINDOW_MS);
    for(size_t i = 0; i < FLEX_SEEN_PAGES; i++) {
        const FlexSeenPage* seen = &instance->seen[i];
        if(seen->hash == hash && now - seen->tick < window) return true;
    }
    instance->seen[instance->seen_next].hash = hash;
    instance->seen[instance->seen_next].tick = now;
    instance->seen_next = (instance->seen_next + 1U) % FLEX_SEEN_PAGES;
    return false;
}

static uint8_t flex_character_class(char c) {
    if(c >= 'A' && c <= 'Z') return 1U;
    if(c >= 'a' && c <= 'z') return 2U;
    if(c >= '0' && c <= '9') return 4U;
    return 8U;
}

static uint8_t flex_unique_characters(uint64_t low, uint64_t high) {
    return (uint8_t)(__builtin_popcountll(low) + __builtin_popcountll(high));
}

/*
 * Networks also carry encrypted data in ordinary ALN pages. Detect common
 * opaque encodings while remaining conservative around normal prose:
 *
 * - standard and URL-safe Base64 with mixed case and a digit or encoding mark;
 * - long hexadecimal strings containing both digits and A-F;
 * - long unbroken printable tokens with all four character classes and broad
 *   character diversity (for Base85-like encodings).
 *
 * FLEX_GAP_MARK is generated locally for a missing fragment. Treat it as
 * transparent so one damaged encrypted message does not become two short runs
 * that evade the filter.
 */
static bool flex_text_looks_encrypted(const char* text) {
    const size_t length = strlen(text);
    size_t base64_run = 0;
    size_t hex_run = 0;
    size_t printable_run = 0;
    uint8_t base64_classes = 0;
    uint8_t hex_classes = 0;
    uint8_t printable_classes = 0;
    uint64_t printable_low = 0;
    uint64_t printable_high = 0;

    for(size_t i = 0; i <= length; i++) {
        if(i < length && i + sizeof(FLEX_GAP_MARK) - 1U <= length &&
           !memcmp(text + i, FLEX_GAP_MARK, sizeof(FLEX_GAP_MARK) - 1U)) {
            i += sizeof(FLEX_GAP_MARK) - 2U;
            continue;
        }

        const char c = text[i];
        const bool upper = c >= 'A' && c <= 'Z';
        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        const bool base64 = upper || lower || digit || c == '+' || c == '/' || c == '=' ||
                            c == '-' || c == '_';
        if(base64) {
            base64_run++;
            base64_classes |= flex_character_class(c);
        } else {
            if(base64_run >= FLEX_ENCRYPTED_BASE64_RUN && (base64_classes & 3U) == 3U &&
               (base64_classes & 12U)) {
                return true;
            }
            base64_run = 0;
            base64_classes = 0;
        }

        const bool hex = digit || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if(hex) {
            hex_run++;
            hex_classes |= digit ? 1U : 2U;
        } else {
            if(hex_run >= FLEX_ENCRYPTED_HEX_RUN && hex_classes == 3U) return true;
            hex_run = 0;
            hex_classes = 0;
        }

        if(c >= 0x21 && c <= 0x7E) {
            printable_run++;
            printable_classes |= flex_character_class(c);
            const uint8_t index = (uint8_t)c - 0x21U;
            if(index < 64U) {
                printable_low |= 1ULL << index;
            } else {
                printable_high |= 1ULL << (index - 64U);
            }
        } else {
            if(printable_run >= FLEX_ENCRYPTED_GENERIC_RUN && printable_classes == 15U &&
               flex_unique_characters(printable_low, printable_high) >=
                   FLEX_ENCRYPTED_MIN_UNIQUE) {
                return true;
            }
            printable_run = 0;
            printable_classes = 0;
            printable_low = 0;
            printable_high = 0;
        }
    }
    return false;
}

static void flex_emit_page(SubGhzProtocolDecoderFlex* instance) {
    const char* message = furi_string_get_cstr(instance->generic.result_msg);
    const uint32_t hash = flex_text_hash(instance->capcode, instance->page_type, message);
    instance->page_phase = instance->phase;
    instance->page_frame = instance->frame;
    instance->page_encrypted =
        instance->page_type == FlexPageTypeSecure ||
        (instance->page_type == FlexPageTypeAlphanumeric && !strncmp(message, "ALN: ", 5) &&
         flex_text_looks_encrypted(message + 5));
    if(instance->page_encrypted) instance->stats.pages_encrypted++;
    instance->page_duplicate = flex_seen_check(instance, hash);
    // Not fully decoded: a failed header checksum, or a joined message missing
    // one or more fragments.
    instance->page_partial = instance->page_suspect || strstr(message, FLEX_GAP_MARK) != NULL;
    instance->stats.pages_decoded++;
    if(instance->page_chase) instance->stats.pages_chase++;
    if(instance->base.callback) instance->base.callback(&instance->base, instance->base.context);
}

// Emit a joined message and free its slot. Its header shows the number of
// parts and J; FLEX_GAP_MARK marks missing parts.
static void flex_fragment_flush(
    SubGhzProtocolDecoderFlex* instance,
    FlexFragmentSlot* slot,
    bool incomplete) {
    if(incomplete) furi_string_cat_str(slot->text, FLEX_GAP_MARK);
    furi_string_set_str(instance->generic.result_msg, "ALN: ");
    furi_string_cat(instance->generic.result_msg, slot->text);

    instance->page_repaired = slot->repaired;
    instance->page_suspect = slot->suspect;
    // Mark the joined message with W if Weak Decode recovered any of its parts.
    instance->page_chase = slot->chase;
    // Show and filter a joined message by where its first part arrived.
    const uint8_t frame = instance->frame;
    instance->frame = slot->frame;
    flex_set_page_header(instance, slot->capcode, slot->type, slot->parts, 'J', slot->phase);
    flex_emit_page(instance);
    instance->frame = frame;

    furi_string_free(slot->text);
    memset(slot, 0, sizeof(FlexFragmentSlot));
}

static void flex_fragment_flush_expired(SubGhzProtocolDecoderFlex* instance) {
    const uint32_t now = furi_get_tick();
    for(size_t i = 0; i < FLEX_FRAGMENT_SLOTS; i++) {
        FlexFragmentSlot* slot = &instance->fragments[i];
        if(slot->used && now - slot->tick >= furi_ms_to_ticks(FLEX_FRAGMENT_TIMEOUT_MS)) {
            instance->stats.joined_timeout++;
            flex_fragment_flush(instance, slot, true);
        }
    }
}

/*
 * Add one fragment of a long alphanumeric message, given as its text without
 * the "ALN: " prefix. The joined message is emitted when the last fragment
 * arrives; parts that never arrive are marked with FLEX_GAP_MARK.
 */
static void flex_fragment_add(
    SubGhzProtocolDecoderFlex* instance,
    uint32_t capcode,
    uint8_t type,
    uint8_t message_number,
    uint8_t fragment,
    bool continuation,
    const char* text,
    char phase) {
    // Capture per-page state before a flush below replaces it with the state
    // of the older joined message being emitted.
    const bool chase_used = instance->page_chase;
    const bool repaired_used = instance->page_repaired;
    const bool suspect_used = instance->page_suspect;
    if(fragment == 3U) {
        instance->stats.fragments_first++;
    } else if(continuation) {
        instance->stats.fragments_more++;
    } else {
        instance->stats.fragments_last++;
    }

    FlexFragmentSlot* slot = NULL;
    for(size_t i = 0; i < FLEX_FRAGMENT_SLOTS; i++) {
        FlexFragmentSlot* candidate = &instance->fragments[i];
        if(candidate->used && candidate->capcode == capcode && candidate->type == type &&
           candidate->message_number == message_number) {
            slot = candidate;
            break;
        }
    }

    const uint32_t part_hash = flex_text_hash(capcode, type, text);
    const size_t part_length = strlen(text);

    // A retransmitted copy of the most recently accepted fragment must not
    // look like a missing fragment and add a false gap to the message.
    if(slot && slot->last_fragment == fragment &&
       slot->last_continuation == continuation && slot->last_part_hash == part_hash &&
       slot->last_part_length == part_length) {
        slot->chase = slot->chase || chase_used;
        slot->repaired = slot->repaired || repaired_used;
        slot->suspect = slot->suspect || suspect_used;
        slot->tick = furi_get_tick();
        instance->stats.joined_duplicate++;
        return;
    }

    // A final fragment without a buffered start has useful text, but nothing
    // was actually joined. Emit it as a partial continuation so the Partial
    // Msgs setting decides whether it is displayed.
    if(!slot && !continuation && fragment != 3U) {
        instance->stats.joined_orphan++;
        instance->page_suspect = true;
        flex_set_page_header(instance, capcode, type, fragment, 'C', phase);
        flex_emit_page(instance);
        return;
    }

    // A new first fragment ends whatever was being joined for this message.
    if(slot && fragment == 3U) {
        flex_fragment_flush(instance, slot, true);
        slot = NULL;
    }

    if(!slot) {
        // Use a free slot, or flush the oldest to make room.
        slot = &instance->fragments[0];
        for(size_t i = 0; i < FLEX_FRAGMENT_SLOTS; i++) {
            FlexFragmentSlot* candidate = &instance->fragments[i];
            if(!candidate->used) {
                slot = candidate;
                break;
            }
            if(candidate->tick - slot->tick > UINT32_MAX / 2U) slot = candidate;
        }
        if(slot->used) flex_fragment_flush(instance, slot, true);

        slot->used = true;
        slot->capcode = capcode;
        slot->type = type;
        slot->message_number = message_number;
        slot->chase = chase_used;
        slot->repaired = repaired_used;
        slot->suspect = suspect_used;
        slot->text = furi_string_alloc();
        slot->phase = phase;
        slot->frame = instance->frame;
        // A later fragment without its start: the beginning is missing.
        if(fragment != 3U) {
            furi_string_set_str(slot->text, FLEX_GAP_MARK);
            instance->stats.joined_orphan++;
        }
    } else {
        slot->chase = slot->chase || chase_used;
        slot->repaired = slot->repaired || repaired_used;
        slot->suspect = slot->suspect || suspect_used;
        if(fragment != slot->next_fragment) {
            furi_string_cat_str(slot->text, FLEX_GAP_MARK);
            instance->stats.joined_sequence_gap++;
        }
    }

    if(furi_string_size(slot->text) + strlen(text) > FLEX_JOINED_MAX_CHARS) {
        const size_t room = FLEX_JOINED_MAX_CHARS > furi_string_size(slot->text) ?
                                FLEX_JOINED_MAX_CHARS - furi_string_size(slot->text) :
                                0U;
        furi_string_cat_printf(slot->text, "%.*s" FLEX_TRUNCATION_MARK, (int)room, text);
    } else {
        furi_string_cat_str(slot->text, text);
    }
    slot->parts++;
    if(fragment == 3U) {
        slot->signature = instance->alpha_signature;
        slot->signature_present = true;
    }
    slot->signature_sum += instance->alpha_signature_sum;
    slot->next_fragment = fragment == 3U ? 0U : (fragment + 1U) % 3U;
    slot->last_fragment = fragment;
    slot->last_continuation = continuation;
    slot->last_part_hash = part_hash;
    slot->last_part_length = part_length;
    slot->tick = furi_get_tick();

    if(!continuation) {
        if(!slot->signature_present ||
           ((~slot->signature_sum) & 0x7FU) != slot->signature) {
            slot->suspect = true;
            instance->stats.joined_signature_bad++;
        }
        instance->stats.joined_complete++;
        flex_fragment_flush(instance, slot, false);
    }
}

static bool flex_partial_matches(
    const FlexPartialPage* partial,
    uint32_t capcode,
    uint8_t type,
    const uint32_t words[FLEX_PHASE_WORDS],
    const bool valid[FLEX_PHASE_WORDS],
    uint8_t first,
    uint8_t length) {
    if(!partial->used || partial->capcode != capcode || partial->type != type ||
       partial->length != length) {
        return false;
    }
    if(furi_get_tick() - partial->tick >= furi_ms_to_ticks(FLEX_PARTIAL_TIMEOUT_MS)) {
        return false;
    }

    // Another message to the same capcode can share the type and length, so
    // require at least one word both copies decoded, and no disagreement.
    uint8_t agreeing = 0;
    for(uint8_t i = 0; i < length; i++) {
        if(!partial->valid[i] || !valid[first + i]) continue;
        if(flex_word_identity(type, i, partial->words[i]) !=
           flex_word_identity(type, i, words[first + i])) {
            return false;
        }
        agreeing++;
    }
    return agreeing > 0;
}

static FlexPartialPage* flex_find_partial(
    SubGhzProtocolDecoderFlex* instance,
    uint32_t capcode,
    uint8_t type,
    const uint32_t words[FLEX_PHASE_WORDS],
    const bool valid[FLEX_PHASE_WORDS],
    uint8_t first,
    uint8_t length) {
    if(!instance->partial) return NULL;
    for(size_t i = 0; i < FLEX_PARTIAL_PAGES; i++) {
        if(flex_partial_matches(
               &instance->partial[i], capcode, type, words, valid, first, length)) {
            return &instance->partial[i];
        }
    }
    return NULL;
}

static uint32_t flex_majority(uint32_t a, uint32_t b, uint32_t c) {
    return (a & b) | (a & c) | (b & c);
}

// Recover a word that failed BCH in this copy and every earlier one.
static bool flex_recover_word(FlexPartialPage* partial, uint8_t index, uint32_t copy) {
    uint32_t recovered = 0;
    bool ok = false;
    if(partial->copies[index] >= 2U) {
        recovered = flex_majority(partial->previous[index], partial->words[index], copy);
        ok = flex_bch_correct(&recovered) >= 0;
        if(!ok) ok = flex_bch_combine(partial->previous[index], copy, &recovered);
    }
    if(!ok) ok = flex_bch_combine(partial->words[index], copy, &recovered);

    if(ok) {
        partial->words[index] = recovered & 0x001FFFFFUL;
        partial->valid[index] = true;
    } else {
        partial->previous[index] = partial->words[index];
        partial->words[index] = copy;
        if(partial->copies[index] < UINT8_MAX) partial->copies[index]++;
    }
    return ok;
}

/*
 * Merge an incomplete page with earlier copies of it. Pages are often sent
 * more than once, and the copies rarely lose the same words. Returns true when
 * the page is now complete, with its words in merged_words at first..last.
 */
static bool flex_repair_page(
    SubGhzProtocolDecoderFlex* instance,
    uint32_t capcode,
    uint8_t type,
    const uint32_t words[FLEX_PHASE_WORDS],
    const bool valid[FLEX_PHASE_WORDS],
    uint8_t first,
    uint8_t last) {
    const uint8_t length = last - first + 1U;
    if(!instance->partial) instance->partial = calloc(FLEX_PARTIAL_PAGES, sizeof(FlexPartialPage));
    FlexPartialPage* partial =
        flex_find_partial(instance, capcode, type, words, valid, first, length);

    if(!partial) {
        // Reuse an empty or expired slot, otherwise the oldest.
        partial = &instance->partial[0];
        for(size_t i = 0; i < FLEX_PARTIAL_PAGES; i++) {
            FlexPartialPage* slot = &instance->partial[i];
            if(!slot->used ||
               furi_get_tick() - slot->tick >= furi_ms_to_ticks(FLEX_PARTIAL_TIMEOUT_MS)) {
                partial = slot;
                break;
            }
            if(slot->tick - partial->tick > UINT32_MAX / 2U) partial = slot;
        }
        partial->used = true;
        partial->capcode = capcode;
        partial->type = type;
        partial->length = length;
        partial->tick = furi_get_tick();
        for(uint8_t i = 0; i < length; i++) {
            partial->words[i] = words[first + i];
            partial->valid[i] = valid[first + i];
            partial->copies[i] = valid[first + i] ? 0U : 1U;
        }
        return false;
    }

    partial->tick = furi_get_tick();
    bool complete = true;
    for(uint8_t i = 0; i < length; i++) {
        if(partial->valid[i]) continue;
        if(valid[first + i]) {
            partial->words[i] = words[first + i];
            partial->valid[i] = true;
        } else if(!flex_recover_word(partial, i, words[first + i])) {
            complete = false;
        }
    }
    if(!complete) return false;

    memset(instance->merged_valid, 0, sizeof(instance->merged_valid));
    for(uint8_t i = 0; i < length; i++) {
        instance->merged_words[first + i] = partial->words[i];
        instance->merged_valid[first + i] = true;
    }
    partial->used = false;
    return true;
}

// Emits every page in the phase through the decoder callback.
static void flex_decode_phase(
    SubGhzProtocolDecoderFlex* instance,
    uint32_t words[FLEX_PHASE_WORDS],
    char phase) {
    bool valid[FLEX_PHASE_WORDS];
    bool chased[FLEX_PHASE_WORDS];
    const uint8_t phase_index = (uint8_t)((phase - 'A') & 0x03);
    for(uint8_t i = 0; i < FLEX_PHASE_WORDS; i++) {
        chased[i] = false;
        valid[i] = flex_bch_correct(&words[i]) >= 0;
        // Weak Decode: when hard BCH fails, soft-correct using the DPLL vote
        // margins carried for this word. Checksum-gated downstream, so a wrong
        // recovery is dropped at the page level.
        if(!valid[i] && instance->weak_decode && instance->phase_rel) {
            const uint8_t* rel =
                &instance->phase_rel[((phase_index * FLEX_PHASE_WORDS) + i) * FLEX_REL_WORD];
            if(flex_bch_chase(&words[i], rel)) {
                valid[i] = true;
                chased[i] = true;
                instance->stats.words_chase++;
            }
        }
        if(valid[i]) words[i] &= 0x001FFFFFUL;
        instance->stats.words_total++;
        if(!valid[i]) {
            instance->stats.words_bad++;
            instance->stats.words_bad_phase[phase_index]++;
        }
    }

    if(!valid[0]) {
        instance->stats.phases_bad_biw++;
        return;
    }
    const uint32_t biw = words[0];
    if(biw == 0 || biw == 0x001FFFFFUL) return;

    const uint8_t vector_offset = (biw >> 10) & 0x3F;
    const uint8_t address_offset = ((biw >> 8) & 0x03) + 1U;
    if(vector_offset < address_offset ||
       (uint16_t)vector_offset + (uint16_t)(vector_offset - address_offset) >
           FLEX_PHASE_WORDS) {
        return;
    }

    for(uint8_t address_index = address_offset; address_index < vector_offset; address_index++) {
        const uint8_t vector_index = vector_offset + address_index - address_offset;
        if(!valid[address_index] || !valid[vector_index]) {
            instance->stats.pages_bad_header++;
            continue;
        }

        const uint32_t address = words[address_index];
        if(address == 0 || address == 0x001FFFFFUL) continue;

        // A long address spans two address words and their two vector slots;
        // the first slot holds the vector and the second extra data. Range
        // test from multimon-ng's parse_capcode(), capcode formula from PDW.
        const bool long_address = address < 0x008001UL ||
                                  (address > 0x1E0000UL && address < 0x1F0001UL) ||
                                  address > 0x1F7FFEUL;
        uint32_t capcode;
        uint8_t long_data_index = FLEX_NO_WORD;
        if(long_address) {
            const uint8_t second = ++address_index;
            long_data_index = vector_index + 1U;
            if(second >= vector_offset || !valid[second] ||
               long_data_index >= FLEX_PHASE_WORDS) {
                instance->stats.pages_bad_header++;
                continue;
            }
            const uint64_t value = (uint64_t)address +
                                   ((uint64_t)(words[second] ^ 0x001FFFFFUL) << 15) +
                                   0x1F9000ULL;
            if(value > UINT32_MAX) {
                instance->stats.pages_bad_header++;
                continue;
            }
            capcode = (uint32_t)value;
            instance->stats.pages_long_address++;
        } else {
            capcode = address - 0x8000UL;
        }

        const uint32_t vector = words[vector_index];
        if(!flex_word_checksum_valid(vector)) {
            instance->stats.pages_bad_header++;
            continue;
        }
        const uint8_t type = (vector >> 4) & 0x07;
        if(type == FlexPageTypeShortInstruction) {
            instance->stats.pages_short_instruction++;
            continue;
        }

        instance->page_repaired = false;
        instance->page_suspect = false;
        // Weak Decode marker: note whether any header word this page is built
        // from was recovered by Chase. Message words are OR'd in below.
        instance->page_chase =
            instance->frame_chase || chased[vector_index] || chased[address_index];
        if(long_address) {
            instance->page_chase = instance->page_chase || chased[address_index - 1U];
            if(long_data_index < FLEX_PHASE_WORDS) {
                instance->page_chase = instance->page_chase || chased[long_data_index];
            }
        }
        if(type == FlexPageTypeTone) {
            // Tone pages live in the vector word, plus the second vector slot
            // for long addresses.
            const uint32_t* long_data = NULL;
            if(long_address && valid[long_data_index]) long_data = &words[long_data_index];
            if(flex_parse_tone(instance, capcode, type, vector, long_data, phase)) {
                flex_emit_page(instance);
            }
            continue;
        }

        const bool numeric = type == FlexPageTypeStandardNumeric ||
                             type == FlexPageTypeSpecialNumeric ||
                             type == FlexPageTypeNumberedNumeric;
        const bool text = type == FlexPageTypeAlphanumeric || type == FlexPageTypeSecure ||
                          type == FlexPageTypeBinary;
        if(!numeric && !text) continue;

        uint8_t first = (vector >> 7) & 0x7F;
        const uint8_t length = numeric ? ((vector >> 14) & 0x07) + 1U : (vector >> 14) & 0x7F;
        const bool alpha = type == FlexPageTypeAlphanumeric || type == FlexPageTypeSecure;
        // With a long address the alphanumeric header is in the vector field,
        // so the message field is one word shorter than the length says.
        const uint8_t field_words = (alpha && long_address && length) ? length - 1U : length;
        uint16_t last = first + field_words - 1U;
        if(length == 0 || (field_words && last >= FLEX_PHASE_WORDS)) {
            instance->stats.pages_bad_range++;
            continue;
        }

        // OR the message's source words into the Weak Decode marker.
        for(uint16_t i = first; i <= last && i < FLEX_PHASE_WORDS; i++) {
            if(chased[i]) instance->page_chase = true;
        }

        const uint32_t* page_words = words;
        const bool* page_valid = valid;
        if(alpha) {
            flex_gather_alpha_page(
                instance,
                words,
                valid,
                first,
                length,
                long_address ? long_data_index : FLEX_NO_WORD);
            page_words = instance->page_words;
            page_valid = instance->page_valid;
            first = 0;
            last = length - 1U;
        }

        if(numeric && long_address) {
            // Its words are not one contiguous range, so it cannot be repaired;
            // the parser checks each word it reads.
        } else if(flex_word_range_valid(page_valid, first, (uint8_t)last)) {
            // A clean copy makes any partial copy of the same page redundant.
            FlexPartialPage* partial = flex_find_partial(
                instance, capcode, type, page_words, page_valid, first, length);
            if(partial) partial->used = false;
        } else {
            instance->stats.pages_incomplete++;
            if(!flex_repair_page(
                   instance, capcode, type, page_words, page_valid, first, (uint8_t)last)) {
                continue;
            }
            page_words = instance->merged_words;
            page_valid = instance->merged_valid;
            instance->page_repaired = true;
            instance->stats.pages_repaired++;
        }

        // Every word passed BCH, but BCH can "correct" a badly damaged word to
        // the wrong value. The header checksum covers the whole message: drop
        // a repaired page that fails it (its copies did not really agree) and
        // mark a received one as suspect.
        const bool alpha_checksum_valid =
            !alpha || flex_alpha_checksum_valid(page_words, first, (uint8_t)last);
        if(!alpha_checksum_valid) {
            instance->stats.pages_checksum_bad++;
            if(instance->page_repaired) continue;
            instance->page_suspect = true;
        }

        bool parsed;
        if(numeric) {
            parsed = flex_parse_numeric(
                instance, page_words, page_valid, capcode, type, vector, long_data_index, phase);
        } else if(type == FlexPageTypeBinary) {
            parsed = flex_parse_binary(
                instance, page_words, page_valid, capcode, type, first, (uint8_t)last, phase);
        } else {
            parsed = flex_parse_alphanumeric(
                instance, page_words, page_valid, capcode, type, first, (uint8_t)last, phase);
        }
        if(parsed && alpha) {
            const uint32_t header = page_words[first];
            const bool continuation = (header >> 10) & 0x01U;
            const uint8_t fragment = (header >> 11) & 0x03U;
            if(!alpha_checksum_valid) {
                // The fragment controls share this failed checksum. Preserve
                // the available text as a partial page, but do not let an
                // untrusted C/F/N field create, alter, or finish a join.
                if(continuation || fragment != 3U) {
                    instance->stats.joined_checksum_rejected++;
                }
                flex_emit_page(instance);
            } else if(continuation || fragment != 3U) {
                // Part of a longer message: join before emitting.
                FuriString* part =
                    furi_string_alloc_set_str(furi_string_get_cstr(instance->generic.result_msg) + 5);
                flex_fragment_add(
                    instance,
                    capcode,
                    type,
                    (header >> 13) & 0x3FU,
                    fragment,
                    continuation,
                    furi_string_get_cstr(part),
                    phase);
                furi_string_free(part);
            } else {
                flex_emit_page(instance);
            }
        } else if(parsed) {
            flex_emit_page(instance);
        } else {
            instance->stats.pages_parse_failed++;
        }
    }
}

static void flex_decode_data(SubGhzProtocolDecoderFlex* instance) {
    // Copy the frame mode so the phase selection survives a callback that
    // resets the decoder.
    const uint16_t baud = instance->baud;
    const uint8_t levels = instance->levels;
    flex_decode_phase(instance, instance->phase_a, 'A');
    if(levels == 4U) flex_decode_phase(instance, instance->phase_b, 'B');
    if(baud == 3200U) flex_decode_phase(instance, instance->phase_c, 'C');
    if(baud == 3200U && levels == 4U) flex_decode_phase(instance, instance->phase_d, 'D');
    flex_fragment_flush_expired(instance);
}

// Shift a reliability into the word's array to mirror the word's >>1 bit
// accumulation, so entry [p] tracks the bit that lands at position p.
static void flex_rel_push(
    SubGhzProtocolDecoderFlex* instance,
    uint8_t phase,
    uint16_t index,
    uint8_t rel) {
    if(!instance->phase_rel) return;
    uint8_t* w = &instance->phase_rel[((phase * FLEX_PHASE_WORDS) + index) * FLEX_REL_WORD];
    memmove(w, w + 1, FLEX_REL_WORD - 1U);
    w[FLEX_REL_WORD - 1U] = rel;
}

static void flex_fiw_rel_push(SubGhzProtocolDecoderFlex* instance, uint8_t rel) {
    memmove(instance->fiw_rel, instance->fiw_rel + 1, sizeof(instance->fiw_rel) - 1U);
    instance->fiw_rel[sizeof(instance->fiw_rel) - 1U] = rel;
}

static void flex_read_data_symbol(SubGhzProtocolDecoderFlex* instance, uint8_t symbol) {
    const uint16_t index =
        ((instance->data_bit_count >> 5) & 0xFFF8U) | (instance->data_bit_count & 0x0007U);
    if(index >= FLEX_PHASE_WORDS) return;

    const bool bit_a = symbol > 1U;
    const bool bit_b = symbol == 1U || symbol == 2U;
    if(instance->baud == 1600U || !instance->phase_toggle) {
        instance->phase_a[index] =
            (instance->phase_a[index] >> 1) | (bit_a ? 0x80000000UL : 0);
        flex_rel_push(instance, 0, index, instance->cur_rel_a);
        if(instance->levels == 4U) {
            instance->phase_b[index] =
                (instance->phase_b[index] >> 1) | (bit_b ? 0x80000000UL : 0);
            flex_rel_push(instance, 1, index, instance->cur_rel_b);
        }
        instance->phase_toggle = instance->baud == 3200U;
        if(instance->baud == 1600U) instance->data_bit_count++;
    } else {
        instance->phase_c[index] =
            (instance->phase_c[index] >> 1) | (bit_a ? 0x80000000UL : 0);
        flex_rel_push(instance, 2, index, instance->cur_rel_a);
        if(instance->levels == 4U) {
            instance->phase_d[index] =
                (instance->phase_d[index] >> 1) | (bit_b ? 0x80000000UL : 0);
            flex_rel_push(instance, 3, index, instance->cur_rel_b);
        }
        instance->phase_toggle = false;
        instance->data_bit_count++;
    }
}

static void flex_process_symbol(SubGhzProtocolDecoderFlex* instance, uint8_t symbol) {
    if(instance->synth_enable) {
        // One tone per recovered symbol while locked; silent while hunting.
        if(instance->state != FlexDecoderStateSync1) {
            if(symbol != instance->synth_level) {
                instance->synth_level = symbol;
                furi_hal_speaker_start(instance->synth_tones[symbol & 3U], instance->synth_volume);
            }
        } else if(instance->synth_level != 0xFFU) {
            instance->synth_level = 0xFFU;
            furi_hal_speaker_start(instance->synth_tones[0], 0.0f);
        }
    }

    if(instance->state == FlexDecoderStateSync1) {
        instance->sync_buffer = (instance->sync_buffer << 1) | (symbol < 2U ? 1U : 0U);
        uint16_t sync_code = flex_sync_check(instance->sync_buffer);
        bool inverted = false;
        if(sync_code == 0) {
            sync_code = flex_sync_check(~instance->sync_buffer);
            inverted = sync_code != 0;
        }

        if(sync_code != 0 && flex_select_mode(instance, sync_code)) {
            instance->stats.sync_detected++;
            instance->inverted = inverted;
            instance->fiw_count = 0;
            instance->fiw_raw = 0;
            memset(instance->fiw_rel, FLEX_REL_HIGH, sizeof(instance->fiw_rel));
            instance->frame_chase = false;
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

    const uint8_t rectified_symbol = instance->inverted ? 3U - symbol : symbol;
    const bool bit = rectified_symbol > 1U;
    switch(instance->state) {
    case FlexDecoderStateFiw:
        instance->fiw_count++;
        if(instance->fiw_count > 16U) {
            instance->fiw_raw = (instance->fiw_raw >> 1) | (bit ? 0x80000000UL : 0);
            flex_fiw_rel_push(instance, instance->cur_rel_a);
        }
        if(instance->fiw_count == 48U) {
            if(flex_decode_fiw(instance)) {
                instance->stats.fiw_valid++;
                instance->sync2_count = 0;
                instance->state = FlexDecoderStateSync2;
            } else {
                instance->stats.fiw_invalid++;
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
            memset(instance->phase_b, 0, sizeof(instance->phase_b));
            memset(instance->phase_c, 0, sizeof(instance->phase_c));
            memset(instance->phase_d, 0, sizeof(instance->phase_d));
            if(instance->phase_rel) memset(instance->phase_rel, FLEX_REL_HIGH, FLEX_REL_SIZE);
            instance->state = FlexDecoderStateData;
        }
        break;

    case FlexDecoderStateData:
        flex_read_data_symbol(instance, rectified_symbol);
        instance->data_count++;
        if(instance->data_count >= (instance->baud * FLEX_DATA_DURATION_MS) / 1000U) {
            flex_decode_data(instance);
            instance->stats.frames_completed++;
            instance->state = FlexDecoderStateSync1;
            instance->sync_buffer = 0;
            instance->data_count = 0;
        }
        break;

    case FlexDecoderStateSync1:
        break;
    }
}

void subghz_protocol_decoder_flex_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    instance->four_level_path = false;

    int32_t remaining_half_us = (int32_t)duration * 2;
    uint16_t processed = 0;
    while(processed < FLEX_MAX_RUN_SYMBOLS) {
        const uint16_t symbol_period =
            (instance->state == FlexDecoderStateSync1 ||
             instance->state == FlexDecoderStateFiw || instance->baud == 1600U) ?
                1250U :
                625U;
        if(remaining_half_us < (int32_t)(symbol_period / 2U)) break;

        flex_process_symbol(instance, level ? 3U : 0U);
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

void subghz_protocol_decoder_flex_feed_4fsk_sample(void* context, uint8_t hard_symbol) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    static const uint8_t hard_symbol_to_level[] = {1U, 0U, 2U, 3U};

    /*
     * The phase tracking and modal sampling are reduced, hard-decision versions
     * of the FLEX demodulators used by multimon-ng and PortaPack Mayhem:
     * https://github.com/EliasOenal/multimon-ng/blob/master/demod_flex.c
     * https://github.com/portapack-mayhem/mayhem-firmware/blob/next/firmware/baseband/proc_flex.cpp
     */

    const uint8_t level = hard_symbol_to_level[hard_symbol & 0x03U];
    instance->four_level_path = true;
    if(!instance->clock_seen) {
        instance->clock_last_level = level;
        instance->clock_seen = true;
        return;
    }

    // Sync 1 and the FIW are always 1600 baud. The rate changes inside
    // flex_process_symbol(), at the decision point, so rescale what is left of
    // the current symbol to the new rate to keep the next boundary in place.
    const uint16_t baud =
        (instance->state == FlexDecoderStateSync1 || instance->state == FlexDecoderStateFiw) ?
            FLEX_SYNC_BAUD :
            instance->baud;
    if(baud != instance->clock_baud) {
        if(instance->clock_baud != 0U) {
            uint32_t remaining = 0x10000UL - instance->clock_phase;
            remaining = (remaining * baud) / instance->clock_baud;
            if(remaining > 0x10000UL) remaining = 0x10000UL;
            instance->clock_phase = (uint16_t)(0x10000UL - remaining);
        }
        instance->clock_baud = baud;
    }

    const uint16_t step = (uint16_t)(((uint32_t)baud << 16) / FLEX_4FSK_SAMPLE_RATE_HZ);
    const uint16_t previous = instance->clock_phase;
    uint16_t phase = previous + step;

    // RX_HARD_DATA has no clock on the two available CC1101 pins. A level change
    // places a symbol boundary halfway between this sample and the last one.
    // Pull the phase 1/8 of the way toward it. While hunting for Sync 1 every
    // change counts so the clock can pull in from any phase; once framed only
    // changes near the boundary count so noisy decisions mid-symbol cannot drag
    // it. A larger gain lets single glitches move the phase far enough that the
    // doubled error after the switch to 3200 baud lands outside the window.
    if(level != instance->clock_last_level) {
        const int16_t error = (int16_t)(uint16_t)(phase - step / 2U);
        if(instance->state == FlexDecoderStateSync1 ||
           (error > -(int16_t)FLEX_CLOCK_WINDOW && error < (int16_t)FLEX_CLOCK_WINDOW)) {
            phase = (uint16_t)(phase - error / 8);
        }
        instance->clock_last_level = level;
    }
    instance->clock_phase = phase;

    // Corrections can step the phase backward past the decision point, so a
    // flag rather than the crossing alone limits it to one decision per symbol.
    if(phase < FLEX_CLOCK_QUARTER) instance->clock_decided = false;

    const int16_t advance = (int16_t)(uint16_t)(phase - previous);
    const uint16_t to_center = (uint16_t)(FLEX_CLOCK_CENTER - previous);
    const uint16_t to_decision = (uint16_t)(FLEX_CLOCK_DECISION - previous);
    if(advance > 0 && to_center != 0 && to_center <= (uint16_t)advance) {
        instance->clock_center_level = level;
    }

    if(phase >= FLEX_CLOCK_QUARTER && phase < FLEX_CLOCK_DECISION) {
        instance->clock_votes[level]++;
    }

    if(!instance->clock_decided && advance > 0 && to_decision != 0 &&
       to_decision <= (uint16_t)advance) {
        // Decide the A and B bit streams independently. They are separately
        // BCH-coded, so two adjacent noisy levels can jointly outvote the
        // modal level even when neither is individually the most common.
        const int16_t va = (int16_t)(instance->clock_votes[2] + instance->clock_votes[3]) -
                           (int16_t)(instance->clock_votes[0] + instance->clock_votes[1]);
        const int16_t vb = (int16_t)(instance->clock_votes[1] + instance->clock_votes[2]) -
                           (int16_t)(instance->clock_votes[0] + instance->clock_votes[3]);
        const bool center_a = instance->clock_center_level > 1U;
        const bool center_b =
            instance->clock_center_level == 1U || instance->clock_center_level == 2U;
        const bool bit_a = va == 0 ? center_a : va > 0;
        const bool bit_b = vb == 0 ? center_b : vb > 0;
        const uint8_t voted_level = bit_a ? (bit_b ? 2U : 3U) : (bit_b ? 1U : 0U);
        if(instance->weak_decode) {
            // Per-bit reliability for Weak Decode: the vote margin of each bit
            // the symbol carries (a = levels 2,3; b = levels 1,2).
            instance->cur_rel_a = (uint8_t)MIN((va < 0 ? -va : va), (int16_t)FLEX_REL_HIGH);
            instance->cur_rel_b = (uint8_t)MIN((vb < 0 ? -vb : vb), (int16_t)FLEX_REL_HIGH);
        }
        memset(instance->clock_votes, 0, sizeof(instance->clock_votes));
        instance->clock_decided = true;
        flex_process_symbol(instance, voted_level);
    }
}

void subghz_protocol_decoder_flex_reset_stats(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    memset(&instance->stats, 0, sizeof(instance->stats));
}

void subghz_protocol_decoder_flex_get_page_info(void* context, FlexPageInfo* info) {
    furi_assert(context);
    furi_assert(info);
    const SubGhzProtocolDecoderFlex* instance = context;
    info->capcode = instance->capcode;
    info->duplicate = instance->page_duplicate;
    info->repaired = instance->page_repaired;
    info->encrypted = instance->page_encrypted;
    info->partial = instance->page_partial;
    info->chase = instance->page_chase;
    info->phase = instance->page_phase;
    info->frame = instance->page_frame;
    switch(instance->page_type) {
    case FlexPageTypeTone:
        info->kind = FlexPageKindTone;
        break;
    case FlexPageTypeStandardNumeric:
    case FlexPageTypeSpecialNumeric:
    case FlexPageTypeNumberedNumeric:
        info->kind = FlexPageKindNumeric;
        break;
    case FlexPageTypeBinary:
        info->kind = FlexPageKindBinary;
        break;
    default:
        info->kind = FlexPageKindAlpha;
        break;
    }
}

void subghz_protocol_decoder_flex_get_stats(void* context, FlexDecoderStats* stats) {
    furi_assert(context);
    furi_assert(stats);
    const SubGhzProtocolDecoderFlex* instance = context;
    *stats = instance->stats;
}

bool subghz_protocol_decoder_flex_is_locked(void* context) {
    furi_assert(context);
    const SubGhzProtocolDecoderFlex* instance = context;
    // True once sync is found and the frame is being read (not hunting noise).
    return instance->state != FlexDecoderStateSync1;
}

void subghz_protocol_decoder_flex_set_synth(
    void* context,
    const float tones[4],
    float volume,
    bool enable) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    if(tones) memcpy(instance->synth_tones, tones, sizeof(instance->synth_tones));
    instance->synth_volume = volume;
    instance->synth_level = 0xFFU;
    instance->synth_enable = enable;
}

void subghz_protocol_decoder_flex_set_weak_decode(void* context, bool enable) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    if(enable && !instance->phase_rel) {
        // Allocated only while Weak Decode is on, so it costs nothing when off.
        instance->phase_rel = malloc(FLEX_REL_SIZE);
        if(instance->phase_rel) {
            memset(instance->phase_rel, FLEX_REL_HIGH, FLEX_REL_SIZE);
        } else {
            FURI_LOG_E(TAG, "Weak Decode allocation failed");
        }
    } else if(!enable && instance->phase_rel) {
        free(instance->phase_rel);
        instance->phase_rel = NULL;
    }
    instance->weak_decode = enable && instance->phase_rel;
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
    value = instance->levels;
    if(!flipper_format_write_uint32(flipper_format, "FlexLevels", &value, 1)) {
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
        if(value == 1600U || value == 3200U) instance->baud = value;
    }
    if(flipper_format_read_uint32(flipper_format, "FlexCycle", &value, 1)) {
        instance->cycle = value;
    }
    if(flipper_format_read_uint32(flipper_format, "FlexFrame", &value, 1)) {
        instance->frame = value;
    }
    if(flipper_format_read_uint32(flipper_format, "FlexSequence", &value, 1)) {
        if(value <= 3U) instance->fragment = value;
    }
    if(flipper_format_read_uint32(flipper_format, "FlexLevels", &value, 1)) {
        if(value == 2U || value == 4U) instance->levels = value;
    }
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_flex_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderFlex* instance = context;
    furi_string_cat_printf(
        output,
        "FLEX %u/%u %02u.%03u %c\r\nCAP: %09" PRIu32 "\r\n",
        flex_bit_rate(instance),
        instance->levels,
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
