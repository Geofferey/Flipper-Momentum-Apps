#include "pocsag_pager_history.h"
#include <flipper_format/flipper_format_i.h>
#include <lib/toolbox/stream/stream.h>
#include <lib/subghz/receiver.h>
#include <storage/storage.h>
#include "protocols/pcsg_generic.h"

#include <furi.h>

#define TAG "PCSGHistory"

/*
 * Every message is appended to a log file on the SD card as soon as it is
 * received, one line per message:
 *
 *   HH:MM:SS <TAB> frequency <TAB> protocol <TAB> header <TAB> message
 *
 * with the fields escaped by pcsg_block_generic_escape(). RAM only keeps a
 * ring of recent entries: where each line is in the file and a short label
 * for the list, so the list can grow far beyond what full messages in RAM
 * would allow.
 */
// The index is a FIFO deque of fixed-size blocks: a block is allocated as the
// newest fills and freed as the oldest empties, so RAM tracks the live window
// and no single allocation is large. An idle receiver costs nothing.
#define PCSG_HISTORY_BLOCK_ITEMS 50U

typedef struct {
    uint32_t offset;
    uint16_t length;
    uint8_t type;
    char label[PCSG_HISTORY_LABEL_SIZE];
} PCSGHistoryItem;

typedef struct PCSGHistoryBlock {
    struct PCSGHistoryBlock* next;
    PCSGHistoryItem items[PCSG_HISTORY_BLOCK_ITEMS];
} PCSGHistoryBlock;

struct PCSGHistory {
    uint32_t last_update_timestamp;
    uint8_t code_last_hash_data;
    bool has_last_hash;

    FuriMutex* mutex;
    // Oldest entries live in head_block from head_offset; newest fill tail_block
    // up to tail_fill. Both NULL when empty.
    PCSGHistoryBlock* head_block;
    PCSGHistoryBlock* tail_block;
    uint16_t head_offset;
    uint16_t tail_fill;
    uint16_t count;
    uint16_t last_evicted;
    uint32_t total;

    Storage* storage;
    File* file;
    bool file_open;
    uint32_t file_size;
    // Temp mode: one hidden file, overwritten each session and destroyed on
    // exit, instead of a kept log per session.
    bool temp_mode;

    FlipperFormat* scratch;
    FuriString* protocol;
    FuriString* ric;
    FuriString* message;
    FuriString* line;
};

// Live item at list index idx (0 = oldest), or NULL if out of range.
static PCSGHistoryItem* pcsg_history_item(PCSGHistory* instance, uint16_t idx) {
    if(idx >= instance->count) return NULL;
    uint16_t position = instance->head_offset + idx;
    PCSGHistoryBlock* block = instance->head_block;
    while(position >= PCSG_HISTORY_BLOCK_ITEMS && block) {
        position -= PCSG_HISTORY_BLOCK_ITEMS;
        block = block->next;
    }
    return block ? &block->items[position] : NULL;
}

static void pcsg_history_free_blocks(PCSGHistory* instance) {
    PCSGHistoryBlock* block = instance->head_block;
    while(block) {
        PCSGHistoryBlock* next = block->next;
        free(block);
        block = next;
    }
    instance->head_block = NULL;
    instance->tail_block = NULL;
    instance->head_offset = 0;
    instance->tail_fill = 0;
}

// Drop the oldest item, freeing the head block once it empties. The entry
// stays in the SD log; only the viewable window shrinks.
static void pcsg_history_evict_oldest(PCSGHistory* instance) {
    if(instance->count == 0) return;
    instance->head_offset++;
    instance->count--;
    if(instance->count == 0) {
        pcsg_history_free_blocks(instance);
    } else if(instance->head_offset >= PCSG_HISTORY_BLOCK_ITEMS) {
        PCSGHistoryBlock* old = instance->head_block;
        instance->head_block = old->next;
        instance->head_offset = 0;
        free(old);
    }
}

// Append a slot for a new newest item, growing the deque by a block as needed.
// Returns NULL if a new block is needed but cannot be allocated.
static PCSGHistoryItem* pcsg_history_append_slot(PCSGHistory* instance) {
    if(!instance->tail_block || instance->tail_fill >= PCSG_HISTORY_BLOCK_ITEMS) {
        PCSGHistoryBlock* block = calloc(1, sizeof(PCSGHistoryBlock));
        if(!block) return NULL;
        if(instance->tail_block) {
            instance->tail_block->next = block;
        } else {
            instance->head_block = block;
            instance->head_offset = 0;
        }
        instance->tail_block = block;
        instance->tail_fill = 0;
    }
    PCSGHistoryItem* item = &instance->tail_block->items[instance->tail_fill];
    instance->tail_fill++;
    instance->count++;
    return item;
}

