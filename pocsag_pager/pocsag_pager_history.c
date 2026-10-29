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
 * with the fields escaped by pcsg_block_generic_escape(). A separate hidden
 * index contains one compact record for each entry admitted to the viewer.
 * It is only needed during the live session and contains no message text.
 */
#define PCSG_HISTORY_INDEX_PATH       EXT_PATH("pocsag/.session.idx")
#define PCSG_HISTORY_LABEL_CACHE_SIZE 4U

typedef struct {
    uint32_t offset;
    uint16_t length;
    uint8_t type;
    uint8_t reserved;
} PCSGHistoryItem;

typedef struct {
    bool valid;
    uint16_t idx;
    uint8_t type;
    char label[PCSG_HISTORY_LABEL_SIZE];
} PCSGHistoryLabel;

struct PCSGHistory {
    FuriMutex* mutex;
    uint16_t count;
    uint16_t last_evicted;
    uint32_t total;

    Storage* storage;
    File* file;
    File* index_file;
    bool file_open;
    bool index_open;
    uint32_t file_size;
    // Temp mode: one hidden file, overwritten each session and destroyed on
    // exit, instead of a kept log per session.
    bool temp_mode;

    FlipperFormat* scratch;
    FuriString* protocol;
    FuriString* ric;
    FuriString* message;
    FuriString* line;
    // Dynamically allocated four-row viewport cache. Session length does not
    // affect its size.
    PCSGHistoryLabel* labels;
    uint8_t label_next;
};

static bool pcsg_history_read_item(
    PCSGHistory* instance,
    uint16_t idx,
    PCSGHistoryItem* item) {
    if(idx >= instance->count || !instance->index_open) return false;
    const uint32_t offset = (uint32_t)idx * sizeof(PCSGHistoryItem);
    return storage_file_seek(instance->index_file, offset, true) &&
           storage_file_read(instance->index_file, item, sizeof(PCSGHistoryItem)) ==
               sizeof(PCSGHistoryItem);
}

