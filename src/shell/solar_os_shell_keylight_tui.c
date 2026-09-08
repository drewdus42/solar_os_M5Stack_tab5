#include "solar_os_shell_keylight_tui.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_keys.h"
#include "solar_os_m5tab5_keyboard.h"
#include "solar_os_sessions.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_shell_parse.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define KEYLIGHT_TUI_STATUS_MAX 64
#define KEYLIGHT_TUI_EDIT_MAX 32
#define KEYLIGHT_TUI_CURSOR_BLINK_MS 500

typedef enum {
    KEYLIGHT_ITEM_BACKLIGHT = 0,
    KEYLIGHT_ITEM_RGB_MODE,
    KEYLIGHT_ITEM_PRESET,
    KEYLIGHT_ITEM_HEX,
    KEYLIGHT_ITEM_RED,
    KEYLIGHT_ITEM_GREEN,
    KEYLIGHT_ITEM_BLUE,
    KEYLIGHT_ITEM_COUNT,
} keylight_item_t;

typedef struct {
    const char *label;
    bool editable;
} keylight_item_def_t;

typedef struct {
    const char *name;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} color_preset_t;

static const color_preset_t color_presets[] = {
    {"off",        0,   0,   0},
    {"dim amber",  60,  25,  0},
    {"warm white", 255, 180, 100},
    {"dim warm",   80,  55,  30},
    {"amber",      255, 120, 0},
    {"dim red",    50,  0,   0},
    {"red",        255, 0,   0},
    {"dim green",  0,   50,  0},
    {"green",      0,   255, 0},
    {"dim blue",   0,   25,  60},
    {"blue",       0,   80,  255},
    {"cyan",       0,   255, 255},
    {"yellow",     255, 220, 0},
    {"purple",     160, 0,   255},
    {"magenta",    255, 0,   180},
    {"white",      255, 255, 255},
};
#define COLOR_PRESETS_COUNT (sizeof(color_presets) / sizeof(color_presets[0]))

static const keylight_item_def_t keylight_items[] = {
    [KEYLIGHT_ITEM_BACKLIGHT] = {.label = "backlight",  .editable = true},
    [KEYLIGHT_ITEM_RGB_MODE]  = {.label = "rgb mode",   .editable = false},
    [KEYLIGHT_ITEM_PRESET]    = {.label = "preset",     .editable = false},
    [KEYLIGHT_ITEM_HEX]       = {.label = "hex color",  .editable = true},
    [KEYLIGHT_ITEM_RED]       = {.label = "red",        .editable = true},
    [KEYLIGHT_ITEM_GREEN]     = {.label = "green",      .editable = true},
    [KEYLIGHT_ITEM_BLUE]      = {.label = "blue",       .editable = true},
};

typedef struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    size_t selected;
    size_t first_visible;
    bool editing;
    bool cursor_visible;
    uint32_t last_cursor_blink_ms;
    char edit_text[KEYLIGHT_TUI_EDIT_MAX];
    char original_text[KEYLIGHT_TUI_EDIT_MAX];
    char status[KEYLIGHT_TUI_STATUS_MAX];

    uint8_t current_backlight;
    uint8_t current_rgb_mode;
    uint8_t current_r;
    uint8_t current_g;
    uint8_t current_b;
    int current_preset;
} keylight_tui_state_t;

static void *keylight_tui_state;
#define keylight_tui (*(keylight_tui_state_t *)keylight_tui_state)

static void keylight_tui_set_status(const char *status)
{
    strlcpy(keylight_tui.status, status != NULL ? status : "", sizeof(keylight_tui.status));
}

static size_t keylight_tui_visible_width(size_t cols, size_t start_col)
{
    return start_col < cols ? cols - start_col : 0;
}

static int keylight_find_matching_preset(uint8_t r, uint8_t g, uint8_t b)
{
    for (size_t i = 0; i < COLOR_PRESETS_COUNT; i++) {
        if (color_presets[i].r == r &&
            color_presets[i].g == g &&
            color_presets[i].b == b) {
            return (int)i;
        }
    }
    return -1;
}

