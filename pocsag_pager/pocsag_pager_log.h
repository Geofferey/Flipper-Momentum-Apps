#pragma once

#include <furi.h>
#include "pocsag_pager_history.h"

/** Read-only view of a saved session log for Review Messages. */
typedef struct PCSGLog PCSGLog;

/** Most messages indexed from one log; larger logs show their newest ones. */
#define PCSG_LOG_MAX_ENTRIES 4096U

PCSGLog* pcsg_log_alloc(void);

void pcsg_log_free(PCSGLog* instance);

/** Open and index a log file, closing any previous one. */
bool pcsg_log_open(PCSGLog* instance, const char* path);

void pcsg_log_close(PCSGLog* instance);

bool pcsg_log_is_open(PCSGLog* instance);

/** Number of indexed messages, oldest at index 0. */
uint16_t pcsg_log_get_count(PCSGLog* instance);

/** Whether the log held more messages than were indexed. */
bool pcsg_log_is_truncated(PCSGLog* instance);

/** List label for a message, using the same formatter as live history. */
void pcsg_log_get_label(PCSGLog* instance, uint16_t idx, FuriString* label);

/** Escaped header/message for the info view; caller frees the returned entry. */
bool pcsg_log_get_entry(PCSGLog* instance, uint16_t idx, PCSGHistoryEntry* entry);

/** Capcode or RIC of a message, read from its header prefix alone. */
bool pcsg_log_get_address(
    PCSGLog* instance,
    uint16_t idx,
    PCSGHistoryAddressType* type,
    uint32_t* address);

/**
 * Nearest message either side of `from` carrying the same address, searched
 * through the saved <stamp>.idx and so covering the whole log. False when
 * there is none, and also when the log has no usable index, which is the
 * caller's signal to fall back to its own bounded search.
 *
 * The index covers only messages that passed the filters while recording, so
 * one filtered out then is visible in the list but is never a destination.
 */
bool pcsg_log_find_address(
    PCSGLog* instance,
    uint16_t from,
    bool forward,
    PCSGHistoryAddressType type,
    uint32_t address,
    uint16_t* found);

/** Whether a saved index was found and accepted for the open log. */
bool pcsg_log_has_index(PCSGLog* instance);
