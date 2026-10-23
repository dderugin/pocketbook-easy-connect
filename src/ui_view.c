#include "ui.h"
#include <string.h>

static int screen_x(int logical_x) {
    return logical_x * ScreenWidth() / UI_CANVAS_WIDTH;
}

static int screen_y(int logical_y) {
    return logical_y * ScreenHeight() / UI_CANVAS_HEIGHT;
}

static void draw_text_in_rect(ui_rect bounds, const char *value, ifont *font, int color) {
    SetFont(font, color);
    DrawTextRect(screen_x(bounds.x), screen_y(bounds.y), screen_x(bounds.width),
                 screen_y(bounds.height), value, ALIGN_LEFT);
}

static void draw_text(ui_text_id area, const char *value, ifont *font) {
    draw_text_in_rect(ui_text_bounds(area), value, font, BLACK);
}

static void draw_button(const ui_app *app, ui_control_id control, const char *label, bool selected,
                        bool enabled) {
    ui_rect bounds = ui_control_bounds(control);
    if (selected) {
        FillArea(screen_x(bounds.x), screen_y(bounds.y), screen_x(bounds.width),
                 screen_y(bounds.height), BLACK);
    } else {
        DrawRect(screen_x(bounds.x), screen_y(bounds.y), screen_x(bounds.width),
                 screen_y(bounds.height), enabled ? BLACK : LGRAY);
    }

    ui_rect label_bounds = {bounds.x + UI_BUTTON_TEXT_INSET_X, bounds.y + UI_BUTTON_TEXT_INSET_Y,
                            bounds.width - 2 * UI_BUTTON_TEXT_INSET_X,
                            bounds.height - UI_BUTTON_TEXT_VERTICAL_TRIM};
    int label_color = enabled ? BLACK : DGRAY;
    if (selected) {
        label_color = WHITE;
    }
    draw_text_in_rect(label_bounds, label, app->fonts.body, label_color);
}

static void draw_header(const ui_app *app) {
    char line[EC_PATH + UI_FINGERPRINT_CAPACITY];
    bool running = ui_is_running(app, app->active_tab);

    draw_text(UI_TEXT_TITLE, "Easy Connect", app->fonts.title);
    snprintf(line, sizeof line, "Device address: %s", app->device_address);
    draw_text(UI_TEXT_ADDRESS, line, app->fonts.small);
    draw_button(app, UI_SSH_TAB, "SSH", app->active_tab == UI_TAB_SSH, true);
    draw_button(app, UI_FTP_TAB, "FTP", app->active_tab == UI_TAB_FTP, true);
    draw_text(UI_TEXT_SERVICE_TITLE, app->active_tab == UI_TAB_SSH ? "SSH daemon" : "FTP daemon",
              app->fonts.title);
    draw_text(UI_TEXT_STATUS, running ? "RUNNING" : "STOPPED", app->fonts.body);

    if (app->active_tab == UI_TAB_SSH) {
        snprintf(line, sizeof line, "ssh -p %u %s@%s", app->config.ssh_port, app->ssh_user,
                 app->device_address);
    } else {
        const char *user =
            app->config.ftp_auth == EC_FTP_ANONYMOUS ? "anonymous" : app->config.ftp_user;
        snprintf(line, sizeof line, "ftp://%s@%s:%u/", user, app->device_address,
                 app->config.ftp_port);
    }
    draw_text(UI_TEXT_CONNECTION, line, app->fonts.body);
    draw_button(app, UI_START, "Start", false, !running);
    draw_button(app, UI_STOP, "Stop", false, running);
    draw_text(UI_TEXT_ACCESS_TYPE, "Access type", app->fonts.body);
}

static void draw_password_status(const ui_app *app, ui_tab tab) {
    bool configured = ec_has_password(&app->config, ui_service_name(tab));
    ui_text_id area = tab == UI_TAB_SSH ? UI_TEXT_SSH_PASSWORD_STATUS : UI_TEXT_FTP_PASSWORD_STATUS;
    ui_control_id button = tab == UI_TAB_SSH ? UI_SSH_SET_PASSWORD : UI_FTP_SET_PASSWORD;
    draw_text(area, configured ? "Password: configured" : "Password: not set", app->fonts.body);
    draw_button(app, button, "Set password", false, !ui_is_running(app, tab));
}