static void keylight_sync_state_from_hardware(void)
{
    uint8_t b = 0;
    if (solar_os_m5tab5_keyboard_get_backlight(&b) == ESP_OK) {
        keylight_tui.current_backlight = b;
    }
    uint8_t m = 0;
    if (solar_os_m5tab5_keyboard_get_rgb_mode(&m) == ESP_OK) {
        keylight_tui.current_rgb_mode = m;
    }
    uint8_t cr = 0, cg = 0, cb = 0;
    if (solar_os_m5tab5_keyboard_get_rgb(&cr, &cg, &cb) == ESP_OK) {
        keylight_tui.current_r = cr;
        keylight_tui.current_g = cg;
        keylight_tui.current_b = cb;
    }
    keylight_tui.current_preset = keylight_find_matching_preset(keylight_tui.current_r,
                                                                keylight_tui.current_g,
                                                                keylight_tui.current_b);
}

static void keylight_format_item_value(keylight_item_t item, char *buffer, size_t max_len)
{
    switch (item) {
    case KEYLIGHT_ITEM_BACKLIGHT: {
        const unsigned val = keylight_tui.current_backlight;
        char bar[12];
        const unsigned filled = (val + 5) / 10;
        for (unsigned i = 0; i < 10; i++) {
            bar[i] = (i < filled) ? '=' : '-';
        }
        bar[10] = '\0';
        snprintf(buffer, max_len, "%3u%%  [%s]", val, bar);
        break;
    }
    case KEYLIGHT_ITEM_RGB_MODE:
        snprintf(buffer, max_len, "%s", keylight_tui.current_rgb_mode == 1U ? "custom" : "auto");
        break;
    case KEYLIGHT_ITEM_PRESET:
        if (keylight_tui.current_preset >= 0 &&
            (size_t)keylight_tui.current_preset < COLOR_PRESETS_COUNT) {
            snprintf(buffer, max_len, "< %s >", color_presets[keylight_tui.current_preset].name);
        } else {
            snprintf(buffer, max_len, "< custom >");
        }
        break;
    case KEYLIGHT_ITEM_HEX:
        snprintf(buffer, max_len, "#%02X%02X%02X",
                 keylight_tui.current_r,
                 keylight_tui.current_g,
                 keylight_tui.current_b);
        break;
    case KEYLIGHT_ITEM_RED:
        snprintf(buffer, max_len, "%u", keylight_tui.current_r);
        break;
    case KEYLIGHT_ITEM_GREEN:
        snprintf(buffer, max_len, "%u", keylight_tui.current_g);
        break;
    case KEYLIGHT_ITEM_BLUE:
        snprintf(buffer, max_len, "%u", keylight_tui.current_b);
        break;
    default:
        strlcpy(buffer, "-", max_len);
        break;
    }
}

