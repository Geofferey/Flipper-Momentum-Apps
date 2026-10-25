#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Most differing bits flex_bch_combine() will search. */
#define FLEX_BCH_COMBINE_MAX_BITS 6U

/** Correct up to two errors in a FLEX BCH(31,21) codeword. */
int flex_bch_correct(uint32_t* codeword);

/**
 * Recover a codeword from two uncorrectable copies of it. Succeeds only when a
 * single codeword explains both copies with every error at a bit where they
 * differ.
 */
bool flex_bch_combine(uint32_t first, uint32_t second, uint32_t* codeword);
