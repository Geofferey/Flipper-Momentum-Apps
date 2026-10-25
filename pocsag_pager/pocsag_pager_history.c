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
#define PCSG_HISTORY_LOG_DIR    EXT_PATH("pocsag/logs")
#define PCSG_HISTORY_LABEL_SIZE 40U
#define PCSG_HISTORY_MAX_LINE   2048U

typedef struct {
    uint32_t offset;
    uint16_t length;
    uint8_t type;
    char label[PCSG_HISTORY_LABEL_SIZE];
} PCSGHistoryItem;

struct PCSGHistory {
    uint32_t last_update_timestamp;
    uint8_t code_last_hash_data;
    bool has_last_hash;

    FuriMutex* mutex;
    PCSGHistoryItem items[PCSG_HISTORY_MAX];
    uint16_t head;
    uint16_t count;
    uint32_t total;

    Storage* storage;
    File* file;
    bool file_open;
    uint32_t file_size;

    FlipperFormat* scratch;
    FuriString* protocol;
    FuriString* ric;
    FuriString* message;
    FuriString* line;
};

static PCSGHistoryItem* pcsg_history_item(PCSGHistory* instance, uint16_t idx) {
    return &instance->items[(instance->head + idx) % PCSG_HISTORY_MAX];
}

static void pcsg_history_close_log(PCSGHistory* instance) {
    if(instance->file_open) {
        storage_file_close(instance->file);
        instance->file_open = false;
    }
}

static bool pcsg_history_open_log(PCSGHistory* instance) {
    if(instance->file_open) return true;

    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);
    FuriString* path = furi_string_alloc_printf(
        "%s/%04u-%02u-%02u_%02u-%02u-%02u.txt",
        PCSG_HISTORY_LOG_DIR,
        datetime.year,
        datetime.month,
        datetime.day,
        datetime.hour,
        datetime.minute,
        datetime.second);

    storage_simply_mkdir(instance->storage, EXT_PATH("pocsag"));
    storage_simply_mkdir(instance->storage, PCSG_HISTORY_LOG_DIR);
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
static void pcsg_history_make_label(char* label, const char* ric, const char* message) {
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
        for(; *text && length + 1 < PCSG_HISTORY_LABEL_SIZE; text++) {
            char c = *text;
            if(c == '\e' || c == '#') continue;
            if(c == '\n' || c == '\r' || c == '\t') c = ' ';
            if(c == ' ' && (length == 0 || label[length - 1] == ' ')) continue;
            label[length++] = c;
        }
    }
    label[length] = '\0';
}

PCSGHistory* pcsg_history_alloc(void) {
    PCSGHistory* instance = calloc(1, sizeof(PCSGHistory));
    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->storage = furi_record_open(RECORD_STORAGE);
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

void pcsg_history_reset(PCSGHistory* instance) {
    furi_assert(instance);
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    pcsg_history_close_log(instance);
    instance->head = 0;
    instance->count = 0;
    instance->total = 0;
    instance->code_last_hash_data = 0;
    instance->has_last_hash = false;
    furi_mutex_release(instance->mutex);
}

uint16_t pcsg_history_get_item(PCSGHistory* instance) {
    furi_assert(instance);
    return instance->count;
}

bool pcsg_history_is_full(PCSGHistory* instance) {
    furi_assert(instance);
    return instance->count >= PCSG_HISTORY_MAX;
}

uint8_t pcsg_history_get_type_protocol(PCSGHistory* instance, uint16_t idx) {
    furi_assert(instance);
    if(idx >= instance->count) return 0;
    return pcsg_history_item(instance, idx)->type;
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

        // Fields: time, frequency, protocol, header, message.
        char* fields[5] = {0};
        char* cursor = buffer;
        for(size_t i = 0; i < COUNT_OF(fields) && cursor; i++) {
            fields[i] = cursor;
            cursor = strchr(cursor, '\t');
            if(cursor) *cursor++ = '\0';
        }

        if(read && fields[4]) {
            // Ric and Message stay escaped; pcsg_block_generic_deserialize()
            // unescapes them.
            stream_clean(flipper_format_get_raw_stream(instance->scratch));
            if(flipper_format_write_string_cstr(instance->scratch, "Protocol", fields[2]) &&
               flipper_format_write_string_cstr(instance->scratch, "Ric", fields[3]) &&
               flipper_format_write_string_cstr(instance->scratch, "Message", fields[4]) &&
               flipper_format_rewind(instance->scratch)) {
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
        if(length > PCSG_HISTORY_MAX_LINE || !pcsg_history_open_log(instance)) break;
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

        if(instance->count >= PCSG_HISTORY_MAX) {
            instance->head = (instance->head + 1U) % PCSG_HISTORY_MAX;
            instance->count--;
        }
        PCSGHistoryItem* item = pcsg_history_item(instance, instance->count);
        item->offset = instance->file_size;
        item->length = length;
        item->type = decoder_base->protocol->type;

        FuriString* ric = furi_string_alloc();
        FuriString* message = furi_string_alloc();
        pcsg_block_generic_unescape(ric, furi_string_get_cstr(instance->ric));
        pcsg_block_generic_unescape(message, furi_string_get_cstr(instance->message));
        pcsg_history_make_label(
            item->label, furi_string_get_cstr(ric), furi_string_get_cstr(message));
        furi_string_free(ric);
        furi_string_free(message);

        instance->file_size += length + 1U;
        instance->count++;
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