static void keylight_tui_render(void)
{
    solar_os_tui_t *tui = &keylight_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);

    if (rows == 0 || cols == 0) {
        return;
    }

    solar_os_tui_clear(tui);

    size_t split = cols / 3;
    if (split < 12) {
        split = 12;
    }
    if (split + 1 >= cols) {
        split = cols > 2 ? cols / 2 : 1;
    }

    solar_os_tui_write_cell(tui, 0, 0, split, "keylight",
                           SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    if (cols > split) {
        solar_os_tui_vrule(tui, 0, split, rows, 1, SOLAR_OS_TUI_ATTR_NORMAL);
        solar_os_tui_write_cell(tui, 0, split + 1,
                               keylight_tui_visible_width(cols, split + 1),
                               "setting",
                               SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    }

    const size_t value_col = split + 1;
    const size_t value_width = keylight_tui_visible_width(cols, value_col);
    const size_t visible_items = rows > 2 ? rows - 2 : 0;

    if (keylight_tui.selected < keylight_tui.first_visible) {
        keylight_tui.first_visible = keylight_tui.selected;
    } else if (visible_items > 0 &&
               keylight_tui.selected >= keylight_tui.first_visible + visible_items) {
        keylight_tui.first_visible = keylight_tui.selected - visible_items + 1;
    }

    for (size_t row = 0; row < visible_items; row++) {
        const size_t i = keylight_tui.first_visible + row;
        if (i >= KEYLIGHT_ITEM_COUNT) {
            break;
        }

        char value[KEYLIGHT_TUI_EDIT_MAX];
        uint8_t label_attr = SOLAR_OS_TUI_ATTR_NORMAL;
        uint8_t value_attr = SOLAR_OS_TUI_ATTR_NORMAL;

        if (i == keylight_tui.selected) {
            label_attr = SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE;
            value_attr = SOLAR_OS_TUI_ATTR_INVERSE;
        }

        keylight_format_item_value((keylight_item_t)i, value, sizeof(value));
        if (keylight_tui.editing && i == keylight_tui.selected) {
            strlcpy(value, keylight_tui.edit_text, sizeof(value));
            value_attr = SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE;
        }

        solar_os_tui_write_cell(tui, row + 1, 0, split,
                               keylight_items[i].label, label_attr);
        if (value_width > 0) {
            solar_os_tui_write_cell(tui, row + 1, value_col, value_width,
                                   value, value_attr);
        }
    }

    if (rows > 1) {
        const char *help = keylight_tui.status[0] != '\0' ? keylight_tui.status :
            (solar_os_m5tab5_keyboard_is_attached() ?
             "arrows adjust  enter edit  esc exits" :
             "warning: keyboard offline (STM32 @ 0x6D not responding)");
        solar_os_tui_draw_help(tui, help);
    }

    if (keylight_tui.editing && value_width > 0 && visible_items > 0) {
        const size_t edit_len = strlen(keylight_tui.edit_text);
        size_t cursor_col = value_col + edit_len;
        if (cursor_col >= cols) {
            cursor_col = cols - 1;
        }
        solar_os_tui_move(tui,
                          keylight_tui.selected - keylight_tui.first_visible + 1,
                          cursor_col);
    }

    solar_os_tui_set_cursor_visible(tui, keylight_tui.editing && keylight_tui.cursor_visible);
    solar_os_tui_refresh(tui);
}

static void keylight_tui_reset_cursor_blink(void)
{
    keylight_tui.cursor_visible = true;
    keylight_tui.last_cursor_blink_ms = 0;
}

static void keylight_tui_begin_edit(void)
{
    if (!keylight_items[keylight_tui.selected].editable) {
        return;
    }
    switch ((keylight_item_t)keylight_tui.selected) {
    case KEYLIGHT_ITEM_BACKLIGHT:
        snprintf(keylight_tui.edit_text, sizeof(keylight_tui.edit_text), "%u", keylight_tui.current_backlight);
        break;
    case KEYLIGHT_ITEM_HEX:
        snprintf(keylight_tui.edit_text, sizeof(keylight_tui.edit_text), "#%02X%02X%02X",
                 keylight_tui.current_r, keylight_tui.current_g, keylight_tui.current_b);
        break;
    case KEYLIGHT_ITEM_RED:
        snprintf(keylight_tui.edit_text, sizeof(keylight_tui.edit_text), "%u", keylight_tui.current_r);
        break;
    case KEYLIGHT_ITEM_GREEN:
        snprintf(keylight_tui.edit_text, sizeof(keylight_tui.edit_text), "%u", keylight_tui.current_g);
        break;
    case KEYLIGHT_ITEM_BLUE:
        snprintf(keylight_tui.edit_text, sizeof(keylight_tui.edit_text), "%u", keylight_tui.current_b);
        break;
    default:
        return;
    }
    strlcpy(keylight_tui.original_text, keylight_tui.edit_text, sizeof(keylight_tui.original_text));
    keylight_tui.editing = true;
    keylight_tui_reset_cursor_blink();
    keylight_tui_set_status("");
    keylight_tui_render();
}

static bool keylight_tui_apply_edit(void)
{
    switch ((keylight_item_t)keylight_tui.selected) {
    case KEYLIGHT_ITEM_BACKLIGHT: {
        size_t percent = 0;
        if (!solar_os_shell_parse_size_arg(keylight_tui.edit_text, 0, 100, &percent)) {
            return false;
        }
        keylight_tui.current_backlight = (uint8_t)percent;
        (void)solar_os_m5tab5_keyboard_set_backlight(keylight_tui.current_backlight);
        return true;
    }
    case KEYLIGHT_ITEM_HEX: {
        uint32_t rgb888 = 0;
        if (!solar_os_shell_parse_rgb888(keylight_tui.edit_text, &rgb888)) {
            return false;
        }
        keylight_tui.current_r = (uint8_t)((rgb888 >> 16) & 0xFF);
        keylight_tui.current_g = (uint8_t)((rgb888 >> 8) & 0xFF);
        keylight_tui.current_b = (uint8_t)(rgb888 & 0xFF);
        keylight_tui.current_rgb_mode = 1U;
        keylight_tui.current_preset = keylight_find_matching_preset(keylight_tui.current_r,
                                                                    keylight_tui.current_g,
                                                                    keylight_tui.current_b);
        (void)solar_os_m5tab5_keyboard_set_rgb(keylight_tui.current_r,
                                              keylight_tui.current_g,
                                              keylight_tui.current_b);
        return true;
    }
    case KEYLIGHT_ITEM_RED:
    case KEYLIGHT_ITEM_GREEN:
    case KEYLIGHT_ITEM_BLUE: {
        size_t val = 0;
        if (!solar_os_shell_parse_size_arg(keylight_tui.edit_text, 0, 255, &val)) {
            return false;
        }
        if (keylight_tui.selected == KEYLIGHT_ITEM_RED) {
            keylight_tui.current_r = (uint8_t)val;
        } else if (keylight_tui.selected == KEYLIGHT_ITEM_GREEN) {
            keylight_tui.current_g = (uint8_t)val;
        } else {
            keylight_tui.current_b = (uint8_t)val;
        }
        keylight_tui.current_rgb_mode = 1U;
        keylight_tui.current_preset = keylight_find_matching_preset(keylight_tui.current_r,
                                                                    keylight_tui.current_g,
                                                                    keylight_tui.current_b);
        (void)solar_os_m5tab5_keyboard_set_rgb(keylight_tui.current_r,
                                              keylight_tui.current_g,
                                              keylight_tui.current_b);
        return true;
    }
    default:
        return false;
    }
}

static void keylight_tui_commit_edit(void)
{
    if (keylight_tui_apply_edit()) {
        keylight_tui.editing = false;
        keylight_tui.cursor_visible = false;
        keylight_tui_set_status("applied & saved to nvs");
    } else {
        keylight_tui_reset_cursor_blink();
        keylight_tui_set_status("invalid value");
    }
    keylight_tui_render();
}

static void keylight_tui_cancel_edit(void)
{
    strlcpy(keylight_tui.edit_text, keylight_tui.original_text, sizeof(keylight_tui.edit_text));
    keylight_tui.editing = false;
    keylight_tui.cursor_visible = false;
    keylight_tui_set_status("");
    keylight_tui_render();
}

static void keylight_tui_cycle(int direction)
{
    switch ((keylight_item_t)keylight_tui.selected) {
    case KEYLIGHT_ITEM_BACKLIGHT: {
        int b = (int)keylight_tui.current_backlight + (direction * 5);
        if (b < 0) b = 0;
        if (b > 100) b = 100;
        keylight_tui.current_backlight = (uint8_t)b;
        (void)solar_os_m5tab5_keyboard_set_backlight(keylight_tui.current_backlight);
        keylight_tui_set_status("backlight updated");
        break;
    }
    case KEYLIGHT_ITEM_RGB_MODE: {
        keylight_tui.current_rgb_mode = (keylight_tui.current_rgb_mode == 1U) ? 0U : 1U;
        (void)solar_os_m5tab5_keyboard_set_rgb_mode(keylight_tui.current_rgb_mode);
        keylight_tui_set_status(keylight_tui.current_rgb_mode == 1U ? "rgb mode: custom" : "rgb mode: auto");
        break;
    }
    case KEYLIGHT_ITEM_PRESET: {
        int next_preset = keylight_tui.current_preset + direction;
        if (next_preset < 0) {
            next_preset = (int)COLOR_PRESETS_COUNT - 1;
        } else if ((size_t)next_preset >= COLOR_PRESETS_COUNT) {
            next_preset = 0;
        }
        keylight_tui.current_preset = next_preset;
        keylight_tui.current_r = color_presets[next_preset].r;
        keylight_tui.current_g = color_presets[next_preset].g;
        keylight_tui.current_b = color_presets[next_preset].b;
        keylight_tui.current_rgb_mode = 1U;
        (void)solar_os_m5tab5_keyboard_set_rgb(keylight_tui.current_r,
                                              keylight_tui.current_g,
                                              keylight_tui.current_b);
        keylight_tui_set_status("preset applied");
        break;
    }
    case KEYLIGHT_ITEM_RED: {
        int r = (int)keylight_tui.current_r + (direction * 10);
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        keylight_tui.current_r = (uint8_t)r;
        keylight_tui.current_rgb_mode = 1U;
        keylight_tui.current_preset = keylight_find_matching_preset(keylight_tui.current_r,
                                                                    keylight_tui.current_g,
                                                                    keylight_tui.current_b);
        (void)solar_os_m5tab5_keyboard_set_rgb(keylight_tui.current_r,
                                              keylight_tui.current_g,
                                              keylight_tui.current_b);
        keylight_tui_set_status("color updated");
        break;
    }
    case KEYLIGHT_ITEM_GREEN: {
        int g = (int)keylight_tui.current_g + (direction * 10);
        if (g < 0) g = 0;
        if (g > 255) g = 255;
        keylight_tui.current_g = (uint8_t)g;
        keylight_tui.current_rgb_mode = 1U;
        keylight_tui.current_preset = keylight_find_matching_preset(keylight_tui.current_r,
                                                                    keylight_tui.current_g,
                                                                    keylight_tui.current_b);
        (void)solar_os_m5tab5_keyboard_set_rgb(keylight_tui.current_r,
                                              keylight_tui.current_g,
                                              keylight_tui.current_b);
        keylight_tui_set_status("color updated");
        break;
    }
    case KEYLIGHT_ITEM_BLUE: {
        int bl = (int)keylight_tui.current_b + (direction * 10);
        if (bl < 0) bl = 0;
        if (bl > 255) bl = 255;
        keylight_tui.current_b = (uint8_t)bl;
        keylight_tui.current_rgb_mode = 1U;
        keylight_tui.current_preset = keylight_find_matching_preset(keylight_tui.current_r,
                                                                    keylight_tui.current_g,
                                                                    keylight_tui.current_b);
        (void)solar_os_m5tab5_keyboard_set_rgb(keylight_tui.current_r,
                                              keylight_tui.current_g,
                                              keylight_tui.current_b);
        keylight_tui_set_status("color updated");
        break;
    }
    case KEYLIGHT_ITEM_HEX:
        /* Hex isn't cycled; pressing Enter edits it */
        break;
    default:
        break;
    }
    keylight_tui_render();
}

static void keylight_tui_handle_edit_key(char ch)
{
    const uint8_t key = (uint8_t)ch;
    const size_t len = strlen(keylight_tui.edit_text);

    keylight_tui_reset_cursor_blink();

    switch (key) {
    case SOLAR_OS_KEY_ESCAPE:
        keylight_tui_cancel_edit();
        return;
    case '\r':
    case '\n':
        keylight_tui_commit_edit();
        return;
    case '\b':
    case 0x7FU:
    case SOLAR_OS_KEY_DELETE:
        if (len > 0) {
            keylight_tui.edit_text[len - 1] = '\0';
            keylight_tui_set_status("");
            keylight_tui_render();
        }
        return;
    default:
        break;
    }

    if (isprint((unsigned char)ch) && len + 1 < sizeof(keylight_tui.edit_text)) {
        keylight_tui.edit_text[len] = ch;
        keylight_tui.edit_text[len + 1] = '\0';
        keylight_tui_set_status("");
        keylight_tui_render();
    }
}

static esp_err_t keylight_tui_start(solar_os_context_t *ctx)
{
    memset(&keylight_tui, 0, sizeof(keylight_tui));
    keylight_tui.ctx = ctx;
    keylight_sync_state_from_hardware();

    const esp_err_t err = solar_os_tui_screen_begin(&keylight_tui.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    solar_os_tui_set_cursor_visible(&keylight_tui.tui, false);
    keylight_tui_render();
    return ESP_OK;
}

static void keylight_tui_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&keylight_tui.tui, true);
    solar_os_tui_clear(&keylight_tui.tui);
    solar_os_tui_refresh(&keylight_tui.tui);
    solar_os_tui_end(&keylight_tui.tui);
}