static void pcsg_history_close_log(PCSGHistory* instance) {
    if(instance->file_open) {
        storage_file_close(instance->file);
        instance->file_open = false;
    }
    if(instance->index_open) {
        storage_file_close(instance->index_file);
        instance->index_open = false;
    }
    storage_simply_remove(instance->storage, PCSG_HISTORY_INDEX_PATH);
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
    storage_simply_remove(instance->storage, PCSG_HISTORY_INDEX_PATH);
    instance->index_open = storage_file_open(
        instance->index_file, PCSG_HISTORY_INDEX_PATH, FSAM_READ_WRITE, FSOM_CREATE_ALWAYS);
    if(instance->file_open && instance->index_open) {
        instance->file_size = 0;
        FURI_LOG_I(TAG, "Logging to %s", furi_string_get_cstr(path));
    } else {
        FURI_LOG_E(TAG, "Unable to open %s", furi_string_get_cstr(path));
        if(instance->file_open) storage_file_close(instance->file);
        if(instance->index_open) storage_file_close(instance->index_file);
        instance->file_open = false;
        instance->index_open = false;
        storage_simply_remove(instance->storage, PCSG_HISTORY_INDEX_PATH);
    }
    furi_string_free(path);
    return instance->file_open && instance->index_open;
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

// Build the same label directly from escaped log fields without allocating
// complete unescaped copies of the header and message.
static void pcsg_history_make_label_escaped(
    char* label,
    size_t size,
    const char* ric,
    const char* message) {
    furi_assert(size);
    const char* newline = strstr(ric, "\\n");
    if(newline && newline[2]) ric = newline + 2;
    if(!strncmp(ric, "\\e#CAP: ", 8)) {
        ric += 8;
        while(*ric == '0' && ric[1] >= '0' && ric[1] <= '9') ric++;
    }

    size_t length = 0;
    const char* parts[] = {ric, message};
    for(size_t part = 0; part < COUNT_OF(parts); part++) {
        const char* text = parts[part];
        while(*text && length + 1U < size) {
            char c = *text++;
            if(c == '\\' && *text) {
                c = *text++;
                if(c == 'n') c = '\n';
                else if(c == 'r') c = '\r';
                else if(c == 't') c = '\t';
                else if(c == 'e') c = '\e';
            }
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
    storage_simply_remove(instance->storage, PCSG_HISTORY_INDEX_PATH);
    instance->file = storage_file_alloc(instance->storage);
    instance->index_file = storage_file_alloc(instance->storage);
    instance->scratch = flipper_format_string_alloc();
    instance->protocol = furi_string_alloc();
    instance->ric = furi_string_alloc();
    instance->message = furi_string_alloc();
    instance->line = furi_string_alloc();
    instance->labels = calloc(PCSG_HISTORY_LABEL_CACHE_SIZE, sizeof(PCSGHistoryLabel));
    return instance;
}

void pcsg_history_free(PCSGHistory* instance) {
    furi_assert(instance);
    pcsg_history_close_log(instance);
    pcsg_history_destroy_temp(instance->storage);
    storage_file_free(instance->file);
    storage_file_free(instance->index_file);
    furi_record_close(RECORD_STORAGE);
    flipper_format_free(instance->scratch);
    furi_string_free(instance->protocol);
    furi_string_free(instance->ric);
    furi_string_free(instance->message);
    furi_string_free(instance->line);
    free(instance->labels);
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
    instance->count = 0;
    instance->total = 0;
    instance->last_evicted = 0;
    instance->label_next = 0;
    memset(
        instance->labels, 0, PCSG_HISTORY_LABEL_CACHE_SIZE * sizeof(PCSGHistoryLabel));
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
    uint8_t type = 0;
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    for(size_t i = 0; i < PCSG_HISTORY_LABEL_CACHE_SIZE; i++) {
        if(instance->labels[i].valid && instance->labels[i].idx == idx) {
            type = instance->labels[i].type;
            break;
        }
    }
    if(!type) {
        PCSGHistoryItem item;
        if(pcsg_history_read_item(instance, idx, &item)) type = item.type;
    }
    furi_mutex_release(instance->mutex);
    return type;
}

void pcsg_history_get_text_item_menu(PCSGHistory* instance, FuriString* output, uint16_t idx) {
    furi_assert(instance);
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    PCSGHistoryLabel* cached = NULL;
    for(size_t i = 0; i < PCSG_HISTORY_LABEL_CACHE_SIZE; i++) {
        if(instance->labels[i].valid && instance->labels[i].idx == idx) {
            cached = &instance->labels[i];
            break;
        }
    }

    if(!cached) {
        PCSGHistoryItem item;
        if(pcsg_history_read_item(instance, idx, &item)) {
            PCSGHistoryLabel* slot = &instance->labels[instance->label_next];
            instance->label_next =
                (instance->label_next + 1U) % PCSG_HISTORY_LABEL_CACHE_SIZE;
            slot->valid = true;
            slot->idx = idx;
            slot->type = item.type;
            slot->label[0] = '\0';

            char* buffer = malloc(item.length + 1U);
            if(buffer) {
                const bool read = storage_file_seek(instance->file, item.offset, true) &&
                                  storage_file_read(instance->file, buffer, item.length) ==
                                      item.length;
                buffer[read ? item.length : 0] = '\0';
                char* fields[PCSGLogFieldCount];
                if(read && pcsg_history_split_line(buffer, fields)) {
                    pcsg_history_make_label_escaped(
                        slot->label,
                        sizeof(slot->label),
                        fields[PCSGLogFieldRic],
                        fields[PCSGLogFieldMessage]);
                }
                free(buffer);
            }
            cached = slot;
        }
    }
    if(cached) furi_string_set_str(output, cached->label);
    else furi_string_reset(output);
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
        if(!instance->file_open) break;
        PCSGHistoryItem item;
        if(!pcsg_history_read_item(instance, idx, &item)) break;

        char* buffer = malloc(item.length + 1U);
        if(!buffer) break;
        const bool read = storage_file_seek(instance->file, item.offset, true) &&
                          storage_file_read(instance->file, buffer, item.length) == item.length;
        buffer[read ? item.length : 0] = '\0';

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

static bool pcsg_history_parse_address(const char* text, const char* tag, uint32_t* address) {
    const char* cursor = strstr(text, tag);
    if(!cursor) return false;
    cursor += strlen(tag);
    if(*cursor < '0' || *cursor > '9') return false;

    uint32_t value = 0;
    do {
        const uint32_t digit = (uint32_t)(*cursor - '0');
        if(value > (UINT32_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
        cursor++;
    } while(*cursor >= '0' && *cursor <= '9');

    *address = value;
    return true;
}

bool pcsg_history_get_address(
    PCSGHistory* instance,
    uint16_t idx,
    PCSGHistoryAddressType* type,
    uint32_t* address) {
    furi_assert(instance);
    furi_assert(type);
    furi_assert(address);
    *type = PCSGHistoryAddressNone;
    *address = 0;

    bool result = false;
    furi_mutex_acquire(instance->mutex, FuriWaitForever);

    do {
        if(!instance->file_open) break;
        PCSGHistoryItem item;
        if(!pcsg_history_read_item(instance, idx, &item)) break;
        char* buffer = malloc(item.length + 1U);
        if(!buffer) break;
        const bool read = storage_file_seek(instance->file, item.offset, true) &&
                          storage_file_read(instance->file, buffer, item.length) == item.length;
        buffer[read ? item.length : 0] = '\0';

        char* fields[PCSGLogFieldCount];
        if(read && pcsg_history_split_line(buffer, fields)) {
            pcsg_block_generic_unescape(instance->ric, fields[PCSGLogFieldRic]);
            const char* header = furi_string_get_cstr(instance->ric);
            if(!strcmp(fields[PCSGLogFieldProtocol], "FLEX")) {
                result = pcsg_history_parse_address(header, "CAP: ", address);
                if(result) *type = PCSGHistoryAddressFlexCap;
            } else if(!strcmp(fields[PCSGLogFieldProtocol], "POCSAG")) {
                result = pcsg_history_parse_address(header, "RIC: ", address);
                if(result) *type = PCSGHistoryAddressPocsagRic;
            }
        }
        free(buffer);
    } while(false);

    furi_mutex_release(instance->mutex);
    return result;
}

PCSGHistoryStateAddKey
    pcsg_history_add_to_history(
        PCSGHistory* instance,
        void* context,
        SubGhzRadioPreset* preset,
        bool display) {
    furi_assert(instance);
    furi_assert(context);

    SubGhzProtocolDecoderBase* decoder_base = context;
    // Callers use this to reconcile their selection after every add attempt.
    instance->last_evicted = 0;

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

        instance->file_size += length + 1U;
        instance->total++;
        if(!display) {
            state = PCSGHistoryStateAddKeyLogged;
            break;
        }

        if(instance->count == UINT16_MAX) {
            // The log remains complete, but the UI's 16-bit list cannot grow
            // beyond this point.
            state = PCSGHistoryStateAddKeyOverflow;
            break;
        }

        PCSGHistoryItem item = {
            .offset = instance->file_size - (length + 1U),
            .length = length,
            .type = decoder_base->protocol->type,
            .reserved = 0,
        };
        const uint32_t index_offset = (uint32_t)instance->count * sizeof(PCSGHistoryItem);
        const bool indexed = storage_file_seek(instance->index_file, index_offset, true) &&
                             storage_file_write(
                                 instance->index_file, &item, sizeof(PCSGHistoryItem)) ==
                                 sizeof(PCSGHistoryItem);
        if(!indexed) {
            FURI_LOG_E(TAG, "Index write error");
            state = PCSGHistoryStateAddKeyOverflow;
            break;
        }

        PCSGHistoryLabel* slot = &instance->labels[instance->label_next];
        instance->label_next = (instance->label_next + 1U) % PCSG_HISTORY_LABEL_CACHE_SIZE;
        slot->valid = true;
        slot->idx = instance->count;
        slot->type = item.type;
        pcsg_history_make_label_escaped(
            slot->label,
            sizeof(slot->label),
            furi_string_get_cstr(instance->ric),
            furi_string_get_cstr(instance->message));

        instance->count++;
        state = PCSGHistoryStateAddKeyNewDada;
    } while(false);
    furi_mutex_release(instance->mutex);
    return state;
}