static void pcsg_history_close_log(PCSGHistory* instance) {
    if(instance->file_open) {
        storage_file_close(instance->file);
        instance->file_open = false;
    }
}

/*
 * Overwrite the temp session file with zeros, then delete it. Best effort:
 * the SD card's wear levelling may still keep old copies of the data.
 */
static void pcsg_history_destroy_temp(Storage* storage) {
    if(!storage_file_exists(storage, PCSG_HISTORY_TEMP_PATH)) return;
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, PCSG_HISTORY_TEMP_PATH, FSAM_WRITE, FSOM_OPEN_EXISTING)) {
        uint64_t remaining = storage_file_size(file);
        uint8_t zeros[64] = {0};
        while(remaining) {
            const size_t chunk = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
            if(storage_file_write(file, zeros, chunk) != chunk) break;
            remaining -= chunk;
        }
        storage_file_sync(file);
        storage_file_close(file);
    }
    storage_file_free(file);
    if(!storage_simply_remove(storage, PCSG_HISTORY_TEMP_PATH)) {
        FURI_LOG_E(TAG, "Unable to remove %s", PCSG_HISTORY_TEMP_PATH);
    }
}

static bool pcsg_history_open_log(PCSGHistory* instance, uint32_t frequency) {
    if(instance->file_open) return true;

    storage_simply_mkdir(instance->storage, EXT_PATH("pocsag"));
    FuriString* path;
    if(instance->temp_mode) {
        // Wipe the previous session's file rather than only truncating it.
        pcsg_history_destroy_temp(instance->storage);
        path = furi_string_alloc_set_str(PCSG_HISTORY_TEMP_PATH);
    } else {
        // Session start time and frequency, e.g. 2026-10-25_04-48-13_929.6125.txt
        DateTime datetime;
        furi_hal_rtc_get_datetime(&datetime);
        path = furi_string_alloc_printf(
            "%s/%04u-%02u-%02u_%02u-%02u-%02u_%lu.%04lu" PCSG_HISTORY_LOG_EXT,
            PCSG_HISTORY_LOG_DIR,
            datetime.year,
            datetime.month,
            datetime.day,
            datetime.hour,
            datetime.minute,
            datetime.second,
            frequency / 1000000U,
            (frequency % 1000000U) / 100U);
        storage_simply_mkdir(instance->storage, PCSG_HISTORY_LOG_DIR);
    }
    instance->file_open = storage_file_open(
        instance->file, furi_string_get_cstr(path), FSAM_READ_WRITE, FSOM_CREATE_ALWAYS);
    if(instance->file_open) {
        instance->file_size = 0;
        FURI_LOG_I(TAG, "Logging to %s", furi_string_get_cstr(path));
    } else {
        FURI_LOG_E(TAG, "Unable to open %s", furi_string_get_cstr(path));
    }
    furi_string_free(path);
    return instance->file_open;
}

// One line for the list: the header without its first-line tag (the FLEX
// [F6400/A ...] block), ESC markers and "CAP: " dropped, then the message.
void pcsg_history_make_label(char* label, size_t size, const char* ric, const char* message) {
    furi_assert(size);
    const char* newline = strchr(ric, '\n');
    if(newline && newline[1]) ric = newline + 1;

    size_t length = 0;
    const char* parts[] = {ric, message};
    for(size_t part = 0; part < COUNT_OF(parts); part++) {
        const char* text = parts[part];
        if(!strncmp(text, "\e#CAP: ", 7)) {
            text += 7;
            while(*text == '0' && text[1] >= '0' && text[1] <= '9') text++;
        }
        for(; *text && length + 1 < size; text++) {
            char c = *text;
            if(c == '\e' || c == '#') continue;
            if(c == '\n' || c == '\r' || c == '\t') c = ' ';
            if(c == ' ' && (length == 0 || label[length - 1] == ' ')) continue;
            label[length++] = c;
        }
    }
    label[length] = '\0';
}

bool pcsg_history_split_line(char* line, char* fields[PCSGLogFieldCount]) {
    char* cursor = line;
    for(size_t i = 0; i < PCSGLogFieldCount; i++) {
        fields[i] = cursor;
        if(!cursor) continue;
        cursor = strchr(cursor, '\t');
        if(cursor) *cursor++ = '\0';
    }
    return fields[PCSGLogFieldMessage] != NULL;
}

