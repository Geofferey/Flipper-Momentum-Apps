#include "pocsag_pager_log.h"

#include "pocsag_pager_history.h"

#include <storage/storage.h>

#define TAG "PCSGLog"

#define PCSG_LOG_READ_CHUNK  512U
#define PCSG_LOG_LABEL_CACHE 4U
#define PCSG_LOG_LABEL_PREFIX 256U

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
    return instance;
}

void pcsg_log_free(PCSGLog* instance) {
    furi_assert(instance);
    pcsg_log_close(instance);
    storage_file_free(instance->file);
    furi_record_close(RECORD_STORAGE);
    free(instance);
}

void pcsg_log_close(PCSGLog* instance) {
    furi_assert(instance);
    if(instance->open) storage_file_close(instance->file);
    instance->open = false;
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
    FURI_LOG_I(TAG, "%s: %lu messages", path, lines);
    return true;
}

// Read message idx into a malloc'd, NUL-terminated buffer, optionally only
// taking its prefix for list labels.
static char* pcsg_log_read_line(PCSGLog* instance, uint16_t idx, uint32_t limit) {
    if(!instance->open || idx >= instance->count) return NULL;
    const uint32_t start = pcsg_log_offset(instance, idx);
    uint32_t length = instance->file_size - start;
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
        // HH:MM from HH:MM:SS, then the same label the receiver list shows.
        const size_t time_length = strnlen(fields[PCSGLogFieldTime], 5);
        memcpy(slot->label, fields[PCSGLogFieldTime], time_length);
        slot->label[time_length] = ' ';
        pcsg_history_make_label_escaped(
            slot->label + time_length + 1U,
            sizeof(slot->label) - time_length - 1U,
            fields[PCSGLogFieldRic],
            fields[PCSGLogFieldMessage]);
    }
    free(line);
    furi_string_set_str(label, slot->label);
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
