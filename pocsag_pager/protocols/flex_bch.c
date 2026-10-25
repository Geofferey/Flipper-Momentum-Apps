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
static uint32_t flex_error_table[1024];
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
        flex_error_table[key] = 1UL << bit;
    }

    for(uint8_t first = 0; first < FLEX_BCH_CODE_BITS; first++) {
        for(uint8_t second = first + 1; second < FLEX_BCH_CODE_BITS; second++) {
            const uint16_t key = flex_bit_key[first] ^ flex_bit_key[second];
            if(flex_error_table[key] == 0) {
                flex_error_table[key] = (1UL << first) | (1UL << second);
            }
        }
    }

    flex_bch_initialized = true;
}

int flex_bch_correct(uint32_t* codeword) {
    flex_bch_init();

    const uint16_t syndrome = flex_bch_syndrome(*codeword);
    if(syndrome == 0) return 0;

    const uint32_t error = flex_error_table[syndrome];
    if(error == 0) return -1;

    *codeword ^= error;
    return flex_bch_popcount(error);
}
