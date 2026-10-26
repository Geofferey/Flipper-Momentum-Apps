#include "pocsag_tx.h"

#include <furi.h>

#define POCSAG_SYNC_WORD       0x7CD215D8U
#define POCSAG_IDLE_WORD       0x7A89C197U
#define POCSAG_PREAMBLE_WORD   0xAAAAAAAAU
#define POCSAG_PREAMBLE_WORDS  18U
#define POCSAG_BATCH_WORDS     16U
#define POCSAG_MAX_BATCHES     3U
#define POCSAG_MAX_WIRE_WORDS  (POCSAG_PREAMBLE_WORDS + POCSAG_MAX_BATCHES * 17U)
#define POCSAG_BCH_POLYNOMIAL  0x769U

struct PocsagTxEncoder {
    uint32_t words[POCSAG_MAX_WIRE_WORDS];
    size_t word_count;
    size_t bit_index;
    uint32_t bit_duration_us;
    bool inverted;
    bool lead_in_pending;
    bool tail_pending;
};

/*
 * Protocol structure follows ITU-R M.584-2. Encoding was independently
 * implemented and cross-checked against DAPNET UniPager, MMDVMHost and the
 * faithanalog POCSAG encoder; see README.md for exact references.
 */
static uint32_t pocsag_tx_encode_codeword(uint32_t data21) {
    uint32_t work = data21 << 10;

    for(int8_t bit = 30; bit >= 10; bit--) {
        if(work & (1UL << bit)) {
            work ^= POCSAG_BCH_POLYNOMIAL << (bit - 10);
        }
    }

    uint32_t codeword = ((data21 << 10) | (work & 0x3FFU)) << 1;
    if(__builtin_popcount(codeword) & 1U) codeword |= 1U;
    return codeword;
}

PocsagTxEncoder* pocsag_tx_encoder_alloc(void) {
    return calloc(1, sizeof(PocsagTxEncoder));
}

void pocsag_tx_encoder_free(PocsagTxEncoder* encoder) {
    free(encoder);
}

bool pocsag_tx_encoder_prepare(PocsagTxEncoder* encoder, const PocsagTxConfig* config) {
    furi_assert(encoder);
    furi_assert(config);

    const size_t message_length = strnlen(config->message, sizeof(config->message));
    if(config->protocol != PagerTxProtocolPocsag || message_length == 0U ||
       message_length > PCSG_TX_MAX_MESSAGE_LENGTH ||
       config->ric > PCSG_TX_RIC_MAX || config->function > 3U ||
       (config->baud != 512U && config->baud != 1200U && config->baud != 2400U)) {
        return false;
    }

    const size_t address_offset = (config->ric & 7U) * 2U;
    const size_t message_words = (message_length * 7U + 19U) / 20U;
    const size_t used_data_words = address_offset + 1U + message_words + 1U;
    const size_t batch_count = (used_data_words + POCSAG_BATCH_WORDS - 1U) /
                               POCSAG_BATCH_WORDS;
    if(batch_count == 0U || batch_count > POCSAG_MAX_BATCHES) return false;

    uint32_t data_words[POCSAG_MAX_BATCHES * POCSAG_BATCH_WORDS];
    for(size_t i = 0; i < COUNT_OF(data_words); i++) data_words[i] = POCSAG_IDLE_WORD;

    const uint32_t address_data = ((config->ric >> 3) << 2) | config->function;
    data_words[address_offset] = pocsag_tx_encode_codeword(address_data);

    for(size_t word = 0; word < message_words; word++) {
        uint32_t payload = 0;
        for(size_t bit = 0; bit < 20U; bit++) {
            const size_t stream_bit = word * 20U + bit;
            if(stream_bit < message_length * 7U) {
                const uint8_t character = config->message[stream_bit / 7U] & 0x7FU;
                const bool value = (character >> (stream_bit % 7U)) & 1U;
                payload |= (uint32_t)value << (19U - bit);
            }
        }
        data_words[address_offset + 1U + word] =
            pocsag_tx_encode_codeword((1UL << 20) | payload);
    }

    size_t output = 0;
    for(size_t i = 0; i < POCSAG_PREAMBLE_WORDS; i++) {
        encoder->words[output++] = POCSAG_PREAMBLE_WORD;
    }
    for(size_t batch = 0; batch < batch_count; batch++) {
        encoder->words[output++] = POCSAG_SYNC_WORD;
        for(size_t word = 0; word < POCSAG_BATCH_WORDS; word++) {
            encoder->words[output++] = data_words[batch * POCSAG_BATCH_WORDS + word];
        }
    }

    encoder->word_count = output;
    encoder->bit_index = 0;
    encoder->bit_duration_us = (1000000U + config->baud / 2U) / config->baud;
    encoder->inverted = config->inverted;
    /*
     * Flipper's asynchronous TX timer starts at a high GPIO level and drops
     * leading low callback entries. Standard polarity would therefore lose
     * the first preamble bit. Prepend the opposite alternating bit so all 576
     * encoded preamble bits make it on air after the timer starts.
     */
    encoder->lead_in_pending = !config->inverted;
    encoder->tail_pending = true;
    return true;
}

