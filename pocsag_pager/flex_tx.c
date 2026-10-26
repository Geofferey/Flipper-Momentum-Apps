#include "flex_tx.h"

#include <furi.h>
#include <furi_hal.h>

#define FLEX_TX_BCH_POLYNOMIAL 0x769U
#define FLEX_TX_DATA_MASK      0x1FFFFFU
#define FLEX_TX_PHASE_WORDS    88U
#define FLEX_TX_ERS_CYCLES     8U
#define FLEX_TX_FRAME_BYTES    379U
#define FLEX_TX_ERS_BYTES      (FLEX_TX_ERS_CYCLES * 12U)
#define FLEX_TX_COPY_BYTES     (FLEX_TX_ERS_BYTES + FLEX_TX_FRAME_BYTES)
#define FLEX_TX_BUFFER_BYTES   (2U * FLEX_TX_COPY_BYTES)

struct FlexTxEncoder {
    uint8_t bytes[FLEX_TX_BUFFER_BYTES];
    size_t byte_count;
    size_t bit_index;
    bool inverted;
    bool lead_in_pending;
    bool tail_pending;
};

/*
 * FLEX framing is implemented from the word layouts already consumed by the
 * local decoder and cross-checked against PortaPack Mayhem's FLEX TX. See the
 * exact references in README.md.
 */
static uint32_t flex_tx_reverse_bits(uint32_t value) {
    value = ((value >> 1) & 0x55555555U) | ((value & 0x55555555U) << 1);
    value = ((value >> 2) & 0x33333333U) | ((value & 0x33333333U) << 2);
    value = ((value >> 4) & 0x0F0F0F0FU) | ((value & 0x0F0F0F0FU) << 4);
    value = ((value >> 8) & 0x00FF00FFU) | ((value & 0x00FF00FFU) << 8);
    return (value >> 16) | (value << 16);
}

static uint32_t flex_tx_encode_word(uint32_t data21) {
    const uint32_t reversed = flex_tx_reverse_bits(data21) >> 11;
    uint32_t remainder = reversed << 10;
    for(int8_t bit = 30; bit >= 10; bit--) {
        if(remainder & (1UL << bit)) {
            remainder ^= FLEX_TX_BCH_POLYNOMIAL << (bit - 10);
        }
    }

    uint32_t codeword = ((reversed << 10) | (remainder & 0x3FFU)) << 1;
    if(__builtin_popcount(codeword) & 1U) codeword |= 1U;
    return codeword;
}

static uint32_t flex_tx_checksum(uint32_t data21) {
    uint32_t sum = (data21 & 0x0FU) + ((data21 >> 4) & 0x0FU) +
                   ((data21 >> 8) & 0x0FU) + ((data21 >> 12) & 0x0FU) +
                   ((data21 >> 16) & 0x0FU) + ((data21 >> 20) & 0x01U);
    return (data21 & ~0x0FU) | ((0x0FU - (sum & 0x0FU)) & 0x0FU);
}

static uint32_t flex_tx_fiw(uint8_t cycle, uint8_t frame) {
    uint32_t word = ((uint32_t)(cycle & 0x0FU) << 4) |
                    ((uint32_t)(frame & 0x7FU) << 8);
    return flex_tx_encode_word(flex_tx_checksum(word));
}

static uint32_t flex_tx_biw1(uint8_t address_start, uint8_t vector_start) {
    uint32_t word = ((uint32_t)(address_start - 1U) & 0x03U) << 8;
    word |= ((uint32_t)vector_start & 0x3FU) << 10;
    return flex_tx_encode_word(flex_tx_checksum(word));
}

static uint32_t flex_tx_short_address(uint32_t capcode) {
    return flex_tx_encode_word((capcode + 0x8000U) & FLEX_TX_DATA_MASK);
}

static uint32_t flex_tx_alpha_vector(uint8_t message_start, uint8_t message_words) {
    uint32_t word = 5U << 4;
    word |= ((uint32_t)message_start & 0x7FU) << 7;
    word |= ((uint32_t)message_words & 0x7FU) << 14;
    return flex_tx_encode_word(flex_tx_checksum(word));
}

