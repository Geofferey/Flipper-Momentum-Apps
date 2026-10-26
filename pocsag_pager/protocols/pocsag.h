#pragma once

#include <lib/subghz/protocols/base.h>

#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include "pcsg_generic.h"
#include <lib/subghz/blocks/math.h>

#define SUBGHZ_PROTOCOL_POCSAG_NAME "POCSAG"

extern const SubGhzProtocol subghz_protocol_pocsag;

/** RIC (pager address) of the message most recently passed to the callback. */
uint32_t subghz_protocol_decoder_pocsag_get_ric(void* context);
