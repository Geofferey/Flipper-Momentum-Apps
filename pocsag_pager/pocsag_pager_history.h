
#pragma once

#include <math.h>
#include <furi.h>
#include <furi_hal.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/subghz/types.h>

/** Entries kept in the on-screen list; every message is also logged to SD. */
#define PCSG_HISTORY_MAX 300

/** Where receive sessions are logged, one file per session. */
#define PCSG_HISTORY_LOG_DIR    EXT_PATH("pocsag/logs")
#define PCSG_HISTORY_LOG_EXT    ".txt"
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

/** Copy split log fields into Protocol/Ric/Message for the info view. */
bool pcsg_history_fields_to_flipper(char* const fields[PCSGLogFieldCount], FlipperFormat* output);

/** Build a one-line list label from an unescaped header and message. */
void pcsg_history_make_label(char* label, size_t size, const char* ric, const char* message);

typedef struct PCSGHistory PCSGHistory;

/** History state add key */
typedef enum {
    PCSGHistoryStateAddKeyUnknown,
    PCSGHistoryStateAddKeyTimeOut,
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

/** Whether the list is full, so the next message drops the oldest entry */
bool pcsg_history_is_full(PCSGHistory* instance);

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

/** Get status bar text for the history
 *
 * @param instance  - PCSGHistory instance
 * @param output    - FuriString* output
 * @return bool - is FULL; always false, the list rolls over instead
 */
bool pcsg_history_get_text_space_left(PCSGHistory* instance, FuriString* output);

/** Add protocol to history and append it to the log file
 *
 * @param instance  - PCSGHistory instance
 * @param context    - SubGhzProtocolCommon context
 * @param preset    - SubGhzRadioPreset preset
 * @return PCSGHistoryStateAddKey;
 */
PCSGHistoryStateAddKey
    pcsg_history_add_to_history(PCSGHistory* instance, void* context, SubGhzRadioPreset* preset);

/** Read history[idx] back from the log file
 *
 * @param instance  - PCSGHistory instance
 * @param idx       - record index
 * @return Protocol, Ric and Message as FlipperFormat, valid until the next
 *         call, or NULL if the log could not be read
 */
FlipperFormat* pcsg_history_get_raw_data(PCSGHistory* instance, uint16_t idx);