static uint8_t flex_tx_encode_alpha(
    const char* message,
    size_t length,
    uint8_t message_number,
    bool retrieval,
    uint32_t* output,
    uint8_t capacity) {
    if(capacity < 2U) return 0;

    uint8_t count = 0;
    output[count++] = (3U << 11) | ((uint32_t)(message_number & 0x3FU) << 13) |
                      ((uint32_t)retrieval << 19);

    size_t character = 0;
    uint32_t word = 0;
    uint8_t value = character < length ? (uint8_t)message[character++] & 0x7FU : 0x03U;
    word |= (uint32_t)value << 7;
    value = character < length ? (uint8_t)message[character++] & 0x7FU : 0x03U;
    word |= (uint32_t)value << 14;
    output[count++] = word;

    while(character < length && count < capacity) {
        word = 0;
        for(uint8_t slot = 0; slot < 3U; slot++) {
            value = character < length ? (uint8_t)message[character++] & 0x7FU : 0x03U;
            word |= (uint32_t)value << (slot * 7U);
        }
        output[count++] = word;
    }
    if(character < length) return 0;

    uint32_t signature = 0;
    for(uint8_t i = 1; i < count; i++) {
        for(uint8_t slot = 0; slot < 3U; slot++) {
            const uint8_t character_value = (output[i] >> (slot * 7U)) & 0x7FU;
            if(character_value != 0x03U) signature += character_value;
        }
    }
    output[1] = (output[1] & ~0x7FU) | ((~signature) & 0x7FU);

    uint32_t checksum = 0;
    for(uint8_t i = 0; i < count; i++) {
        checksum += output[i] & 0xFFU;
        checksum += (output[i] >> 8) & 0xFFU;
        checksum += (output[i] >> 16) & 0x1FU;
    }
    output[0] |= (~checksum) & 0x3FFU;

    for(uint8_t i = 0; i < count; i++) output[i] = flex_tx_encode_word(output[i]);
    return count;
}

static void flex_tx_write_word(uint8_t** output, uint32_t word) {
    *(*output)++ = (word >> 24) & 0xFFU;
    *(*output)++ = (word >> 16) & 0xFFU;
    *(*output)++ = (word >> 8) & 0xFFU;
    *(*output)++ = word & 0xFFU;
}

static void flex_tx_write_ers(uint8_t** output) {
    static const uint8_t ers_code[] = {0xCB, 0x20, 0x59, 0x39};
    for(uint8_t cycle = 0; cycle < FLEX_TX_ERS_CYCLES; cycle++) {
        *(*output)++ = 0xAA;
        *(*output)++ = 0xAA;
        for(size_t i = 0; i < COUNT_OF(ers_code); i++) *(*output)++ = ers_code[i];
        *(*output)++ = 0x55;
        *(*output)++ = 0x55;
        for(size_t i = 0; i < COUNT_OF(ers_code); i++) *(*output)++ = ~ers_code[i];
    }
}

static void flex_tx_write_interleaved(uint8_t** output, const uint32_t words[8]) {
    for(uint8_t bit = 0; bit < 32U; bit++) {
        uint8_t byte = 0;
        for(uint8_t word = 0; word < 8U; word++) {
            byte |= ((words[word] >> (31U - bit)) & 1U) << (7U - word);
        }
        *(*output)++ = byte;
    }
}

