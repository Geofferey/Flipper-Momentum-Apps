#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Most differing bits flex_bch_combine() will search. */
#define FLEX_BCH_COMBINE_MAX_BITS 6U

/** Least-reliable bits the Chase decoder flips (2^N BCH tries per word). */
#define FLEX_BCH_CHASE_BITS 6U

/**
 * Soft-decision (Chase) correction of a word that failed flex_bch_correct().
 * rel holds per-bit reliabilities (bit 0..30, lower = less reliable). Flips
 * the least-reliable bits and re-corrects, accepting only a unique soft-best
 * codeword. Returns true and writes it when found.
 */
bool flex_bch_chase(uint32_t* word, const uint8_t rel[31]);

/** Correct up to two errors in a FLEX BCH(31,21) codeword. */
int flex_bch_correct(uint32_t* codeword);

/**
 * Recover a codeword from two uncorrectable copies of it. Succeeds only when a
 * single codeword explains both copies with every error at a bit where they
 * differ.
 */
bool flex_bch_combine(uint32_t first, uint32_t second, uint32_t* codeword);
