#include "ui_layout.h"
#include <string.h>

enum {
    PAGE_MARGIN = 24,
    CONTENT_WIDTH = UI_CANVAS_WIDTH - 2 * PAGE_MARGIN,
    TAB_WIDTH = CONTENT_WIDTH / 2,
    TWO_COLUMN_GAP = 24,
    TWO_COLUMN_WIDTH = (CONTENT_WIDTH - TWO_COLUMN_GAP) / 2,
    THREE_COLUMN_GAP = 12,
    THREE_COLUMN_WIDTH = (CONTENT_WIDTH - 2 * THREE_COLUMN_GAP) / 3,
    COLUMN_LEFT = PAGE_MARGIN,
    COLUMN_MIDDLE = COLUMN_LEFT + THREE_COLUMN_WIDTH + THREE_COLUMN_GAP,
    COLUMN_RIGHT = COLUMN_MIDDLE + THREE_COLUMN_WIDTH + THREE_COLUMN_GAP,
    TWO_COLUMN_RIGHT = PAGE_MARGIN + TWO_COLUMN_WIDTH + TWO_COLUMN_GAP,
    TAB_TOP = 110,
    TAB_HEIGHT = 62,
    SERVICE_BUTTON_TOP = 312,
    SERVICE_BUTTON_HEIGHT = 62,
    AUTH_ROW_TOP = 440,
    AUTH_ROW_HEIGHT = 52,
    PORT_ROW_TOP = 514,
    PORT_ROW_HEIGHT = 54,
    FOOTER_TOP = 900,
    FOOTER_HEIGHT = 48
};

typedef enum { SCOPE_COMMON, SCOPE_SSH, SCOPE_FTP } control_scope;

typedef struct {
    const char *name;
    ui_rect bounds;
    control_scope scope;
} control_definition;

/* A control has one rectangle for both drawing and touch handling. */
static const control_definition controls[UI_CONTROL_COUNT] = {
    [UI_SSH_TAB] = {"ssh-tab", {PAGE_MARGIN, TAB_TOP, TAB_WIDTH, TAB_HEIGHT}, SCOPE_COMMON},
    [UI_FTP_TAB] = {"ftp-tab",
                    {PAGE_MARGIN + TAB_WIDTH, TAB_TOP, TAB_WIDTH, TAB_HEIGHT},
                    SCOPE_COMMON},
    [UI_START] = {"start",
                  {COLUMN_LEFT, SERVICE_BUTTON_TOP, TWO_COLUMN_WIDTH, SERVICE_BUTTON_HEIGHT},
                  SCOPE_COMMON},
    [UI_STOP] = {"stop",
                 {TWO_COLUMN_RIGHT, SERVICE_BUTTON_TOP, TWO_COLUMN_WIDTH, SERVICE_BUTTON_HEIGHT},
                 SCOPE_COMMON},
    [UI_SSH_KEYS] = {"ssh-keys",
                     {COLUMN_LEFT, AUTH_ROW_TOP, THREE_COLUMN_WIDTH, AUTH_ROW_HEIGHT},
                     SCOPE_SSH},
    [UI_SSH_PASSWORD] = {"ssh-password",
                         {COLUMN_MIDDLE, AUTH_ROW_TOP, THREE_COLUMN_WIDTH, AUTH_ROW_HEIGHT},
                         SCOPE_SSH},
    [UI_SSH_BOTH] = {"ssh-both",
                     {COLUMN_RIGHT, AUTH_ROW_TOP, THREE_COLUMN_WIDTH, AUTH_ROW_HEIGHT},
                     SCOPE_SSH},
    [UI_SSH_PORT] = {"ssh-port",
                     {PAGE_MARGIN, PORT_ROW_TOP, CONTENT_WIDTH, PORT_ROW_HEIGHT},
                     SCOPE_SSH},
    [UI_SSH_SET_PASSWORD] = {"ssh-set-password",
                             {COLUMN_RIGHT, 584, THREE_COLUMN_WIDTH, PORT_ROW_HEIGHT},
                             SCOPE_SSH},
    [UI_FTP_PASSWORD] = {"ftp-password",
                         {COLUMN_LEFT, AUTH_ROW_TOP, TWO_COLUMN_WIDTH, AUTH_ROW_HEIGHT},
                         SCOPE_FTP},
    [UI_FTP_ANONYMOUS] = {"ftp-anonymous",
                          {TWO_COLUMN_RIGHT, AUTH_ROW_TOP, TWO_COLUMN_WIDTH, AUTH_ROW_HEIGHT},
                          SCOPE_FTP},
    [UI_FTP_PORT] = {"ftp-port",
                     {COLUMN_LEFT, PORT_ROW_TOP, THREE_COLUMN_WIDTH, PORT_ROW_HEIGHT},
                     SCOPE_FTP},
    [UI_FTP_PASSIVE_MIN] = {"ftp-passive-min",
                            {COLUMN_MIDDLE, PORT_ROW_TOP, THREE_COLUMN_WIDTH, PORT_ROW_HEIGHT},
                            SCOPE_FTP},
    [UI_FTP_PASSIVE_MAX] = {"ftp-passive-max",
                            {COLUMN_RIGHT, PORT_ROW_TOP, THREE_COLUMN_WIDTH, PORT_ROW_HEIGHT},
                            SCOPE_FTP},
    [UI_FTP_ROOT] = {"ftp-root", {PAGE_MARGIN, 590, CONTENT_WIDTH, 70}, SCOPE_FTP},
    [UI_FTP_SET_PASSWORD] = {"ftp-set-password",
                             {COLUMN_RIGHT, 688, THREE_COLUMN_WIDTH, PORT_ROW_HEIGHT},
                             SCOPE_FTP},
    [UI_SAVE_DIAGNOSTICS] = {"save-diagnostics",
                             {COLUMN_LEFT, FOOTER_TOP, THREE_COLUMN_WIDTH, FOOTER_HEIGHT},
                             SCOPE_COMMON},
    [UI_REFRESH_LIBRARY] = {"refresh-library",
                            {COLUMN_MIDDLE, FOOTER_TOP, THREE_COLUMN_WIDTH, FOOTER_HEIGHT},
                            SCOPE_COMMON},
    [UI_EXIT] = {"exit", {COLUMN_RIGHT, FOOTER_TOP, THREE_COLUMN_WIDTH, FOOTER_HEIGHT}, SCOPE_COMMON}};

