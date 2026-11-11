
#pragma once

#include <math.h>
#include <furi.h>
#include <furi_hal.h>
#include <lib/subghz/types.h>

/*
 * Every message is logged to SD. Displayed-entry offsets are kept in a small
 * SD-side index, while RAM caches only the rows currently being drawn. Thus a
 * long receive session does not make the live list consume progressively more
 * heap.
 */

/** Where receive sessions are logged, one file per session. */
#define PCSG_HISTORY_LOG_DIR    EXT_PATH("pocsag/logs")
#define PCSG_HISTORY_LOG_EXT    ".txt"
/*
 * A kept log's index is saved beside it under the same stamp, so the pair
 * stays identifiable: <stamp>.txt and <stamp>.idx. The index holds each
 * message's offset and address, which is what lets a reader search by
 * address without reading the log itself. A temp session's index is
 * discarded with the rest of it.
 */
#define PCSG_HISTORY_INDEX_EXT  ".idx"
/** Single session file used instead in temp mode, outside the logs folder. */
#define PCSG_HISTORY_TEMP_PATH  EXT_PATH("pocsag/.session.tmp")
#define PCSG_HISTORY_LABEL_SIZE 40U
/** Longest log line written or read; fits the longest escaped header and message. */
#define PCSG_HISTORY_MAX_LINE   3072U

/** Tab-separated fields of a log line; Ric and Message are escaped. */
typedef enum {
    PCSGLogFieldTime,
    PCSGLogFieldFrequency,
    PCSGLogFieldProtocol,
    PCSGLogFieldRic,
    PCSGLogFieldMessage,
    PCSGLogFieldCount,
} PCSGLogField;

/** Split a log line in place. Returns false if fields are missing. */
bool pcsg_history_split_line(char* line, char* fields[PCSGLogFieldCount]);

/** Build a one-line list label directly from escaped log fields. */
void pcsg_history_make_label_escaped(
    char* label,
    size_t size,
    const char* ric,
    const char* message);

/** One owned log line with pointers into its escaped Ric and Message fields. */
typedef struct {
    char* data;
    const char* ric;
    const char* message;
} PCSGHistoryEntry;

/** Release an entry returned by a history or saved-log lookup. */
void pcsg_history_entry_free(PCSGHistoryEntry* entry);

typedef struct PCSGHistory PCSGHistory;

/** Address kind stored in a received message header. */
typedef enum {
    PCSGHistoryAddressNone,
    PCSGHistoryAddressFlexCap,
    PCSGHistoryAddressPocsagRic,
} PCSGHistoryAddressType;

/**
 * Read the address out of a log line's protocol and Ric fields. The Ric may
 * still be escaped: escaping only rewrites backslash and the control
 * characters, so the "CAP: " and "RIC: " tags and the digits after them are
 * the same either way. False when the protocol carries no address.
 */
bool pcsg_history_address_from_parts(
    const char* protocol,
    const char* ric,
    PCSGHistoryAddressType* type,
    uint32_t* address);

/** History state add key */
typedef enum {
    PCSGHistoryStateAddKeyUnknown,
    /** Written to the session log but intentionally omitted from the viewer. */
    PCSGHistoryStateAddKeyLogged,
    PCSGHistoryStateAddKeyNewDada,
    PCSGHistoryStateAddKeyUpdateData,
    PCSGHistoryStateAddKeyOverflow,
} PCSGHistoryStateAddKey;

/** Allocate PCSGHistory
 *
 * @return PCSGHistory*
 */
PCSGHistory* pcsg_history_alloc(void);

/** Free PCSGHistory
 *
 * @param instance - PCSGHistory instance
 */
void pcsg_history_free(PCSGHistory* instance);

/**
 * Choose where the next session is logged: a kept, dated log per session, or
 * (temp) one hidden file overwritten each session and destroyed when the
 * history is freed. Takes effect when the next log file is opened.
 */
void pcsg_history_set_temp_mode(PCSGHistory* instance, bool temp);

/** Clear history and end the current log file; the next message starts a new one.
 *
 * @param instance - PCSGHistory instance
 */
void pcsg_history_reset(PCSGHistory* instance);

/** Get the number of entries in the list
 *
 * @param instance  - PCSGHistory instance
 * @return count    - entries, oldest at index 0
 */
uint16_t pcsg_history_get_item(PCSGHistory* instance);

/** Entries rolled off the viewable window by the most recent add. */
uint16_t pcsg_history_last_evicted(PCSGHistory* instance);

/** Get type protocol to history[idx]
 *
 * @param instance  - PCSGHistory instance
 * @param idx       - record index
 * @return type      - type protocol
 */
uint8_t pcsg_history_get_type_protocol(PCSGHistory* instance, uint16_t idx);

/** Get string item menu to history[idx]
 *
 * @param instance  - PCSGHistory instance
 * @param output    - FuriString* output
 * @param idx       - record index
 */
void pcsg_history_get_text_item_menu(PCSGHistory* instance, FuriString* output, uint16_t idx);

/** Number of decoded entries written to this session log, including hidden ones. */
uint32_t pcsg_history_get_total(PCSGHistory* instance);

/** Append a protocol to the log file and optionally add it to the viewer.
 *
 * @param instance  - PCSGHistory instance
 * @param context    - SubGhzProtocolCommon context
 * @param preset    - SubGhzRadioPreset preset
 * @param display   - include this entry in the live viewer
 * @return PCSGHistoryStateAddKey;
 */
PCSGHistoryStateAddKey
    pcsg_history_add_to_history(
        PCSGHistory* instance,
        void* context,
        SubGhzRadioPreset* preset,
        bool display);

/** Read history[idx] back from the log file.
 *
 * @param instance  - PCSGHistory instance
 * @param idx       - record index
 * @param entry     - owned escaped header/message, freed with pcsg_history_entry_free
 * @return true if the entry was read
 */
bool pcsg_history_get_entry(PCSGHistory* instance, uint16_t idx, PCSGHistoryEntry* entry);

/** Read the FLEX CAP or POCSAG RIC from a viewable history entry. */
bool pcsg_history_get_address(
    PCSGHistory* instance,
    uint16_t idx,
    PCSGHistoryAddressType* type,
    uint32_t* address);

/**
 * Nearest message either side of `from` carrying the same address, searched
 * through the index rather than the log, so the whole session is covered
 * without a bound. False when there is none.
 */
bool pcsg_history_find_address(
    PCSGHistory* instance,
    uint16_t from,
    bool forward,
    PCSGHistoryAddressType type,
    uint32_t address,
    uint16_t* found);
