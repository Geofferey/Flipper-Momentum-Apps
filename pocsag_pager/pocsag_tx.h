#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <toolbox/level_duration.h>

#define PCSG_TX_MAX_MESSAGE_LENGTH 80U
#define PCSG_TX_RIC_MAX            0x1FFFFFU
#define PCSG_TX_CORRECTION_MIN_HZ   (-10000)
#define PCSG_TX_CORRECTION_MAX_HZ   10000
#define PCSG_TX_CORRECTION_STEP_HZ  500

typedef enum {
    PagerTxProtocolPocsag,
    PagerTxProtocolFlex,
    PagerTxProtocolCount,
} PagerTxProtocol;

typedef struct {
    PagerTxProtocol protocol;
    uint32_t frequency;
    uint32_t ric;
    uint16_t baud;
    uint8_t function;
    bool inverted;
    int32_t frequency_correction_hz;
    char message[PCSG_TX_MAX_MESSAGE_LENGTH + 1U];
} PocsagTxConfig;

typedef struct PocsagTxEncoder PocsagTxEncoder;

PocsagTxEncoder* pocsag_tx_encoder_alloc(void);
void pocsag_tx_encoder_free(PocsagTxEncoder* encoder);

/** Build one complete, batch-padded alphanumeric POCSAG transmission. */
bool pocsag_tx_encoder_prepare(PocsagTxEncoder* encoder, const PocsagTxConfig* config);

/** Callback consumed by the Sub-GHz asynchronous transmitter. */
LevelDuration pocsag_tx_encoder_yield(void* context);

/** Known-good asynchronous 2-FSK preset; the waveform timer supplies the baud. */
uint8_t* pocsag_tx_get_preset(uint16_t baud);