bool pcsg_history_fields_to_flipper(char* const fields[PCSGLogFieldCount], FlipperFormat* output) {
    // Ric and Message stay escaped; pcsg_block_generic_deserialize() unescapes them.
    stream_clean(flipper_format_get_raw_stream(output));
    return flipper_format_write_string_cstr(output, "Protocol", fields[PCSGLogFieldProtocol]) &&
           flipper_format_write_string_cstr(output, "Ric", fields[PCSGLogFieldRic]) &&
           flipper_format_write_string_cstr(output, "Message", fields[PCSGLogFieldMessage]) &&
           flipper_format_rewind(output);
}

PCSGHistory* pcsg_history_alloc(void) {
    PCSGHistory* instance = calloc(1, sizeof(PCSGHistory));
    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->storage = furi_record_open(RECORD_STORAGE);
    // A temp file left by a crash or power loss is destroyed at the next start.
    pcsg_history_destroy_temp(instance->storage);
    instance->file = storage_file_alloc(instance->storage);
    instance->scratch = flipper_format_string_alloc();
    instance->protocol = furi_string_alloc();
    instance->ric = furi_string_alloc();
    instance->message = furi_string_alloc();
    instance->line = furi_string_alloc();
    return instance;
}

void pcsg_history_free(PCSGHistory* instance) {
    furi_assert(instance);
    pcsg_history_close_log(instance);
    pcsg_history_destroy_temp(instance->storage);
    pcsg_history_free_blocks(instance);
    storage_file_free(instance->file);
    furi_record_close(RECORD_STORAGE);
    flipper_format_free(instance->scratch);
    furi_string_free(instance->protocol);
    furi_string_free(instance->ric);
    furi_string_free(instance->message);
    furi_string_free(instance->line);
    furi_mutex_free(instance->mutex);
    free(instance);
}

void pcsg_history_set_temp_mode(PCSGHistory* instance, bool temp) {
    furi_assert(instance);
    instance->temp_mode = temp;
}

void pcsg_history_reset(PCSGHistory* instance) {
    furi_assert(instance);
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    pcsg_history_close_log(instance);
    pcsg_history_free_blocks(instance);
    instance->count = 0;
    instance->total = 0;
    instance->last_evicted = 0;
    instance->code_last_hash_data = 0;
    instance->has_last_hash = false;
    furi_mutex_release(instance->mutex);
}

uint16_t pcsg_history_get_item(PCSGHistory* instance) {
    furi_assert(instance);
    return instance->count;
}

uint16_t pcsg_history_last_evicted(PCSGHistory* instance) {
    furi_assert(instance);
    return instance->last_evicted;
}

uint8_t pcsg_history_get_type_protocol(PCSGHistory* instance, uint16_t idx) {
    furi_assert(instance);
    const PCSGHistoryItem* item = pcsg_history_item(instance, idx);
    return item ? item->type : 0;
}

void pcsg_history_get_text_item_menu(PCSGHistory* instance, FuriString* output, uint16_t idx) {
    furi_assert(instance);
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    if(idx < instance->count) {
        furi_string_set_str(output, pcsg_history_item(instance, idx)->label);
    } else {
        furi_string_reset(output);
    }
    furi_mutex_release(instance->mutex);
}

bool pcsg_history_get_text_space_left(PCSGHistory* instance, FuriString* output) {
    furi_assert(instance);
    // Messages received this session; the list keeps the newest ones.
    if(output != NULL) furi_string_printf(output, "%lu", instance->total);
    return false;
}

FlipperFormat* pcsg_history_get_raw_data(PCSGHistory* instance, uint16_t idx) {
    furi_assert(instance);
    FlipperFormat* result = NULL;
    furi_mutex_acquire(instance->mutex, FuriWaitForever);

    do {
        if(idx >= instance->count || !instance->file_open) break;
        const PCSGHistoryItem* item = pcsg_history_item(instance, idx);

        char* buffer = malloc(item->length + 1U);
        const bool read = storage_file_seek(instance->file, item->offset, true) &&
                          storage_file_read(instance->file, buffer, item->length) == item->length;
        buffer[read ? item->length : 0] = '\0';

        char* fields[PCSGLogFieldCount];
        if(read && pcsg_history_split_line(buffer, fields)) {
            if(pcsg_history_fields_to_flipper(fields, instance->scratch)) {
                result = instance->scratch;
            }
        } else {
            FURI_LOG_E(TAG, "Unable to read entry %u", idx);
        }
        free(buffer);
    } while(false);

    furi_mutex_release(instance->mutex);
    return result;
}

