/*
 * BCH(31,21,2) error correction for FLEX.
 *
 * Derived from the public-domain BCH implementation in multimon-ng:
 * https://github.com/EliasOenal/multimon-ng/blob/master/bch.c
 */

#include "flex_bch.h"

#include <stdbool.h>
#include <string.h>

#define FLEX_BCH_CODE_BITS 31U
#define FLEX_BCH_PRIMITIVE 0x25U

static uint8_t flex_exp_table[32];
static uint8_t flex_s1_table[FLEX_BCH_CODE_BITS];
static uint8_t flex_s3_table[FLEX_BCH_CODE_BITS];
static uint16_t flex_bit_key[FLEX_BCH_CODE_BITS];
// Error bits for each syndrome: up to two bit positions, each stored plus one
// in 5 bits so that 0 means none (2 KB instead of 4 KB of 32-bit masks).
static uint16_t flex_error_table[1024];
static bool flex_bch_initialized;

static uint8_t flex_bch_ctz(uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return (uint8_t)__builtin_ctz(value);
#else
    uint8_t bit = 0;
    while((value & 1U) == 0U) {
        value >>= 1;
        bit++;
    }
    return bit;
#endif
}

static uint8_t flex_bch_popcount(uint32_t value) {
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

static uint16_t flex_bch_syndrome(uint32_t codeword) {
    uint8_t s1 = 0;
    uint8_t s3 = 0;

    codeword &= 0x7FFFFFFFU;
    while(codeword) {
        const uint8_t bit = flex_bch_ctz(codeword);
        const uint8_t received_index = 30U - bit;
        s1 ^= flex_s1_table[received_index];
        s3 ^= flex_s3_table[received_index];
        codeword &= codeword - 1U;
    }

    return ((uint16_t)s1 << 5) | s3;
}

static void flex_bch_init(void) {
    if(flex_bch_initialized) return;

    uint8_t element = 1;
    for(uint8_t i = 0; i < FLEX_BCH_CODE_BITS; i++) {
        flex_exp_table[i] = element;
        element <<= 1;
        if(element & 0x20U) element ^= FLEX_BCH_PRIMITIVE;
    }
    flex_exp_table[FLEX_BCH_CODE_BITS] = flex_exp_table[0];

    for(uint8_t i = 0; i < FLEX_BCH_CODE_BITS; i++) {
        flex_s1_table[i] = flex_exp_table[i];
        flex_s3_table[i] = flex_exp_table[(3U * i) % FLEX_BCH_CODE_BITS];
    }

    memset(flex_error_table, 0, sizeof(flex_error_table));
    for(uint8_t bit = 0; bit < FLEX_BCH_CODE_BITS; bit++) {
        const uint8_t received_index = 30U - bit;
        const uint16_t key =
            ((uint16_t)flex_s1_table[received_index] << 5) |
            flex_s3_table[received_index];
        flex_bit_key[bit] = key;
        flex_error_table[key] = bit + 1U;
    }

    for(uint8_t first = 0; first < FLEX_BCH_CODE_BITS; first++) {
        for(uint8_t second = first + 1; second < FLEX_BCH_CODE_BITS; second++) {
            const uint16_t key = flex_bit_key[first] ^ flex_bit_key[second];
            if(flex_error_table[key] == 0) {
                flex_error_table[key] = (first + 1U) | ((second + 1U) << 5);
            }
        }
    }

    flex_bch_initialized = true;
}

bool flex_bch_chase(uint32_t* word, const uint8_t rel[31]) {
    flex_bch_init();
    const uint32_t received = *word & 0x7FFFFFFFU;

    // The FLEX_BCH_CHASE_BITS least-reliable data-word bit positions (0..30).
    uint8_t pos[FLEX_BCH_CHASE_BITS];
    for(uint8_t i = 0; i < FLEX_BCH_CHASE_BITS; i++) {
        uint8_t best = 0xFFU;
        uint8_t chosen = 0;
        for(uint8_t b = 0; b < 31U; b++) {
            bool taken = false;
            for(uint8_t k = 0; k < i; k++) {
                if(pos[k] == b) taken = true;
            }
            if(!taken && rel[b] < best) {
                best = rel[b];
                chosen = b;
            }
        }
        pos[i] = chosen;
    }

    // Try every flip pattern of those bits, BCH-correct, and keep the unique
    // codeword of least soft cost (reliability of all the bits it changes).
    uint32_t best_word = 0;
    uint32_t best_cost = UINT32_MAX;
    bool unique = false;
    for(uint32_t pattern = 1; pattern < (1UL << FLEX_BCH_CHASE_BITS); pattern++) {
        uint32_t test = received;
        for(uint8_t i = 0; i < FLEX_BCH_CHASE_BITS; i++) {
            if(pattern & (1UL << i)) {
                test ^= 1UL << pos[i];
            }
        }
        uint32_t candidate = test;
        if(flex_bch_correct(&candidate) < 0) continue;
        candidate &= 0x7FFFFFFFU;
        // Score the resulting codeword against the received word, not the
        // path used to reach it. BCH may flip a Chase bit back; charging both
        // flips would count a bit that is unchanged in the candidate twice.
        uint32_t cost = 0;
        uint32_t diff = candidate ^ received;
        while(diff) {
            const uint8_t b = flex_bch_ctz(diff);
            if(b < 31U) cost += rel[b];
            diff &= diff - 1U;
        }
        if(cost < best_cost) {
            best_cost = cost;
            best_word = candidate;
            unique = true;
        } else if(cost == best_cost && candidate != best_word) {
            unique = false;
        }
    }

    if(unique && best_cost != UINT32_MAX) {
        *word = best_word;
        return true;
    }
    return false;
}

int flex_bch_correct(uint32_t* codeword) {
    flex_bch_init();

    const uint16_t syndrome = flex_bch_syndrome(*codeword);
    if(syndrome == 0) return 0;

    const uint16_t entry = flex_error_table[syndrome];
    if(entry == 0) return -1;
    uint32_t error = 1UL << ((entry & 0x1FU) - 1U);
    if(entry >> 5) error |= 1UL << ((entry >> 5) - 1U);

    *codeword ^= error;
    return flex_bch_popcount(error);
}

bool flex_bch_combine(uint32_t first, uint32_t second, uint32_t* codeword) {
    flex_bch_init();
    first &= 0x7FFFFFFFU;
    second &= 0x7FFFFFFFU;

    // Exactly one copy is wrong wherever the two differ. Try every assignment
    // of those bits and keep codewords that need no change where the copies
    // agree. Each copy alone was uncorrectable (3+ errors), so two copies with
    // disjoint errors differ in 6+ bits; the cap keeps the chance of a wrong
    // codeword landing in the search space low.
    const uint32_t differ = first ^ second;
    const uint8_t count = flex_bch_popcount(differ);
    if(count == 0 || count > FLEX_BCH_COMBINE_MAX_BITS) return false;

    uint8_t positions[FLEX_BCH_COMBINE_MAX_BITS];
    uint32_t remaining = differ;
    for(uint8_t i = 0; i < count; i++) {
        positions[i] = flex_bch_ctz(remaining);
        remaining &= remaining - 1U;
    }

    bool found = false;
    uint32_t best = 0;
    for(uint32_t pattern = 0; pattern < (1UL << count); pattern++) {
        uint32_t candidate = first;
        for(uint8_t i = 0; i < count; i++) {
            if(pattern & (1UL << i)) candidate ^= 1UL << positions[i];
        }
        if(flex_bch_correct(&candidate) < 0) continue;
        if((candidate ^ first) & ~differ) continue;
        if(found && candidate != best) return false;
        found = true;
        best = candidate;
    }

    if(found) *codeword = best;
    return found;
}
