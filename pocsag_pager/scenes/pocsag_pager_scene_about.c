#include "../pocsag_pager_app_i.h"
#include "../helpers/pocsag_pager_types.h"

#include <storage/storage.h>

void pocsag_pager_scene_about_widget_callback(GuiButtonType result, InputType type, void* context) {
    POCSAGPagerApp* app = context;
    if(type == InputTypeShort) {
        view_dispatcher_send_custom_event(app->view_dispatcher, result);
    }
}

/*
 * The body of this screen lives on the SD card rather than in .rodata. A FAP
 * is loaded into RAM whole and stays there for the session, so a page opened
 * once would otherwise cost about a kilobyte of RAM the whole time the app
 * runs. tools/export_about.py publishes the file beside the built .fap.
 *
 * Without it the screen still names who the app is built on and where to find
 * the rest, so attribution never depends on a file being copied.
 */
#define PCSG_ABOUT_PATH EXT_PATH("pocsag/ABOUT.txt")
#define PCSG_ABOUT_MAX_SIZE 4096U

static const char pcsg_about_fallback[] =
    "Developed by @xMasterX &\n@Shmuma, @htotoo, @Geofferey,\n"
    "on multimon-ng and Mayhem\nFLEX. Icons by @Svaarich.\n\n"
    "ABOUT.txt was not found in\n/ext/pocsag/\n\nFull credits and docs:\n"
    "github.com/Geofferey/\nFlipper-Momentum-Apps\n/tree/flex/pocsag_pager\n";

// Append the file's contents, or false if it could not be read whole.
static bool pocsag_pager_scene_about_load(FuriString* out) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool loaded = false;

    if(storage_file_open(file, PCSG_ABOUT_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const uint64_t size = storage_file_size(file);
        if(size && size <= PCSG_ABOUT_MAX_SIZE) {
            char* buffer = malloc((size_t)size + 1U);
            if(buffer) {
                if(storage_file_read(file, buffer, (size_t)size) == size) {
                    buffer[size] = '\0';
                    furi_string_cat_str(out, buffer);
                    loaded = true;
                }
                free(buffer);
            }
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return loaded;
}

void pocsag_pager_scene_about_on_enter(void* context) {
    POCSAGPagerApp* app = context;

    FuriString* temp_str;
    temp_str = furi_string_alloc();
    // Built-in because it has to match the binary, not whatever was copied.
    furi_string_printf(temp_str, "\e#Flipper Pager\nVersion: %s\n\n", PCSG_VERSION_APP);

    if(!pocsag_pager_scene_about_load(temp_str)) {
        furi_string_cat_str(temp_str, pcsg_about_fallback);
    }

    widget_add_text_box_element(
        app->widget,
        0,
        0,
        128,
        14,
        AlignCenter,
        AlignBottom,
        "\e#\e!                                                      \e!\n",
        false);
    widget_add_text_box_element(
        app->widget,
        0,
        2,
        128,
        14,
        AlignCenter,
        AlignBottom,
        "\e#\e!        Flipper Pager      \e!\n",
        false);
    widget_add_text_scroll_element(app->widget, 0, 16, 128, 50, furi_string_get_cstr(temp_str));
    furi_string_free(temp_str);

    view_dispatcher_switch_to_view(app->view_dispatcher, POCSAGPagerViewWidget);
}

bool pocsag_pager_scene_about_on_event(void* context, SceneManagerEvent event) {
    POCSAGPagerApp* app = context;
    bool consumed = false;
    UNUSED(app);
    UNUSED(event);

    return consumed;
}

void pocsag_pager_scene_about_on_exit(void* context) {
    POCSAGPagerApp* app = context;

    // Clear views
    widget_reset(app->widget);
}
