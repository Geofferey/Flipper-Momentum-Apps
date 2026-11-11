#include "pocsag_pager_log.h"

#include "pocsag_pager_history.h"

#include <storage/storage.h>

#define TAG "PCSGLog"

#define PCSG_LOG_READ_CHUNK  512U
#define PCSG_LOG_LABEL_CACHE 4U
#define PCSG_LOG_LABEL_PREFIX 256U
// Enough of a line to reach past its header into the message field, which is
// what pcsg_history_split_line() needs to accept it.
#define PCSG_LOG_ADDRESS_PREFIX 256U

// Offsets live in 2 KB blocks allocated as lines are found, so no single
// allocation is large and short logs stay small.
#define PCSG_LOG_BLOCK_ENTRIES 512U
#define PCSG_LOG_BLOCKS        (PCSG_LOG_MAX_ENTRIES / PCSG_LOG_BLOCK_ENTRIES)

/*
 * Only the start offset of each line is kept in RAM (4 bytes per message).
 * Labels are built from the file when a row is drawn and cached for the few
 * rows on screen.
 */
typedef struct {
    bool valid;
    uint16_t idx;
    char label[PCSG_HISTORY_LABEL_SIZE];
} PCSGLogLabel;

struct PCSGLog {
    Storage* storage;
    File* file;
    bool open;
    uint32_t file_size;

    /*
     * The <stamp>.idx the live viewer saved beside this log, when there is
     * one. It carries each message's address against its byte offset, which
     * is what lets a search for one address cover the whole log; without it
     * the search falls back to reading header prefixes, bounded.
     *
     * It indexes only the messages that passed the filters while recording,
     * so it is a subset of the lines here and is matched by offset rather
     * than by position.
     */
    File* index_file;
    bool index_open;
    uint16_t index_count;

    uint32_t* blocks[PCSG_LOG_BLOCKS];
    // Ring position of index 0; non-zero only when the log was truncated.
    uint16_t first;
    uint16_t count;
    bool truncated;

    PCSGLogLabel labels[PCSG_LOG_LABEL_CACHE];
    uint8_t label_next;

};

static uint32_t* pcsg_log_slot(PCSGLog* instance, uint32_t position) {
    uint32_t** block = &instance->blocks[position / PCSG_LOG_BLOCK_ENTRIES];
    if(!*block) *block = malloc(PCSG_LOG_BLOCK_ENTRIES * sizeof(uint32_t));
    return *block ? &(*block)[position % PCSG_LOG_BLOCK_ENTRIES] : NULL;
}

static uint32_t pcsg_log_offset(PCSGLog* instance, uint16_t idx) {
    const uint32_t position = (instance->first + idx) % PCSG_LOG_MAX_ENTRIES;
    const uint32_t* block = instance->blocks[position / PCSG_LOG_BLOCK_ENTRIES];
    return block ? block[position % PCSG_LOG_BLOCK_ENTRIES] : instance->file_size;
}

PCSGLog* pcsg_log_alloc(void) {
    PCSGLog* instance = calloc(1, sizeof(PCSGLog));
    instance->storage = furi_record_open(RECORD_STORAGE);
    instance->file = storage_file_alloc(instance->storage);
    instance->index_file = storage_file_alloc(instance->storage);
    return instance;
}

void pcsg_log_free(PCSGLog* instance) {
    furi_assert(instance);
    pcsg_log_close(instance);
    storage_file_free(instance->index_file);
    storage_file_free(instance->file);
    furi_record_close(RECORD_STORAGE);
    free(instance);
}

void pcsg_log_close(PCSGLog* instance) {
    furi_assert(instance);
    if(instance->open) storage_file_close(instance->file);
    instance->open = false;
    if(instance->index_open) storage_file_close(instance->index_file);
    instance->index_open = false;
    instance->index_count = 0;
    for(size_t i = 0; i < PCSG_LOG_BLOCKS; i++) {
        free(instance->blocks[i]);
        instance->blocks[i] = NULL;
    }
    instance->first = 0;
    instance->count = 0;
    instance->truncated = false;
    memset(instance->labels, 0, sizeof(instance->labels));
}

