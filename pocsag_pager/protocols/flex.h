#pragma once

#include <lib/subghz/protocols/base.h>

#define SUBGHZ_PROTOCOL_FLEX_NAME "FLEX"

typedef struct {
    uint32_t sync_detected;
    uint32_t fiw_valid;
    uint32_t fiw_invalid;
    uint32_t frames_completed;
    uint32_t pages_decoded;
    uint32_t pages_repaired;
    /** Data words checked by BCH, and how many were uncorrectable. */
    uint32_t words_total;
    uint32_t words_bad;
    /** Uncorrectable words split by phase A-D. */
    uint32_t words_bad_phase[4];
    /** Pages skipped as short instructions, bad word ranges, or failed parses. */
    uint32_t pages_short_instruction;
    uint32_t pages_bad_range;
    uint32_t pages_parse_failed;
    /** Alphanumeric pages whose header checksum did not match. */
    uint32_t pages_checksum_bad;
    /** Fragments by kind, and how joined messages ended. */
    uint32_t fragments_first;
    uint32_t fragments_more;
    uint32_t fragments_last;
    uint32_t joined_complete;
    uint32_t joined_orphan;
    uint32_t joined_timeout;
    /** Phases dropped because the block information word failed BCH. */
    uint32_t phases_bad_biw;
    /** Pages skipped because the address or vector word failed BCH. */
    uint32_t pages_bad_header;
    /** Pages addressed with a two-word long address. */
    uint32_t pages_long_address;
    /** Pages held for repair because some message words failed BCH. */
    uint32_t pages_incomplete;
} FlexDecoderStats;

typedef enum {
    FlexPageKindAlpha,
    FlexPageKindNumeric,
    FlexPageKindTone,
    FlexPageKindBinary,
} FlexPageKind;

typedef struct {
    uint32_t capcode;
    FlexPageKind kind;
    /** The same page was already emitted within the last few minutes. */
    bool duplicate;
    /** Some words were rebuilt from earlier copies of the page. */
    bool repaired;
    /** A FLEX secure page, or text that looks like an encrypted payload. */
    bool encrypted;
} FlexPageInfo;

/** Rate at which RX_HARD_DATA is sampled: 8 samples per 3200-baud symbol. */
#define FLEX_4FSK_SAMPLE_RATE_HZ 25600U

/** Feed one 4-FSK hard-decision sample (CC1101 RX_HARD_DATA[1:0]). */
void subghz_protocol_decoder_flex_feed_4fsk_sample(void* context, uint8_t hard_symbol);
void subghz_protocol_decoder_flex_reset_stats(void* context);
/** Describe the page most recently passed to the decoder callback. */
void subghz_protocol_decoder_flex_get_page_info(void* context, FlexPageInfo* info);
void subghz_protocol_decoder_flex_get_stats(void* context, FlexDecoderStats* stats);

extern const SubGhzProtocol subghz_protocol_flex;
