#include "pocsag_pager_receiver.h"
#include "../pocsag_pager_app_i.h"
#include "pocsag_pager_icons.h"
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
    // Exact byte offset of each wrapped line. Built on the first draw because
    // pixel widths require a Canvas, then freed with the viewed message.
    uint16_t* line_offsets;
    bool layout_ready;
    uint16_t scroll;
    uint16_t lines;
} PCSGReceiverInfoModel;

// Append escaped log text directly without the ESC "#" bold markers used in
// list headers. This avoids a second full-message temporary string.
static void pcsg_view_receiver_info_append_unescaped(FuriString* output, const char* text) {
    for(; *text; text++) {
        char value = *text;
        if(value == '\\' && text[1]) {
            text++;
            switch(*text) {
            case 'n':
                value = '\n';
                break;
            case 'r':
                value = '\r';
                break;
            case 't':
                value = '\t';
                break;
            case 'e':
                value = '\e';
                break;
            default:
                value = *text;
                break;
            }
        }
        if(value == '\e') {
            if(text[1] == '#') text++;
            continue;
        }
        furi_string_push_back(output, value);
    }
}

void pcsg_view_receiver_info_update(
    PCSGReceiverInfo* pcsg_receiver_info,
    const char* ric,
    const char* message) {
    furi_assert(pcsg_receiver_info);
    furi_assert(ric);
    furi_assert(message);

    with_view_model(
        pcsg_receiver_info->view,
        PCSGReceiverInfoModel * model,
        {
            if(!model->text) model->text = furi_string_alloc();
            furi_string_reset(model->text);
            free(model->line_offsets);
            model->line_offsets = NULL;
            model->layout_ready = false;
            pcsg_view_receiver_info_append_unescaped(model->text, ric);
            furi_string_trim(model->text, "\n");
            furi_string_push_back(model->text, '\n');
            pcsg_view_receiver_info_append_unescaped(model->text, message);
            model->scroll = 0;
            model->lines = 0;
        },
        true);
}

// Wrap one line, optionally returning its display text, and return the start
// of the next line. The same function builds the cache and renders from it so
// the two paths cannot disagree about wrapping.
static const char* pcsg_view_receiver_info_next_line(
    Canvas* canvas,
    const char* text,
    char line[INFO_LINE_CHARS]) {
    size_t length = 0;
    size_t space = SIZE_MAX;
    while(text[length] && text[length] != '\n' && length < INFO_LINE_CHARS - 1U) {
        line[length] = text[length];
        line[length + 1U] = '\0';
        if(length && canvas_string_width(canvas, line) > INFO_TEXT_WIDTH) break;
        if(text[length] == ' ') space = length;
        length++;
    }

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

    text += advance;
    if(wrapped) {
        while(*text == ' ') text++;
    }
    return text;
}

static void pcsg_view_receiver_info_cache_layout(
    Canvas* canvas,
    PCSGReceiverInfoModel* model) {
    const char* const text = furi_string_get_cstr(model->text);
    const char* cursor = text;
    char line[INFO_LINE_CHARS];
    uint16_t lines = 0;
    while(*cursor && lines < UINT16_MAX) {
        cursor = pcsg_view_receiver_info_next_line(canvas, cursor, line);
        lines++;
    }

    model->lines = lines;
    model->line_offsets = lines ? malloc(lines * sizeof(uint16_t)) : NULL;
    if(model->line_offsets) {
        cursor = text;
        for(uint16_t i = 0; i < lines; i++) {
            model->line_offsets[i] = (uint16_t)(cursor - text);
            cursor = pcsg_view_receiver_info_next_line(canvas, cursor, line);
        }
    }
    model->layout_ready = true;
}

void pcsg_view_receiver_info_draw(Canvas* canvas, PCSGReceiverInfoModel* model) {
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    if(!model->layout_ready) pcsg_view_receiver_info_cache_layout(canvas, model);
    const char* const text = furi_string_get_cstr(model->text);
    char line[INFO_LINE_CHARS];
    if(model->line_offsets) {
        const uint16_t last = MIN(model->lines, model->scroll + INFO_LINES);
        for(uint16_t i = model->scroll; i < last; i++) {
            pcsg_view_receiver_info_next_line(canvas, text + model->line_offsets[i], line);
            canvas_draw_str(canvas, 0, (i - model->scroll + 1U) * INFO_LINE_HEIGHT - 2U, line);
        }
    } else {
        // Allocation failure fallback: keep the message view usable by walking
        // to the visible rows without retaining an index.
        const char* cursor = text;
        uint16_t line_index = 0;
        while(*cursor && line_index < model->scroll + INFO_LINES) {
            cursor = pcsg_view_receiver_info_next_line(canvas, cursor, line);
            if(line_index >= model->scroll) {
                canvas_draw_str(
                    canvas,
                    0,
                    (line_index - model->scroll + 1U) * INFO_LINE_HEIGHT - 2U,
                    line);
            }
            line_index++;
        }
    }
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
            free(model->line_offsets);
            model->line_offsets = NULL;
            model->layout_ready = false;
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
            free(model->line_offsets);
        },
        false);

    view_free(pcsg_receiver_info->view);
    free(pcsg_receiver_info);
}

View* pcsg_view_receiver_info_get_view(PCSGReceiverInfo* pcsg_receiver_info) {
    furi_assert(pcsg_receiver_info);
    return pcsg_receiver_info->view;
}