bool pcsg_log_is_open(PCSGLog* instance) {
    furi_assert(instance);
    return instance->open;
}

uint16_t pcsg_log_get_count(PCSGLog* instance) {
    furi_assert(instance);
    return instance->count;
}

bool pcsg_log_is_truncated(PCSGLog* instance) {
    furi_assert(instance);
    return instance->truncated;
}

bool pcsg_log_has_index(PCSGLog* instance) {
    furi_assert(instance);
    return instance->index_open && instance->index_count;
}

// A run of index records in one seek. Bounds-checked against index_count,
// which pcsg_log_open_index() sets only once the file has been accepted.
static bool pcsg_log_read_index(
    PCSGLog* instance,
    uint16_t idx,
    PCSGHistoryItem* items,
    uint16_t count) {
    if(!instance->index_open || !count) return false;
    if(instance->index_count && idx + count > instance->index_count) return false;
    const uint32_t offset = (uint32_t)idx * sizeof(PCSGHistoryItem);
    const size_t bytes = (size_t)count * sizeof(PCSGHistoryItem);
    return storage_file_seek(instance->index_file, offset, true) &&
           storage_file_read(instance->index_file, items, bytes) == bytes;
}

/*
 * Open the <stamp>.idx beside <stamp>.txt, if the session that wrote the log
 * kept one. Absent, unreadable or inconsistent with this log, the viewer
 * simply works without it.
 */
static void pcsg_log_open_index(PCSGLog* instance, const char* path) {
    FuriString* index_path = furi_string_alloc_set_str(path);
    const size_t dot = furi_string_search_rchar(index_path, '.');
    if(dot != FURI_STRING_FAILURE) furi_string_left(index_path, dot);
    furi_string_cat_str(index_path, PCSG_HISTORY_INDEX_EXT);

    do {
        if(!storage_file_open(
               instance->index_file,
               furi_string_get_cstr(index_path),
               FSAM_READ,
               FSOM_OPEN_EXISTING)) {
            break;
        }
        instance->index_open = true;

        const uint64_t size = storage_file_size(instance->index_file);
        const uint64_t records = size / sizeof(PCSGHistoryItem);
        // Whole records only, and no more than the log could hold.
        if(!records || size % sizeof(PCSGHistoryItem) || records > PCSG_LOG_MAX_ENTRIES) break;

        // An index for a different log would point outside this one.
        PCSGHistoryItem last;
        if(!pcsg_log_read_index(instance, (uint16_t)records - 1U, &last, 1U)) break;
        if(last.offset >= instance->file_size) break;

        instance->index_count = (uint16_t)records;
    } while(false);

    if(instance->index_open && !instance->index_count) {
        storage_file_close(instance->index_file);
        instance->index_open = false;
    }
    furi_string_free(index_path);
}