static void draw_ssh_settings(const ui_app *app) {
    const ec_config *config = &app->config;
    bool editable = !ui_is_running(app, UI_TAB_SSH);
    char line[EC_PATH + UI_FINGERPRINT_CAPACITY];

    draw_button(app, UI_SSH_KEYS, "Public key", config->auth == EC_KEYS, editable);
    draw_button(app, UI_SSH_PASSWORD, "Password", config->auth == EC_PASSWORD, editable);
    draw_button(app, UI_SSH_BOTH, "Key or password", config->auth == EC_BOTH, editable);
    snprintf(line, sizeof line, "SSH port: %u    Edit", config->ssh_port);
    draw_button(app, UI_SSH_PORT, line, false, editable);

    if (ui_control_visible(UI_SSH_SET_PASSWORD, UI_TAB_SSH, config)) {
        draw_password_status(app, UI_TAB_SSH);
    } else {
        draw_text(UI_TEXT_SSH_KEY_HINT, "Connect with your private SSH key.", app->fonts.body);
    }
    if (config->auth != EC_PASSWORD) {
        snprintf(line, sizeof line, "Public keys: %u entries", ec_key_count(config));
        draw_text(UI_TEXT_SSH_KEY_COUNT, line, app->fonts.body);
        snprintf(line, sizeof line, "Copy .pub keys to:\n%s/authorized_keys", config->state);
        draw_text(UI_TEXT_SSH_KEY_PATH, line, app->fonts.small);
    } else {
        draw_text(UI_TEXT_SSH_PASSWORD_HINT,
                  "Use the device account shown above and your Easy Connect SSH password.",
                  app->fonts.body);
    }
    snprintf(line, sizeof line, "Device fingerprint\n%s", app->fingerprint);
    draw_text(UI_TEXT_FINGERPRINT, line, app->fonts.small);
}

static void draw_ftp_settings(const ui_app *app) {
    const ec_config *config = &app->config;
    bool editable = !ui_is_running(app, UI_TAB_FTP);
    char line[EC_PATH + UI_FINGERPRINT_CAPACITY];

    draw_button(app, UI_FTP_PASSWORD, "Password", config->ftp_auth == EC_FTP_PASSWORD, editable);
    draw_button(app, UI_FTP_ANONYMOUS, "No password", config->ftp_auth == EC_FTP_ANONYMOUS,
                editable);
    snprintf(line, sizeof line, "Port: %u", config->ftp_port);
    draw_button(app, UI_FTP_PORT, line, false, editable);
    snprintf(line, sizeof line, "Passive: %u", config->pasv_min);
    draw_button(app, UI_FTP_PASSIVE_MIN, line, false, editable);
    snprintf(line, sizeof line, "to %u", config->pasv_max);
    draw_button(app, UI_FTP_PASSIVE_MAX, line, false, editable);
    snprintf(line, sizeof line, "Starting folder: %s", config->ftp_root);
    draw_button(app, UI_FTP_ROOT, line, false, editable);

    if (ui_control_visible(UI_FTP_SET_PASSWORD, UI_TAB_FTP, config)) {
        draw_password_status(app, UI_TAB_FTP);
        draw_text(UI_TEXT_FTP_PASSWORD_HINT,
                  "Login uses the account shown above. FTP transfers are unencrypted.",
                  app->fonts.small);
    } else {
        draw_text(UI_TEXT_FTP_ANONYMOUS_HINT,
                  "Login as anonymous; no password needed. Anyone on your network can access these "
                  "files.",
                  app->fonts.body);
    }
    if (app->ftp_rootless) {
        draw_text(UI_TEXT_FTP_PERMISSIONS,
                  "Access follows this user's file permissions; no folder jail.", app->fonts.small);
    }
}

static void draw_footer(const ui_app *app) {
    const char *notice = app->notices[app->active_tab];
    draw_text(UI_TEXT_NOTICE, *notice ? notice : "Stop this daemon before changing its settings.",
              app->fonts.small);
    draw_button(app, UI_SAVE_DIAGNOSTICS, "Save diagnostics", false, true);
    draw_button(app, UI_EXIT, "Exit", false, true);
}

void ui_draw(const ui_app *app) {
    if (app->editor.field != UI_EDIT_NONE || !app->fonts.body) {
        return;
    }
    ClearScreen();
    draw_header(app);
    if (app->active_tab == UI_TAB_SSH) {
        draw_ssh_settings(app);
    } else {
        draw_ftp_settings(app);
    }
    draw_footer(app);
    FullUpdate();
}
