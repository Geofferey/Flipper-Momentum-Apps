#pragma once

#include <lib/subghz/protocols/base.h>

#define SUBGHZ_PROTOCOL_FLEX_NAME "FLEX"

typedef struct {
    uint32_t sync_detected;
    uint32_t fiw_valid;
    uint32_t fiw_invalid;
    /** Frame information words recovered by the opt-in soft decoder. */
    uint32_t fiw_chase;
    uint32_t frames_completed;
    uint32_t pages_decoded;
    /** Pages classified as secure or as opaque encrypted-looking text. */
    uint32_t pages_encrypted;
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
    /** Buffered joins emitted as partials when FLEX4 reception stopped. */
    uint32_t joined_session_flush;
    /** Join hardening: rejected controls, sequence gaps, duplicates, and bad signatures. */
    uint32_t joined_checksum_rejected;
    uint32_t joined_sequence_gap;
    uint32_t joined_duplicate;
    uint32_t joined_signature_bad;
    /** Phases dropped because the block information word failed BCH. */
    uint32_t phases_bad_biw;
    /** Pages skipped because the address or vector word failed BCH. */
    uint32_t pages_bad_header;
    /** Pages addressed with a two-word long address. */
    uint32_t pages_long_address;
    /** Pages held for repair because some message words failed BCH. */
    uint32_t pages_incomplete;
    /** Words recovered by the opt-in soft-decision Weak Decode (Chase). */
    uint32_t words_chase;
    /** Pages emitted that used at least one Chase-recovered word. */
    uint32_t pages_chase;
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
    /** The same content was already emitted to any capcode recently. */
    bool duplicate;
    /** The same content was already emitted to this capcode recently. */
    bool same_cap_duplicate;
    /** Some words were rebuilt from earlier copies of the page. */
    bool repaired;
    /** A FLEX secure page, or text that looks like an encrypted payload. */
    bool encrypted;
    /** Not fully decoded: failed checksum/signature, orphan, or missing fragment. */
    bool partial;
    /** A word in this page was recovered by the Weak Decode (Chase) decoder. */
    bool chase;
    /** Phase (A-D) and frame (0-127) of the page, or of a joined message's first part. */
    char phase;
    uint8_t frame;
} FlexPageInfo;

/** Timing carried by the most recently validated frame information word. */
typedef struct {
    uint8_t cycle;
    uint8_t frame;
    uint32_t tick;
    uint32_t sequence;
    bool chase;
} FlexFrameTiming;

/** Rate at which RX_HARD_DATA is sampled: 8 samples per 3200-baud symbol. */
#define FLEX_4FSK_SAMPLE_RATE_HZ 25600U

/** Feed one 4-FSK hard-decision sample (CC1101 RX_HARD_DATA[1:0]). */
void subghz_protocol_decoder_flex_feed_4fsk_sample(void* context, uint8_t hard_symbol);
/** Emit and clear every buffered fragmented message as the best available partial. */
void subghz_protocol_decoder_flex_flush_fragments(void* context);
void subghz_protocol_decoder_flex_reset_stats(void* context);
/** Describe the page most recently passed to the decoder callback. */
void subghz_protocol_decoder_flex_get_page_info(void* context, FlexPageInfo* info);
void subghz_protocol_decoder_flex_get_stats(void* context, FlexDecoderStats* stats);
/** Copy the most recent FIW timing; false until a valid FIW has arrived. */
bool subghz_protocol_decoder_flex_get_frame_timing(void* context, FlexFrameTiming* timing);
/** True while the decoder is locked onto a frame (not hunting for sync). */
bool subghz_protocol_decoder_flex_is_locked(void* context);

/**
 * Synthetic sound: play one tone per recovered symbol while locked. Must be
 * driven from the thread that owns the speaker (the FLEX4 sampler thread).
 */
void subghz_protocol_decoder_flex_set_synth(
    void* context,
    const float tones[4],
    float volume,
    bool enable);

/**
 * Opt-in Weak Decode: carry per-symbol DPLL vote margins as bit reliabilities
 * and soft-correct (Chase) words that fail BCH. Allocates its reliability
 * buffer only while enabled, so there is no RAM or CPU cost when off.
 */
void subghz_protocol_decoder_flex_set_weak_decode(void* context, bool enable);

extern const SubGhzProtocol subghz_protocol_flex;