static bool keylight_tui_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (!keylight_tui.editing) {
            return false;
        }

        const uint32_t now_ms = event->data.tick_ms;
        if (keylight_tui.last_cursor_blink_ms == 0) {
            keylight_tui.last_cursor_blink_ms = now_ms;
            return true;
        }
        if ((now_ms - keylight_tui.last_cursor_blink_ms) >= KEYLIGHT_TUI_CURSOR_BLINK_MS) {
            keylight_tui.last_cursor_blink_ms = now_ms;
            keylight_tui.cursor_visible = !keylight_tui.cursor_visible;
            solar_os_tui_set_cursor_visible(&keylight_tui.tui, keylight_tui.cursor_visible);
        }
        return true;
    }

    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_finish(keylight_tui.ctx, 0, NULL);
        return true;
    }

    if (keylight_tui.editing) {
        keylight_tui_handle_edit_key(event->data.ch);
        return true;
    }

    switch (key) {
    case SOLAR_OS_KEY_UP:
        if (keylight_tui.selected > 0) {
            keylight_tui.selected--;
            keylight_tui_set_status("");
            keylight_tui_render();
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        if (keylight_tui.selected + 1 < KEYLIGHT_ITEM_COUNT) {
            keylight_tui.selected++;
            keylight_tui_set_status("");
            keylight_tui_render();
        }
        break;
    case SOLAR_OS_KEY_LEFT:
    case '-':
        keylight_tui_cycle(-1);
        break;
    case SOLAR_OS_KEY_RIGHT:
    case '+':
    case '=':
        keylight_tui_cycle(1);
        break;
    case '\r':
    case '\n':
        keylight_tui_begin_edit();
        break;
    case 'q':
    case 'Q':
    case SOLAR_OS_KEY_ESCAPE:
        solar_os_context_finish(keylight_tui.ctx, 0, NULL);
        break;
    default:
        break;
    }

    return true;
}

