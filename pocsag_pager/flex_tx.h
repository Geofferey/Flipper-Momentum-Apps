#pragma once

#include "pocsag_tx.h"

#define FLEX_TX_SHORT_CAPCODE_MIN 1U
#define FLEX_TX_SHORT_CAPCODE_MAX 1933312U
#define FLEX_TX_BAUD              1600U

typedef struct FlexTxEncoder FlexTxEncoder;

FlexTxEncoder* flex_tx_encoder_alloc(void);
void flex_tx_encoder_free(FlexTxEncoder* encoder);

/** Build two complete 1600/2FSK FLEX frames for a short-address alpha page. */
bool flex_tx_encoder_prepare(FlexTxEncoder* encoder, const PocsagTxConfig* config);

/** Callback consumed by the Sub-GHz asynchronous transmitter. */
LevelDuration flex_tx_encoder_yield(void* context);
