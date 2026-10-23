#ifndef EASY_CONNECT_UI_LAYOUT_H
#define EASY_CONNECT_UI_LAYOUT_H

#include "ec.h"
#include <stdbool.h>

/* Geometry uses logical pixels; the InkView adapter scales to the device. */
enum {
    UI_CANVAS_WIDTH = 720,
    UI_CANVAS_HEIGHT = 960,
    UI_BODY_FONT_SIZE = 20,
    UI_SMALL_FONT_PERCENT = 80,
    UI_TITLE_FONT_PERCENT = 150,
    UI_BUTTON_TEXT_INSET_X = 12,
    UI_BUTTON_TEXT_INSET_Y = 12,
    UI_BUTTON_TEXT_VERTICAL_TRIM = 16,
    UI_STATUS_REFRESH_MS = 1000
};

typedef enum { UI_TAB_SSH, UI_TAB_FTP, UI_TAB_COUNT } ui_tab;

typedef struct {
    int x;
    int y;
    int width;
    int height;
} ui_rect;

typedef enum {
    UI_CONTROL_NONE,
    UI_SSH_TAB,
    UI_FTP_TAB,
    UI_START,
    UI_STOP,
    UI_SSH_KEYS,
    UI_SSH_PASSWORD,
    UI_SSH_BOTH,
    UI_SSH_PORT,
    UI_SSH_SET_PASSWORD,
    UI_FTP_PASSWORD,
    UI_FTP_ANONYMOUS,
    UI_FTP_PORT,
    UI_FTP_PASSIVE_MIN,
    UI_FTP_PASSIVE_MAX,
    UI_FTP_ROOT,
    UI_FTP_SET_PASSWORD,
    UI_SAVE_DIAGNOSTICS,
    UI_EXIT,
    UI_CONTROL_COUNT
} ui_control_id;

typedef enum {
    UI_TEXT_TITLE,
    UI_TEXT_ADDRESS,
    UI_TEXT_SERVICE_TITLE,
    UI_TEXT_STATUS,
    UI_TEXT_CONNECTION,
    UI_TEXT_ACCESS_TYPE,
    UI_TEXT_SSH_PASSWORD_STATUS,
    UI_TEXT_SSH_KEY_HINT,
    UI_TEXT_SSH_KEY_COUNT,
    UI_TEXT_SSH_KEY_PATH,
    UI_TEXT_SSH_PASSWORD_HINT,
    UI_TEXT_FINGERPRINT,
    UI_TEXT_FTP_PASSWORD_STATUS,
    UI_TEXT_FTP_PASSWORD_HINT,
    UI_TEXT_FTP_ANONYMOUS_HINT,
    UI_TEXT_FTP_PERMISSIONS,
    UI_TEXT_NOTICE,
    UI_TEXT_COUNT
} ui_text_id;

ui_rect ui_control_bounds(ui_control_id id);
ui_rect ui_text_bounds(ui_text_id id);
ui_control_id ui_control_named(const char *name);
const char *ui_control_name(ui_control_id id);
bool ui_control_visible(ui_control_id id, ui_tab tab, const ec_config *config);
bool ui_rect_contains(ui_rect rect, int x, int y);
ui_control_id ui_hit_test(ui_tab tab, const ec_config *config, int x, int y);

#endif