static bool pocsag_tx_encoder_wire_bit(const PocsagTxEncoder* encoder, size_t bit_index) {
    const uint32_t word = encoder->words[bit_index / 32U];
    return (word >> (31U - (bit_index % 32U))) & 1U;
}

LevelDuration pocsag_tx_encoder_yield(void* context) {
    furi_assert(context);
    PocsagTxEncoder* encoder = context;
    if(encoder->lead_in_pending) {
        encoder->lead_in_pending = false;
        return level_duration_make(true, encoder->bit_duration_us);
    }

    const size_t total_bits = encoder->word_count * 32U;
    if(encoder->bit_index >= total_bits) {
        if(encoder->tail_pending) {
            encoder->tail_pending = false;
            /* Force a transition so the timer emits the final low-level run. */
            return level_duration_make(!encoder->inverted, encoder->bit_duration_us);
        }
        return level_duration_reset();
    }

    const bool wire_bit = pocsag_tx_encoder_wire_bit(encoder, encoder->bit_index);
    size_t run_bits = 1U;
    while(encoder->bit_index + run_bits < total_bits &&
          pocsag_tx_encoder_wire_bit(encoder, encoder->bit_index + run_bits) == wire_bit) {
        run_bits++;
    }
    encoder->bit_index += run_bits;

    /*
     * Mayhem's standard-polarity path inverts the encoded bit before its FSK
     * modulator because POCSAG bit 1 is the negative-deviation tone. CC1101
     * asynchronous input 1 selects positive deviation, so standard polarity
     * needs the same inversion. The UI can deliberately swap it for systems
     * that use the non-standard polarity.
     */
    const bool async_level = encoder->inverted ? wire_bit : !wire_bit;
    return level_duration_make(async_level, encoder->bit_duration_us * run_bits);
}

/*
 * The CC1101 samples GDO0 at 8x its programmed data rate in asynchronous
 * mode. Program 38.4 kBaud so GDO0 is sampled at about 307 kHz for all POCSAG
 * rates; the timer callback above controls actual symbol duration. This cuts
 * transition quantization from about 26 us at the stock 4.8 kBaud setting to
 * about 3.3 us, and avoids the unsupported sub-600-baud range at POCSAG 512.
 * Deviation 0x13 is +/-4.36 kHz, the closest CC1101 step to Mayhem's +/-4.5.
 */
static uint8_t pocsag_tx_preset[] = {
    0x02, 0x0D, 0x0B, 0x06, 0x08, 0x32, 0x07, 0x04, 0x14, 0x00, 0x13, 0x02,
    0x12, 0x04, 0x11, 0x83, 0x10, 0x6A, 0x15, 0x13, 0x18, 0x18, 0x19, 0x16,
    0x1D, 0x91, 0x1C, 0x00, 0x1B, 0x07, 0x20, 0xFB, 0x22, 0x10, 0x21, 0x56,
    0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

uint8_t* pocsag_tx_get_preset(uint16_t baud) {
    if(baud != 512U && baud != 1200U && baud != 1600U && baud != 2400U) return NULL;
    return pocsag_tx_preset;
}