PCSGHistoryStateAddKey
    pcsg_history_add_to_history(PCSGHistory* instance, void* context, SubGhzRadioPreset* preset) {
    furi_assert(instance);
    furi_assert(context);

    SubGhzProtocolDecoderBase* decoder_base = context;
    const uint8_t hash = subghz_protocol_decoder_base_get_hash_data(decoder_base);
    if(instance->has_last_hash && (instance->code_last_hash_data == hash) &&
       ((furi_get_tick() - instance->last_update_timestamp) < 500)) {
        instance->last_update_timestamp = furi_get_tick();
        return PCSGHistoryStateAddKeyTimeOut;
    }

    // Ric and Message come back escaped from the serializer, ready for the log.
    FlipperFormat* scratch = instance->scratch;
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    bool valid = subghz_protocol_decoder_base_serialize(decoder_base, scratch, preset) ==
                     SubGhzProtocolStatusOk &&
                 flipper_format_rewind(scratch) &&
                 flipper_format_read_string(scratch, "Protocol", instance->protocol) &&
                 flipper_format_read_string(scratch, "Ric", instance->ric) &&
                 flipper_format_read_string(scratch, "Message", instance->message);
    if(!valid) {
        furi_mutex_release(instance->mutex);
        FURI_LOG_E(TAG, "Serialize error");
        return PCSGHistoryStateAddKeyUnknown;
    }

    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);
    furi_string_printf(
        instance->line,
        "%02u:%02u:%02u\t%lu\t%s\t%s\t%s",
        datetime.hour,
        datetime.minute,
        datetime.second,
        preset->frequency,
        furi_string_get_cstr(instance->protocol),
        furi_string_get_cstr(instance->ric),
        furi_string_get_cstr(instance->message));
    const size_t length = furi_string_size(instance->line);

    PCSGHistoryStateAddKey state = PCSGHistoryStateAddKeyUnknown;
    do {
        if(length > PCSG_HISTORY_MAX_LINE ||
           !pcsg_history_open_log(instance, preset->frequency)) {
            break;
        }
        furi_string_push_back(instance->line, '\n');
        if(!storage_file_seek(instance->file, instance->file_size, true) ||
           storage_file_write(
               instance->file, furi_string_get_cstr(instance->line), length + 1U) !=
               length + 1U) {
            FURI_LOG_E(TAG, "Log write error");
            break;
        }
        // Keep the log intact if the app or the battery dies.
        storage_file_sync(instance->file);

        // Roll the oldest entries off the viewable window while free heap is
        // tight, keeping at least PCSG_HISTORY_MIN_ITEMS. They stay in the log.
        instance->last_evicted = 0;
        while(instance->count > PCSG_HISTORY_MIN_ITEMS &&
              memmgr_get_free_heap() < PCSG_HISTORY_HEAP_RESERVE) {
            pcsg_history_evict_oldest(instance);
            instance->last_evicted++;
        }

        PCSGHistoryItem* item = pcsg_history_append_slot(instance);
        while(!item && instance->count > 0) {
            // Under severe pressure a new block will not allocate; free one by
            // dropping the oldest, then retry.
            pcsg_history_evict_oldest(instance);
            instance->last_evicted++;
            item = pcsg_history_append_slot(instance);
        }
        if(!item) {
            // Nothing could be stored in RAM, but the entry is in the SD log.
            instance->file_size += length + 1U;
            instance->total++;
            state = PCSGHistoryStateAddKeyOverflow;
            break;
        }
        item->offset = instance->file_size;
        item->length = length;
        item->type = decoder_base->protocol->type;

        FuriString* ric = furi_string_alloc();
        FuriString* message = furi_string_alloc();
        pcsg_block_generic_unescape(ric, furi_string_get_cstr(instance->ric));
        pcsg_block_generic_unescape(message, furi_string_get_cstr(instance->message));
        pcsg_history_make_label(
            item->label,
            sizeof(item->label),
            furi_string_get_cstr(ric),
            furi_string_get_cstr(message));
        furi_string_free(ric);
        furi_string_free(message);

        instance->file_size += length + 1U;
        instance->total++;
        state = PCSGHistoryStateAddKeyNewDada;
    } while(false);
    furi_mutex_release(instance->mutex);

    if(state == PCSGHistoryStateAddKeyNewDada) {
        instance->code_last_hash_data = hash;
        instance->has_last_hash = true;
        instance->last_update_timestamp = furi_get_tick();
    }
    return state;
}