bool pcsg_log_open(PCSGLog* instance, const char* path) {
    furi_assert(instance);
    pcsg_log_close(instance);
    if(!storage_file_open(instance->file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Unable to open %s", path);
        return false;
    }
    instance->open = true;
    instance->file_size = storage_file_size(instance->file);

    // Record where each line starts. Past the cap, keep the newest lines as a
    // ring that starts at the oldest kept line.
    uint8_t* chunk = malloc(PCSG_LOG_READ_CHUNK);
    if(!chunk) {
        pcsg_log_close(instance);
        return false;
    }
    uint32_t position = 0;
    uint32_t line_start = 0;
    uint32_t lines = 0;
    size_t read;
    while((read = storage_file_read(instance->file, chunk, PCSG_LOG_READ_CHUNK)) > 0) {
        for(size_t i = 0; i < read; i++) {
            if(chunk[i] != '\n') continue;
            if(position + i > line_start) {
                uint32_t* slot = pcsg_log_slot(instance, lines % PCSG_LOG_MAX_ENTRIES);
                if(!slot) {
                    free(chunk);
                    pcsg_log_close(instance);
                    return false;
                }
                *slot = line_start;
                lines++;
            }
            line_start = position + i + 1U;
        }
        position += read;
    }
    free(chunk);

    if(lines > PCSG_LOG_MAX_ENTRIES) {
        instance->first = lines % PCSG_LOG_MAX_ENTRIES;
        instance->count = PCSG_LOG_MAX_ENTRIES;
        instance->truncated = true;
    } else {
        instance->count = lines;
    }
    pcsg_log_open_index(instance, path);
    FURI_LOG_I(
        TAG,
        "%s: %lu messages, %u indexed",
        path,
        lines,
        instance->index_open ? instance->index_count : 0U);
    return true;
}

// Read message idx into a malloc'd, NUL-terminated buffer, optionally only
// taking its prefix for list labels.
static char* pcsg_log_read_line(PCSGLog* instance, uint16_t idx, uint32_t limit) {
    if(!instance->open || idx >= instance->count) return NULL;
    const uint32_t start = pcsg_log_offset(instance, idx);
    const uint32_t end = idx + 1U < instance->count ?
                             pcsg_log_offset(instance, idx + 1U) : instance->file_size;
    uint32_t length = end - start;
    if(length > limit) length = limit;

    char* buffer = malloc(length + 1U);
    if(!buffer) return NULL;
    size_t read = 0;
    if(storage_file_seek(instance->file, start, true)) {
        read = storage_file_read(instance->file, buffer, length);
    }
    buffer[read] = '\0';
    char* newline = strchr(buffer, '\n');
    if(newline) *newline = '\0';
    return buffer;
}

void pcsg_log_get_label(PCSGLog* instance, uint16_t idx, FuriString* label) {
    furi_assert(instance);
    for(size_t i = 0; i < PCSG_LOG_LABEL_CACHE; i++) {
        if(instance->labels[i].valid && instance->labels[i].idx == idx) {
            furi_string_set_str(label, instance->labels[i].label);
            return;
        }
    }

    PCSGLogLabel* slot = &instance->labels[instance->label_next];
    instance->label_next = (instance->label_next + 1U) % PCSG_LOG_LABEL_CACHE;
    slot->valid = true;
    slot->idx = idx;
    slot->label[0] = '\0';

    char* line = pcsg_log_read_line(instance, idx, PCSG_LOG_LABEL_PREFIX - 1U);
    char* fields[PCSGLogFieldCount];
    if(line && pcsg_history_split_line(line, fields)) {
        pcsg_history_make_label_escaped(
            slot->label,
            sizeof(slot->label),
            fields[PCSGLogFieldRic],
            fields[PCSGLogFieldMessage]);
    }
    free(line);
    furi_string_set_str(label, slot->label);
}

bool pcsg_log_get_address(
    PCSGLog* instance,
    uint16_t idx,
    PCSGHistoryAddressType* type,
    uint32_t* address) {
    furi_assert(instance);
    *type = PCSGHistoryAddressNone;
    *address = 0;
    if(!instance->open || idx >= instance->count) return false;

    /*
     * The saved log's index holds only each line's offset: addresses would add
     * four bytes an entry across up to PCSG_LOG_MAX_ENTRIES, which is more RAM
     * than a read-only viewer should hold. So this reads the header instead,
     * but only its prefix, into the stack, rather than taking the whole line
     * onto the heap for every entry a search steps over.
     */
    char buffer[PCSG_LOG_ADDRESS_PREFIX];
    if(!storage_file_seek(instance->file, pcsg_log_offset(instance, idx), true)) return false;
    const size_t read = storage_file_read(instance->file, buffer, sizeof(buffer) - 1U);
    buffer[read] = '\0';
    char* newline = strchr(buffer, '\n');
    if(newline) *newline = '\0';

    char* fields[PCSGLogFieldCount];
    if(!pcsg_history_split_line(buffer, fields)) return false;
    return pcsg_history_address_from_parts(
        fields[PCSGLogFieldProtocol], fields[PCSGLogFieldRic], type, address);
}

// Index records read per seek while searching, 384 bytes of stack.
#define PCSG_LOG_SCAN_ITEMS 32U

/*
 * Number of index records whose offset is below `target`. Records ascend by
 * offset, so this is a plain binary search and gives the scan its starting
 * point whether or not `target` is itself indexed, which matters because a
 * message filtered out while recording has no record.
 */
static uint16_t pcsg_log_index_lower_bound(PCSGLog* instance, uint32_t target) {
    uint16_t low = 0;
    uint16_t high = instance->index_count;
    while(low < high) {
        const uint16_t mid = low + (high - low) / 2U;
        PCSGHistoryItem item;
        if(!pcsg_log_read_index(instance, mid, &item, 1U)) break;
        if(item.offset < target) low = mid + 1U;
        else high = mid;
    }
    return low;
}

// Log entry starting at this byte offset. Offsets ascend with position here
// too, including across the ring of a truncated log.
static bool pcsg_log_index_of_offset(PCSGLog* instance, uint32_t offset, uint16_t* idx) {
    uint16_t low = 0;
    uint16_t high = instance->count;
    while(low < high) {
        const uint16_t mid = low + (high - low) / 2U;
        const uint32_t probe = pcsg_log_offset(instance, mid);
        if(probe == offset) {
            *idx = mid;
            return true;
        }
        if(probe < offset) low = mid + 1U;
        else high = mid;
    }
    return false;
}

bool pcsg_log_find_address(
    PCSGLog* instance,
    uint16_t from,
    bool forward,
    PCSGHistoryAddressType type,
    uint32_t address,
    uint16_t* found) {
    furi_assert(instance);
    furi_assert(found);
    if(!instance->open || !instance->index_open || type == PCSGHistoryAddressNone) return false;
    if(from >= instance->count) return false;

    const uint32_t from_offset = pcsg_log_offset(instance, from);
    PCSGHistoryItem items[PCSG_LOG_SCAN_ITEMS];

    if(forward) {
        // First record past this entry, even if this entry has none itself.
        uint16_t pos = pcsg_log_index_lower_bound(instance, from_offset + 1U);
        while(pos < instance->index_count) {
            const uint16_t batch =
                MIN((uint16_t)PCSG_LOG_SCAN_ITEMS, (uint16_t)(instance->index_count - pos));
            if(!pcsg_log_read_index(instance, pos, items, batch)) break;
            for(uint16_t i = 0; i < batch; i++) {
                if(items[i].address_type == type && items[i].address == address &&
                   pcsg_log_index_of_offset(instance, items[i].offset, found)) {
                    return true;
                }
            }
            pos += batch;
        }
    } else {
        uint16_t end = pcsg_log_index_lower_bound(instance, from_offset);
        while(end) {
            const uint16_t batch = MIN((uint16_t)PCSG_LOG_SCAN_ITEMS, end);
            const uint16_t start = end - batch;
            if(!pcsg_log_read_index(instance, start, items, batch)) break;
            for(uint16_t i = batch; i; i--) {
                const PCSGHistoryItem* item = &items[i - 1U];
                if(item->address_type == type && item->address == address &&
                   pcsg_log_index_of_offset(instance, item->offset, found)) {
                    return true;
                }
            }
            end = start;
        }
    }
    return false;
}

bool pcsg_log_get_entry(PCSGLog* instance, uint16_t idx, PCSGHistoryEntry* entry) {
    furi_assert(instance);
    furi_assert(entry);
    memset(entry, 0, sizeof(*entry));
    bool result = false;
    char* line = pcsg_log_read_line(instance, idx, PCSG_HISTORY_MAX_LINE);
    char* fields[PCSGLogFieldCount];
    if(line && pcsg_history_split_line(line, fields)) {
        entry->data = line;
        entry->ric = fields[PCSGLogFieldRic];
        entry->message = fields[PCSGLogFieldMessage];
        result = true;
    } else {
        free(line);
    }
    return result;
}