const solar_os_app_t solar_os_keylight_app = {
    .name = "keylight",
    .summary = "keyboard backlight and RGB LED control",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .start = keylight_tui_start,
    .stop = keylight_tui_stop,
    .event = keylight_tui_event,
    .state_slot = &keylight_tui_state,
    .state_size = sizeof(keylight_tui_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
};

esp_err_t solar_os_shell_launch_keylight_tui(solar_os_context_t *ctx)
{
    return solar_os_context_request_launch(ctx, &solar_os_keylight_app, 0, NULL);
}

void solar_os_shell_cmd_keylight(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = solar_os_shell_command_io(ctx);

    if (argc <= 1) {
        const esp_err_t err = solar_os_shell_launch_keylight_tui(ctx);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "keylight: launch failed: %s\n", solar_os_shell_error_text(err));
        } else {
            solar_os_shell_session_prepare_foreground_launch(ctx, true);
        }
        return;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "help") == 0 || strcmp(subcmd, "--help") == 0 || strcmp(subcmd, "-h") == 0) {
        solar_os_shell_io_writeln(term, "usage: kbd [command] [args...]");
        solar_os_shell_io_writeln(term, "  kbd                              launch interactive settings");
        solar_os_shell_io_writeln(term, "  kbd status                       show current lighting status");
        solar_os_shell_io_writeln(term, "  kbd brightness <0..100>          set backlight brightness percent");
        solar_os_shell_io_writeln(term, "  kbd color <#hex|r g b|off|auto>  set RGB LED color or mode");
        solar_os_shell_io_writeln(term, "  kbd off                          turn off backlight and RGB LEDs");
        return;
    }

    if (strcmp(subcmd, "status") == 0) {
        const bool attached = solar_os_m5tab5_keyboard_is_attached();
        uint8_t bl = 0;
        uint8_t mode = 0;
        uint8_t r = 0, g = 0, b = 0;
        (void)solar_os_m5tab5_keyboard_get_backlight(&bl);
        (void)solar_os_m5tab5_keyboard_get_rgb_mode(&mode);
        (void)solar_os_m5tab5_keyboard_get_rgb(&r, &g, &b);

        solar_os_shell_io_printf(term, "keyboard: %s\n",
                                 attached ? "M5Tab5 Hardware Keyboard (STM32 @ 0x6D)" : "not detected");
        solar_os_shell_io_printf(term, "status:   %s\n", attached ? "attached" : "disconnected");
        solar_os_shell_io_printf(term, "backlight: %u%%\n", (unsigned)bl);
        solar_os_shell_io_printf(term, "rgb mode:  %s\n", mode == 1U ? "custom" : "auto");
        solar_os_shell_io_printf(term, "rgb color: #%02X%02X%02X (R: %u, G: %u, B: %u)\n",
                                 (unsigned)r, (unsigned)g, (unsigned)b,
                                 (unsigned)r, (unsigned)g, (unsigned)b);
        return;
    }

    if (strcmp(subcmd, "off") == 0) {
        (void)solar_os_m5tab5_keyboard_set_backlight(0);
        (void)solar_os_m5tab5_keyboard_set_rgb(0, 0, 0);
        solar_os_shell_io_writeln(term, "keyboard backlight and RGB LEDs turned off");
        return;
    }

    if (strcmp(subcmd, "brightness") == 0 ||
        strcmp(subcmd, "backlight") == 0 ||
        strcmp(subcmd, "b") == 0) {
        if (argc < 3) {
            solar_os_shell_diag_missing(term, "kbd brightness", "percent", "kbd brightness <0..100>");
            return;
        }
        size_t val = 0;
        if (!solar_os_shell_parse_size_arg(argv[2], 0, 100, &val)) {
            solar_os_shell_diag_invalid(term, "kbd brightness", "percent", argv[2], "0..100", "kbd brightness <0..100>", false);
            return;
        }
        const esp_err_t err = solar_os_m5tab5_keyboard_set_backlight((uint8_t)val);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "kbd: failed to set brightness: %s\n", solar_os_shell_error_text(err));
        } else {
            solar_os_shell_io_printf(term, "keyboard backlight set to %u%%\n", (unsigned)val);
        }
        return;
    }

    if (strcmp(subcmd, "color") == 0 ||
        strcmp(subcmd, "c") == 0 ||
        strcmp(subcmd, "rgb") == 0) {
        if (argc < 3) {
            solar_os_shell_diag_missing(term, "kbd color", "color", "kbd color <#hex | r g b | off | auto>");
            return;
        }

        const char *arg = argv[2];

        if (strcmp(arg, "off") == 0) {
            const esp_err_t err = solar_os_m5tab5_keyboard_set_rgb(0, 0, 0);
            if (err != ESP_OK) {
                solar_os_shell_io_printf(term, "kbd: failed to turn off RGB: %s\n", solar_os_shell_error_text(err));
            } else {
                solar_os_shell_io_writeln(term, "keyboard RGB LEDs turned off");
            }
            return;
        }

        if (strcmp(arg, "auto") == 0) {
            const esp_err_t err = solar_os_m5tab5_keyboard_set_rgb_mode(0);
            if (err != ESP_OK) {
                solar_os_shell_io_printf(term, "kbd: failed to set RGB auto mode: %s\n", solar_os_shell_error_text(err));
            } else {
                solar_os_shell_io_writeln(term, "keyboard RGB set to auto/binding mode");
            }
            return;
        }

        /* Check for 3 separate arguments: r g b */
        if (argc >= 5) {
            size_t r = 0, g = 0, b = 0;
            if (!solar_os_shell_parse_size_arg(argv[2], 0, 255, &r) ||
                !solar_os_shell_parse_size_arg(argv[3], 0, 255, &g) ||
                !solar_os_shell_parse_size_arg(argv[4], 0, 255, &b)) {
                solar_os_shell_diag_invalid(term, "kbd color", "rgb", argv[2], "0..255 0..255 0..255", "kbd color <r> <g> <b>", false);
                return;
            }
            const esp_err_t err = solar_os_m5tab5_keyboard_set_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
            if (err != ESP_OK) {
                solar_os_shell_io_printf(term, "kbd: failed to set color: %s\n", solar_os_shell_error_text(err));
            } else {
                solar_os_shell_io_printf(term, "keyboard RGB LED set to #%02X%02X%02X\n",
                                         (unsigned)r, (unsigned)g, (unsigned)b);
            }
            return;
        }

        /* Check preset names */
        for (size_t i = 0; i < COLOR_PRESETS_COUNT; i++) {
            if (strcasecmp(arg, color_presets[i].name) == 0) {
                const esp_err_t err = solar_os_m5tab5_keyboard_set_rgb(color_presets[i].r,
                                                                      color_presets[i].g,
                                                                      color_presets[i].b);
                if (err != ESP_OK) {
                    solar_os_shell_io_printf(term, "kbd: failed to set color: %s\n", solar_os_shell_error_text(err));
                } else {
                    solar_os_shell_io_printf(term, "keyboard RGB LED set to preset '%s' (#%02X%02X%02X)\n",
                                             color_presets[i].name,
                                             (unsigned)color_presets[i].r,
                                             (unsigned)color_presets[i].g,
                                             (unsigned)color_presets[i].b);
                }
                return;
            }
        }

        /* Try parsing as hex: #RRGGBB, 0xRRGGBB, or RRGGBB */
        uint32_t rgb888 = 0;
        if (solar_os_shell_parse_rgb888(arg, &rgb888)) {
            const uint8_t r = (uint8_t)((rgb888 >> 16) & 0xFF);
            const uint8_t g = (uint8_t)((rgb888 >> 8) & 0xFF);
            const uint8_t b = (uint8_t)(rgb888 & 0xFF);
            const esp_err_t err = solar_os_m5tab5_keyboard_set_rgb(r, g, b);
            if (err != ESP_OK) {
                solar_os_shell_io_printf(term, "kbd: failed to set color: %s\n", solar_os_shell_error_text(err));
            } else {
                solar_os_shell_io_printf(term, "keyboard RGB LED set to #%06" PRIX32 "\n", rgb888);
            }
            return;
        }

        solar_os_shell_diag_invalid(term, "kbd color", "color", arg,
                                    "#hex, 0..255 0..255 0..255, preset, off, auto",
                                    "kbd color <#hex | r g b | off | auto>", false);
        return;
    }

    solar_os_shell_diag_unknown(term, "kbd", "subcommand", subcmd, "help",
                                "kbd [brightness|color|off|status|help]");
}