/* Text-only regions do not participate in hit testing. */
static const ui_rect text_regions[UI_TEXT_COUNT] = {
    [UI_TEXT_TITLE] = {PAGE_MARGIN, 20, CONTENT_WIDTH, 45},
    [UI_TEXT_ADDRESS] = {PAGE_MARGIN, 68, CONTENT_WIDTH, 32},
    [UI_TEXT_SERVICE_TITLE] = {PAGE_MARGIN, 198, 360, 40},
    [UI_TEXT_STATUS] = {472, 204, 224, 34},
    [UI_TEXT_CONNECTION] = {PAGE_MARGIN, 248, CONTENT_WIDTH, 48},
    [UI_TEXT_ACCESS_TYPE] = {PAGE_MARGIN, 400, CONTENT_WIDTH, 32},
    [UI_TEXT_SSH_PASSWORD_STATUS] = {PAGE_MARGIN, 598, 430, 36},
    [UI_TEXT_SSH_KEY_HINT] = {PAGE_MARGIN, 586, CONTENT_WIDTH, 52},
    [UI_TEXT_SSH_KEY_COUNT] = {PAGE_MARGIN, 656, CONTENT_WIDTH, 32},
    [UI_TEXT_SSH_KEY_PATH] = {PAGE_MARGIN, 698, CONTENT_WIDTH, 72},
    [UI_TEXT_SSH_PASSWORD_HINT] = {PAGE_MARGIN, 656, CONTENT_WIDTH, 88},
    [UI_TEXT_FINGERPRINT] = {PAGE_MARGIN, 770, CONTENT_WIDTH, 64},
    [UI_TEXT_FTP_PASSWORD_STATUS] = {PAGE_MARGIN, 702, 430, 36},
    [UI_TEXT_FTP_PASSWORD_HINT] = {PAGE_MARGIN, 764, CONTENT_WIDTH, 58},
    [UI_TEXT_FTP_ANONYMOUS_HINT] = {PAGE_MARGIN, 690, CONTENT_WIDTH, 70},
    [UI_TEXT_FTP_PERMISSIONS] = {PAGE_MARGIN, 828, CONTENT_WIDTH, 32},
    [UI_TEXT_NOTICE] = {PAGE_MARGIN, 850, CONTENT_WIDTH, 40}};

ui_rect ui_control_bounds(ui_control_id id) {
    if (id <= UI_CONTROL_NONE || id >= UI_CONTROL_COUNT) {
        return (ui_rect){0};
    }
    return controls[id].bounds;
}

ui_rect ui_text_bounds(ui_text_id id) {
    if (id < 0 || id >= UI_TEXT_COUNT) {
        return (ui_rect){0};
    }
    return text_regions[id];
}

const char *ui_control_name(ui_control_id id) {
    if (id <= UI_CONTROL_NONE || id >= UI_CONTROL_COUNT) {
        return NULL;
    }
    return controls[id].name;
}

ui_control_id ui_control_named(const char *name) {
    for (ui_control_id id = UI_CONTROL_NONE + 1; id < UI_CONTROL_COUNT; ++id) {
        if (!strcmp(controls[id].name, name)) {
            return id;
        }
    }
    return UI_CONTROL_NONE;
}

bool ui_control_visible(ui_control_id id, ui_tab tab, const ec_config *config) {
    if (id <= UI_CONTROL_NONE || id >= UI_CONTROL_COUNT) {
        return false;
    }
    if (controls[id].scope == SCOPE_SSH && tab != UI_TAB_SSH) {
        return false;
    }
    if (controls[id].scope == SCOPE_FTP && tab != UI_TAB_FTP) {
        return false;
    }
    if (id == UI_SSH_SET_PASSWORD && config->auth == EC_KEYS) {
        return false;
    }
    if (id == UI_FTP_SET_PASSWORD && config->ftp_auth == EC_FTP_ANONYMOUS) {
        return false;
    }
    return true;
}

bool ui_rect_contains(ui_rect rect, int x, int y) {
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

ui_control_id ui_hit_test(ui_tab tab, const ec_config *config, int x, int y) {
    for (ui_control_id id = UI_CONTROL_NONE + 1; id < UI_CONTROL_COUNT; ++id) {
        if (ui_control_visible(id, tab, config) && ui_rect_contains(controls[id].bounds, x, y)) {
            return id;
        }
    }
    return UI_CONTROL_NONE;
}
