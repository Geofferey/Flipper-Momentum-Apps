#include "pocsag_pager_receiver.h"
#include "../pocsag_pager_app_i.h"
#include "pocsag_pager_icons.h"
#include "../protocols/pcsg_generic.h"
#include <input/input.h>
#include <gui/elements.h>

#define abs(x) ((x) > 0 ? (x) : -(x))

#define INFO_LINE_HEIGHT 10U
#define INFO_LINES       6U
#define INFO_TEXT_WIDTH  122U
#define INFO_LINE_CHARS  64U

struct PCSGReceiverInfo {
    View* view;
};

typedef struct {
    // Header and message as plain text, word-wrapped when drawn.
    FuriString* text;
    uint16_t scroll;
    uint16_t lines;
} PCSGReceiverInfoModel;

// Append text without the ESC "#" bold markers used in list headers.
static void pcsg_view_receiver_info_append_plain(FuriString* output, const char* text) {
    for(; *text; text++) {
        if(*text == '\e') {
            if(text[1] == '#') text++;
            continue;
        }
        furi_string_push_back(output, *text);
    }
}

void pcsg_view_receiver_info_update(PCSGReceiverInfo* pcsg_receiver_info, FlipperFormat* fff) {
    furi_assert(pcsg_receiver_info);
    furi_assert(fff);

    with_view_model(
        pcsg_receiver_info->view,
        PCSGReceiverInfoModel * model,
        {
            if(!model->text) model->text = furi_string_alloc();
            furi_string_reset(model->text);
            FuriString* raw = furi_string_alloc();
            FuriString* decoded = furi_string_alloc();
            if(flipper_format_rewind(fff) && flipper_format_read_string(fff, "Ric", raw)) {
                pcsg_block_generic_unescape(decoded, furi_string_get_cstr(raw));
                pcsg_view_receiver_info_append_plain(model->text, furi_string_get_cstr(decoded));
                furi_string_trim(model->text, "\n");
                furi_string_push_back(model->text, '\n');
                if(flipper_format_read_string(fff, "Message", raw)) {
                    pcsg_block_generic_unescape(decoded, furi_string_get_cstr(raw));
                    pcsg_view_receiver_info_append_plain(
                        model->text, furi_string_get_cstr(decoded));
                }
            }
            furi_string_free(raw);
            furi_string_free(decoded);
            model->scroll = 0;
            model->lines = 0;
        },
        true);
}

/*
 * Word-wrap text to INFO_TEXT_WIDTH, drawing the INFO_LINES lines starting at
 * line first. Returns the total number of lines. Wrapping is done on every
 * draw: messages are at most a few hundred characters.
 */
static uint16_t pcsg_view_receiver_info_layout(Canvas* canvas, const char* text, uint16_t first) {
    char line[INFO_LINE_CHARS];
    uint16_t lines = 0;
    while(*text) {
        size_t length = 0;
        size_t space = SIZE_MAX;
        while(text[length] && text[length] != '\n' && length < sizeof(line) - 1U) {
            line[length] = text[length];
            line[length + 1U] = '\0';
            if(length && canvas_string_width(canvas, line) > INFO_TEXT_WIDTH) break;
            if(text[length] == ' ') space = length;
            length++;
        }

        // Prefer to break at the last space; a single long word is split.
        const bool wrapped = text[length] && text[length] != '\n';
        size_t take = length;
        size_t advance = length;
        if(wrapped && space != SIZE_MAX && space > 0) {
            take = space;
            advance = space + 1U;
        } else if(!wrapped && text[length] == '\n') {
            advance = length + 1U;
        }
        if(advance == 0) advance = 1;
        line[take] = '\0';

        if(lines >= first && lines < first + INFO_LINES) {
            canvas_draw_str(
                canvas, 0, (lines - first + 1U) * INFO_LINE_HEIGHT - 2U, line);
        }
        lines++;

        text += advance;
        if(wrapped) {
            while(*text == ' ') text++;
        }
    }
    return lines;
}

void pcsg_view_receiver_info_draw(Canvas* canvas, PCSGReceiverInfoModel* model) {
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    model->lines =
        pcsg_view_receiver_info_layout(canvas, furi_string_get_cstr(model->text), model->scroll);
    if(model->lines > INFO_LINES) {
        elements_scrollbar_pos(
            canvas, 128, 0, 64, model->scroll, model->lines - INFO_LINES + 1U);
    }
}

bool pcsg_view_receiver_info_input(InputEvent* event, void* context) {
    furi_assert(context);
    PCSGReceiverInfo* pcsg_receiver_info = context;

    if(event->key == InputKeyBack) {
        return false;
    }

    if((event->key == InputKeyUp || event->key == InputKeyDown) &&
       (event->type == InputTypeShort || event->type == InputTypeRepeat)) {
        with_view_model(
            pcsg_receiver_info->view,
            PCSGReceiverInfoModel * model,
            {
                if(event->key == InputKeyUp && model->scroll > 0) {
                    model->scroll--;
                } else if(
                    event->key == InputKeyDown && model->scroll + INFO_LINES < model->lines) {
                    model->scroll++;
                }
            },
            true);
    }

    return true;
}

void pcsg_view_receiver_info_enter(void* context) {
    furi_assert(context);
}

void pcsg_view_receiver_info_exit(void* context) {
    furi_assert(context);
    PCSGReceiverInfo* pcsg_receiver_info = context;

    with_view_model(
        pcsg_receiver_info->view,
        PCSGReceiverInfoModel * model,
        {
            // A long page can grow several strings to its full size. Free the
            // backing buffers when leaving instead of retaining that peak for
            // the rest of the receive session.
            if(model->text) {
                furi_string_free(model->text);
                model->text = NULL;
            }
            model->scroll = 0;
            model->lines = 0;
        },
        false);
}

PCSGReceiverInfo* pcsg_view_receiver_info_alloc() {
    PCSGReceiverInfo* pcsg_receiver_info = malloc(sizeof(PCSGReceiverInfo));

    // View allocation and configuration
    pcsg_receiver_info->view = view_alloc();

    view_allocate_model(
        pcsg_receiver_info->view, ViewModelTypeLocking, sizeof(PCSGReceiverInfoModel));
    view_set_context(pcsg_receiver_info->view, pcsg_receiver_info);
    view_set_draw_callback(
        pcsg_receiver_info->view, (ViewDrawCallback)pcsg_view_receiver_info_draw);
    view_set_input_callback(pcsg_receiver_info->view, pcsg_view_receiver_info_input);
    view_set_enter_callback(pcsg_receiver_info->view, pcsg_view_receiver_info_enter);
    view_set_exit_callback(pcsg_receiver_info->view, pcsg_view_receiver_info_exit);

    with_view_model(
        pcsg_receiver_info->view,
        PCSGReceiverInfoModel * model,
        {
            model->text = furi_string_alloc();
        },
        true);

    return pcsg_receiver_info;
}

void pcsg_view_receiver_info_free(PCSGReceiverInfo* pcsg_receiver_info) {
    furi_assert(pcsg_receiver_info);

    with_view_model(
        pcsg_receiver_info->view,
        PCSGReceiverInfoModel * model,
        {
            if(model->text) furi_string_free(model->text);
        },
        false);

    view_free(pcsg_receiver_info->view);
    free(pcsg_receiver_info);
}

View* pcsg_view_receiver_info_get_view(PCSGReceiverInfo* pcsg_receiver_info) {
    furi_assert(pcsg_receiver_info);
    return pcsg_receiver_info->view;
}