static void flex_tx_write_frame(
    uint8_t** output,
    const PocsagTxConfig* config,
    uint8_t cycle,
    uint8_t frame,
    uint8_t message_number,
    bool retrieval) {
    static const uint8_t sync1[] = {
        0xAA, 0xAA, 0xAA, 0xAA, 0x78, 0xF3, 0x59,
        0x39, 0x55, 0x55, 0x87, 0x0C, 0xA6, 0xC6,
    };
    static const uint8_t sync2[] = {0xAE, 0xD8, 0x45, 0x12, 0x7B};
    for(size_t i = 0; i < COUNT_OF(sync1); i++) *(*output)++ = sync1[i];
    flex_tx_write_word(output, flex_tx_fiw(cycle, frame));
    for(size_t i = 0; i < COUNT_OF(sync2); i++) *(*output)++ = sync2[i];

    uint32_t message_words[32];
    const size_t message_length = strlen(config->message);
    const uint8_t message_word_count = flex_tx_encode_alpha(
        config->message,
        message_length,
        message_number,
        retrieval,
        message_words,
        COUNT_OF(message_words));

    uint32_t frame_words[FLEX_TX_PHASE_WORDS];
    for(uint8_t i = 0; i < FLEX_TX_PHASE_WORDS; i++) {
        frame_words[i] = (i & 1U) ? 0x00000000U : 0xFFFFFFFFU;
    }
    const uint8_t address_start = 1U;
    const uint8_t vector_start = 2U;
    const uint8_t message_start = 3U;
    frame_words[0] = flex_tx_biw1(address_start, vector_start);
    frame_words[address_start] = flex_tx_short_address(config->ric);
    frame_words[vector_start] = flex_tx_alpha_vector(message_start, message_word_count);
    for(uint8_t i = 0; i < message_word_count; i++) {
        frame_words[message_start + i] = message_words[i];
    }

    for(uint8_t block = 0; block < 11U; block++) {
        flex_tx_write_interleaved(output, &frame_words[block * 8U]);
    }
    for(uint8_t i = 0; i < 4U; i++) *(*output)++ = 0xAA;
}

FlexTxEncoder* flex_tx_encoder_alloc(void) {
    return calloc(1, sizeof(FlexTxEncoder));
}

void flex_tx_encoder_free(FlexTxEncoder* encoder) {
    free(encoder);
}

bool flex_tx_encoder_prepare(FlexTxEncoder* encoder, const PocsagTxConfig* config) {
    furi_assert(encoder);
    furi_assert(config);

    const size_t message_length = strnlen(config->message, sizeof(config->message));
    if(config->protocol != PagerTxProtocolFlex || message_length == 0U ||
       message_length > PCSG_TX_MAX_MESSAGE_LENGTH || config->ric < FLEX_TX_SHORT_CAPCODE_MIN ||
       config->ric > FLEX_TX_SHORT_CAPCODE_MAX) {
        return false;
    }

    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);
    const uint8_t cycle = datetime.minute / 4U;
    const uint8_t frame = (config->ric / 16U) % 128U;
    uint8_t message_number =
        (datetime.second + datetime.minute * 7U + datetime.hour) & 0x3FU;
    if(message_number == 0U) message_number = 1U;

    uint8_t* output = encoder->bytes;
    flex_tx_write_ers(&output);
    flex_tx_write_frame(&output, config, cycle, frame, message_number, true);
    flex_tx_write_ers(&output);
    flex_tx_write_frame(&output, config, cycle, frame, message_number, false);
    encoder->byte_count = output - encoder->bytes;
    furi_check(encoder->byte_count == sizeof(encoder->bytes));

    encoder->bit_index = 0;
    encoder->inverted = config->inverted;
    encoder->lead_in_pending = !config->inverted;
    encoder->tail_pending = true;
    return true;
}

static bool flex_tx_wire_bit(const FlexTxEncoder* encoder, size_t bit_index) {
    return (encoder->bytes[bit_index / 8U] >> (7U - (bit_index % 8U))) & 1U;
}

LevelDuration flex_tx_encoder_yield(void* context) {
    furi_assert(context);
    FlexTxEncoder* encoder = context;
    if(encoder->lead_in_pending) {
        encoder->lead_in_pending = false;
        return level_duration_make(true, 1000000U / FLEX_TX_BAUD);
    }

    const size_t total_bits = encoder->byte_count * 8U;
    if(encoder->bit_index >= total_bits) {
        if(encoder->tail_pending) {
            encoder->tail_pending = false;
            return level_duration_make(!encoder->inverted, 1000000U / FLEX_TX_BAUD);
        }
        return level_duration_reset();
    }

    const bool wire_bit = flex_tx_wire_bit(encoder, encoder->bit_index);
    size_t run_bits = 1U;
    while(encoder->bit_index + run_bits < total_bits &&
          flex_tx_wire_bit(encoder, encoder->bit_index + run_bits) == wire_bit) {
        run_bits++;
    }
    encoder->bit_index += run_bits;

    const bool async_level = encoder->inverted ? wire_bit : !wire_bit;
    return level_duration_make(async_level, (1000000U / FLEX_TX_BAUD) * run_bits);
}
