#ifndef EASY_CONNECT_UI_H
#define EASY_CONNECT_UI_H

#include "ui_layout.h"
#include <inkview.h>

enum { UI_NOTICE_CAPACITY = 256, UI_FINGERPRINT_CAPACITY = 128, UI_PASSWORD_MIN_CHARACTERS = 12 };

typedef enum {
    UI_EDIT_NONE,
    UI_EDIT_SSH_PORT,
    UI_EDIT_FTP_PORT,
    UI_EDIT_PASSIVE_MIN,
    UI_EDIT_PASSIVE_MAX,
    UI_EDIT_FTP_ROOT,
    UI_EDIT_PASSWORD,
    UI_EDIT_PASSWORD_CONFIRM
} ui_edit_field;

typedef struct {
    ifont *body;
    ifont *small;
    ifont *title;
} ui_fonts;

typedef struct {
    ui_edit_field field;
    ui_tab password_tab;
    char buffer[EC_PATH];
    char pending_password[EC_PASSWORD_MAX_BYTES + 1];
} ui_editor;

/* InkView callbacks share one application instance, rather than loose globals. */
typedef struct {
    ec_config config;
    ec_services services;
    bool services_open;
    ui_tab active_tab;
    ui_fonts fonts;
    ui_editor editor;
    char notices[UI_TAB_COUNT][UI_NOTICE_CAPACITY];
    char fingerprint[UI_FINGERPRINT_CAPACITY];
    char device_address[64];
    char ssh_user[64];
    bool ftp_rootless;
    bool network_available;
    bool foreground;
    bool library_handoff;
    pid_t library_refresh;
    int screen_width, screen_height;
} ui_app;

static inline const char *ui_service_name(ui_tab tab) {
    return tab == UI_TAB_SSH ? "ssh" : "ftp";
}

static inline bool ui_is_running(const ui_app *app, ui_tab tab) {
    return tab == UI_TAB_SSH ? app->services.ssh != 0 : app->services.ftp != 0;
}

void ui_draw(const ui_app *app);
int ui_library_start(ui_app *app);
/* 0 while running/idle, 1 on completion, -1 on failure. */
int ui_library_poll(ui_app *app);
void ui_library_stop(ui_app *app);

#endif
