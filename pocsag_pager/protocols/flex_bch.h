#pragma once

#include <stdint.h>

/** Correct up to two errors in a FLEX BCH(31,21) codeword. */
int flex_bch_correct(uint32_t* codeword);
