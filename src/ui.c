/* InkView event handling, settings edits, and daemon lifecycle. */
#include "ui.h"
#include "device.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <openssl/crypto.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static ui_app app = {.active_tab = UI_TAB_SSH, .fingerprint = "Created when SSH starts."};
static bool rejected_device;

static void keyboard_result(char *value);
static void refresh_status(void);

static void set_notice(ui_tab tab, const char *message) {
    snprintf(app.notices[tab], sizeof app.notices[tab], "%s", message);
}

static void notify(const char *message) {
    set_notice(app.active_tab, message);
}

static bool read_device_address(char *out, size_t size) {
#ifdef EC_UI_TEST
    const char *address = getenv("EC_UI_ADDRESS");
    if (address) {
        snprintf(out, size, "%s", *address ? address : "not connected");
        return *address != '\0';
    }
#endif
    return ec_network_address(out, size);
}

static void redraw(void) {
    if (!app.foreground || app.modal || app.editor.field != UI_EDIT_NONE) {
        return;
    }
    app.network_available = read_device_address(app.device_address, sizeof app.device_address);
    ui_draw(&app);
}

static void update_fingerprint(void) {
    char info[1024];
    char path[EC_PATH];
    if (ec_path(path, sizeof path, app.config.state, "host_ed25519") || access(path, R_OK)) {
        return;
    }
    if (ec_key_info(&app.config, info, sizeof info)) {
        return;
    }
    char *hash = strstr(info, "SHA256:");
    if (hash) {
        hash[strcspn(hash, "\r\n")] = '\0';
        snprintf(app.fingerprint, sizeof app.fingerprint, "%.*s", (int)sizeof app.fingerprint - 1,
                 hash);
    }
}

static bool selected_service_is_editable(void) {
    if (ui_is_running(&app, app.active_tab)) {
        notify("Stop this daemon before changing its settings.");
        return false;
    }
    return true;
}

static void save_settings(const ec_config *candidate) {
    if (ec_config_save(candidate)) {
        notify(ec_last_error());
        return;
    }
    app.config = *candidate;
    notify("Settings saved.");
}

static void select_access_type(ui_control_id control) {
    if (!selected_service_is_editable()) {
        return;
    }
    ec_config candidate = app.config;
    switch (control) {
    case UI_SSH_KEYS:
        candidate.auth = EC_KEYS;
        break;
    case UI_SSH_PASSWORD:
        candidate.auth = EC_PASSWORD;
        break;
    case UI_SSH_BOTH:
        candidate.auth = EC_BOTH;
        break;
    case UI_FTP_PASSWORD:
        candidate.ftp_auth = EC_FTP_PASSWORD;
        break;
    case UI_FTP_ANONYMOUS:
        candidate.ftp_auth = EC_FTP_ANONYMOUS;
        break;
    default:
        return;
    }
    save_settings(&candidate);
}

static void clear_password_buffers(void) {
    OPENSSL_cleanse(app.editor.buffer, sizeof app.editor.buffer);
    OPENSSL_cleanse(app.editor.pending_password, sizeof app.editor.pending_password);
}

static void begin_edit(ui_edit_field field) {
    if (!selected_service_is_editable()) {
        return;
    }
    const char *title;
    int flags = KBD_NUMERIC;
    size_t capacity = sizeof app.editor.buffer;
    char *buffer = app.editor.buffer;

    switch (field) {
    case UI_EDIT_SSH_PORT:
        title = "SSH port";
        snprintf(buffer, capacity, "%u", app.config.ssh_port);
        break;
    case UI_EDIT_FTP_PORT:
        title = "FTP port";
        snprintf(buffer, capacity, "%u", app.config.ftp_port);
        break;
    case UI_EDIT_PASSIVE_MIN:
        title = "First passive port";
        snprintf(buffer, capacity, "%u", app.config.pasv_min);
        break;
    case UI_EDIT_PASSIVE_MAX:
        title = "Last passive port";
        snprintf(buffer, capacity, "%u", app.config.pasv_max);
        break;
    case UI_EDIT_FTP_ROOT:
        title = "FTP starting folder";
        snprintf(buffer, capacity, "%s", app.config.ftp_root);
        flags = 0;
        break;
    case UI_EDIT_PASSWORD:
        title = "New password (12+ characters)";
        app.editor.password_tab = app.active_tab;
        buffer[0] = '\0';
        flags = KBD_PASSWORD;
        capacity = sizeof app.editor.pending_password;
        break;
    case UI_EDIT_PASSWORD_CONFIRM:
        title = "Confirm new password";
        buffer[0] = '\0';
        flags = KBD_PASSWORD;
        capacity = sizeof app.editor.pending_password;
        break;
    default:
        return;
    }
    app.editor.field = field;
    OpenKeyboard(title, buffer, (int)capacity, flags, keyboard_result);
}

static bool password_input_is_valid(const char *value) {
    size_t characters = 0;
    for (const unsigned char *byte = (const unsigned char *)value; *byte; ++byte) {
        /* UTF-8 continuation bytes do not begin a character. */
        if ((*byte & 0xc0) != 0x80) {
            ++characters;
        }
    }
    return characters >= UI_PASSWORD_MIN_CHARACTERS && strlen(value) <= EC_PASSWORD_MAX_BYTES &&
           !strpbrk(value, "\r\n");
}

static void accept_new_password(const char *value) {
    if (!password_input_is_valid(value)) {
        notify("Use at least 12 characters, without line breaks.");
        clear_password_buffers();
        return;
    }
    memcpy(app.editor.pending_password, value, strlen(value) + 1);
    OPENSSL_cleanse(app.editor.buffer, sizeof app.editor.buffer);
    begin_edit(UI_EDIT_PASSWORD_CONFIRM);
}

static void confirm_new_password(const char *value) {
    if (strcmp(value, app.editor.pending_password)) {
        notify("Passwords did not match; password unchanged.");
    } else {
        const char *service = ui_service_name(app.editor.password_tab);
        if (ec_set_password(&app.config, service, app.editor.pending_password)) {
            notify(ec_last_error());
        } else {
            notify("Password saved.");
        }
    }
    clear_password_buffers();
}

static bool parse_edited_port(const char *value, unsigned *port) {
    char *end;
    errno = 0;
    unsigned long number = strtoul(value, &end, 10);
    if (!*value || strspn(value, "0123456789") != strlen(value) || *end || errno ||
        number < EC_PORT_MIN || number > EC_PORT_MAX) {
        notify("Port must be a number between 1024 and 65535.");
        return false;
    }
    *port = (unsigned)number;
    return true;
}

static void apply_setting_edit(ui_edit_field field, const char *value) {
    ec_config candidate = app.config;
    if (field == UI_EDIT_FTP_ROOT) {
        if (strlen(value) >= sizeof candidate.ftp_root) {
            notify("Folder path is too long.");
            return;
        }
        strcpy(candidate.ftp_root, value);
    } else {
        unsigned port;
        if (!parse_edited_port(value, &port)) {
            return;
        }
        switch (field) {
        case UI_EDIT_SSH_PORT:
            candidate.ssh_port = port;
            break;
        case UI_EDIT_FTP_PORT:
            candidate.ftp_port = port;
            break;
        case UI_EDIT_PASSIVE_MIN:
            candidate.pasv_min = port;
            break;
        case UI_EDIT_PASSIVE_MAX:
            candidate.pasv_max = port;
            break;
        default:
            return;
        }
    }
    save_settings(&candidate);
    OPENSSL_cleanse(app.editor.buffer, sizeof app.editor.buffer);
}

static void keyboard_result(char *value) {
    ui_edit_field field = app.editor.field;
    app.editor.field = UI_EDIT_NONE;
    if (!value) {
        clear_password_buffers();
        notify("Edit cancelled.");
    } else {
        switch (field) {
        case UI_EDIT_PASSWORD:
            accept_new_password(value);
            break;
        case UI_EDIT_PASSWORD_CONFIRM:
            confirm_new_password(value);
            break;
        case UI_EDIT_NONE:
            break;
        default:
            apply_setting_edit(field, value);
            break;
        }
    }
    redraw();
}

static void start_selected_service(void) {
    if (ui_is_running(&app, app.active_tab)) {
        return;
    }
    if (app.active_tab == UI_TAB_SSH && app.config.auth != EC_PASSWORD &&
        !ec_key_count(&app.config)) {
        notify("Add a .pub key at the path shown, or select Password and set one.");
        return;
    }
    bool needs_password = app.active_tab == UI_TAB_SSH ? app.config.auth != EC_KEYS
                                                      : app.config.ftp_auth == EC_FTP_PASSWORD;
    if (needs_password && !ec_has_password(&app.config, ui_service_name(app.active_tab))) {
        notify("Set a password, then press Start again.");
        begin_edit(UI_EDIT_PASSWORD);
        return;
    }
    char controller[EC_PATH];
    if (ec_path(controller, sizeof controller, app.config.root, "bin/easy-connect")) return;
#ifdef EC_UI_TEST
    const char *test_controller = getenv("EC_UI_CONTROLLER");
    if (test_controller) snprintf(controller, sizeof controller, "%s", test_controller);
#endif
    if (ec_background_start(&app.config, &app.services, ui_service_name(app.active_tab), controller)) {
        notify(ec_last_error());
        return;
    }
    notify("Daemon started.");
    if (app.active_tab == UI_TAB_SSH) {
        update_fingerprint();
    }
}

static void stop_selected_service(void) {
    if (!ui_is_running(&app, app.active_tab)) {
        return;
    }
    if (ec_background_stop(&app.config, &app.services, ui_service_name(app.active_tab))) {
        notify(ec_last_error());
    } else {
        notify("Daemon stopped.");
    }
}

static void save_diagnostics(void) {
    char path[EC_PATH];
    if (ec_path(path, sizeof path, app.config.root, "diagnostics.txt")) {
        return;
    }
    FILE *out = fopen(path, "w");
    if (!out) {
        notify("Could not save diagnostics.");
        return;
    }
    ec_doctor(&app.config, out);
    fprintf(out, "Device: %s\nFirmware: %s\nFirmware API: %u\nVersion: %s (%s)\n",
            GetDeviceModel(), GetSoftwareVersion(), ec_firmware_api(GetSoftwareVersion()), ec_version(), ec_family());
    fprintf(out, "UI viewport: %d x %d\nUI screen: %d x %d\nUI panel type: %d\n",
            app.screen_width, app.screen_height, ScreenWidth(), ScreenHeight(), GetPanelType());
    fprintf(out, "Library refresh: %s\nLibrary scan log: %s/library-scan.log\n",
            app.library_refresh ? "running" : "idle", app.config.root);
    fprintf(out, "Exit library scan log: %s/library-exit-scan.log\n", app.config.root);
    fclose(out);
    notify("Saved diagnostics.txt in the app folder.");
}

static void refresh_library(void) {
    if (app.library_refresh) {
        notify("Library refresh is already running.");
        return;
    }
    if (ui_library_start(&app)) {
        notify(ec_last_error());
        return;
    }
    app.library_handoff = true;
    for (ui_tab tab = UI_TAB_SSH; tab < UI_TAB_COUNT; ++tab) {
        set_notice(tab, "Refreshing library. The firmware may show a scanning screen.");
    }
}

static bool poll_library(void) {
    int result = ui_library_poll(&app);
    if (result) {
        const char *message = result > 0 ? "Library refresh finished. Open Library to see changes."
                                         : ec_last_error();
        for (ui_tab tab = UI_TAB_SSH; tab < UI_TAB_COUNT; ++tab) {
            set_notice(tab, message);
        }
    }
    return result != 0;
}

static void activate_control(ui_control_id control) {
    switch (control) {
    case UI_SSH_TAB:
        app.active_tab = UI_TAB_SSH;
        break;
    case UI_FTP_TAB:
        app.active_tab = UI_TAB_FTP;
        break;
    case UI_START:
        start_selected_service();
        break;
    case UI_STOP:
        stop_selected_service();
        break;
    case UI_SSH_KEYS:
    case UI_SSH_PASSWORD:
    case UI_SSH_BOTH:
    case UI_FTP_PASSWORD:
    case UI_FTP_ANONYMOUS:
        select_access_type(control);
        break;
    case UI_SSH_PORT:
        begin_edit(UI_EDIT_SSH_PORT);
        break;
    case UI_FTP_PORT:
        begin_edit(UI_EDIT_FTP_PORT);
        break;
    case UI_FTP_PASSIVE_MIN:
        begin_edit(UI_EDIT_PASSIVE_MIN);
        break;
    case UI_FTP_PASSIVE_MAX:
        begin_edit(UI_EDIT_PASSIVE_MAX);
        break;
    case UI_FTP_ROOT:
        begin_edit(UI_EDIT_FTP_ROOT);
        break;
    case UI_SSH_SET_PASSWORD:
    case UI_FTP_SET_PASSWORD:
        begin_edit(UI_EDIT_PASSWORD);
        break;
    case UI_CHECK_UPDATES:
        ui_update_check(&app);
        break;
    case UI_VERSION:
        ui_update_version(&app);
        break;
    case UI_SAVE_DIAGNOSTICS:
        save_diagnostics();
        break;
    case UI_REFRESH_LIBRARY:
        refresh_library();
        break;
    case UI_EXIT:
        CloseApp();
        break;
    case UI_CONTROL_NONE:
    case UI_CONTROL_COUNT:
        break;
    }
}

static void handle_pointer_up(int screen_x, int screen_y) {
    if (app.modal || app.editor.field != UI_EDIT_NONE) {
        return;
    }
    (void)ec_background_poll(&app.config, &app.services);
    ui_control_id control = UI_CONTROL_NONE;
    if (screen_y < app.toolbar.height) {
        const ui_control_id actions[] = {UI_EXIT, UI_REFRESH_LIBRARY, UI_SAVE_DIAGNOSTICS};
        for (size_t i = 0; i < sizeof actions / sizeof actions[0]; ++i) {
            ui_rect bounds = ui_toolbar_bounds(actions[i], app.screen_width, app.toolbar.height,
                app.toolbar.slot_width, app.toolbar.left_indent, app.toolbar.right_indent);
            if (ui_rect_contains(bounds, screen_x, screen_y)) control = actions[i];
        }
    } else {
        int logical_x = screen_x * UI_CANVAS_WIDTH / app.screen_width;
        int logical_y = UI_BODY_TOP + (screen_y - app.toolbar.height) *
                        (UI_CANVAS_HEIGHT - UI_BODY_TOP) / (app.screen_height - app.toolbar.height);
        control = ui_hit_test(app.active_tab, &app.config, logical_x, logical_y);
    }
    if (!app.network_available && control != UI_SSH_TAB && control != UI_FTP_TAB &&
        control != UI_STOP && control != UI_EXIT && control != UI_SAVE_DIAGNOSTICS && control != UI_REFRESH_LIBRARY && control != UI_CHECK_UPDATES && control != UI_VERSION) {
        return;
    }
    activate_control(control);
    if (control != UI_EXIT) {
        redraw();
    }
}

static void refresh_status(void) {
    ui_update_poll();
    bool library_changed = poll_library();
    pid_t previous_ssh = app.services.ssh;
    pid_t previous_ftp = app.services.ftp;
    (void)ec_background_poll(&app.config, &app.services);
    if (previous_ssh && !app.services.ssh) {
        set_notice(UI_TAB_SSH, "SSH exited. Check its log for details.");
    }
    if (previous_ftp && !app.services.ftp) {
        set_notice(UI_TAB_FTP, "FTP exited. Check its log for details.");
    }
    char address[sizeof app.device_address];
    read_device_address(address, sizeof address);
    if (app.foreground && (library_changed || previous_ssh != app.services.ssh ||
        previous_ftp != app.services.ftp || strcmp(address, app.device_address))) {
        redraw();
    }
    if (app.foreground || app.library_refresh || app.library_handoff) {
        SetHardTimer("easy-connect-status", refresh_status, ui_update_active() ? 100 : UI_STATUS_REFRESH_MS);
    }
}

static void enter_background(void) {
    ui_update_cancel(&app);
    app.foreground = false;
    ClearTimer(refresh_status);
    /* The headless owner keeps listeners and their sessions alive. */
    if (app.library_refresh || app.library_handoff)
        SetHardTimer("easy-connect-status", refresh_status, ui_update_active() ? 100 : UI_STATUS_REFRESH_MS);
}

static bool open_fonts(void) {
    int width_size = UI_BODY_FONT_SIZE * ScreenWidth() / UI_CANVAS_WIDTH;
    int height_size = UI_BODY_FONT_SIZE * (ScreenHeight() - app.toolbar.height) /
                      (UI_CANVAS_HEIGHT - UI_BODY_TOP);
    int size = width_size < height_size ? width_size : height_size;
    app.fonts.body = OpenFont(DEFAULTFONT, size, 0);
    app.fonts.small = OpenFont(DEFAULTFONT, size * UI_SMALL_FONT_PERCENT / 100, 0);
    app.fonts.title = OpenFont(DEFAULTFONT, size * UI_TITLE_FONT_PERCENT / 100, 1);
    if (!app.fonts.body || !app.fonts.small || !app.fonts.title) {
        CloseApp();
        return false;
    }
    return true;
}

static void close_font(ifont **font) {
    if (*font) {
        CloseFont(*font);
        *font = NULL;
    }
}

static void close_application(void) {
    static bool closed;
    if (closed) return;
    closed = true;
    bool refresh_on_exit = app.services_open && !rejected_device;
    ClearTimer(refresh_status);
    ui_update_close();
    ui_library_stop(&app);
    if (app.services_open) {
        app.services_open = false;
    }
    close_font(&app.fonts.body);
    close_font(&app.fonts.small);
    close_font(&app.fonts.title);
    clear_password_buffers();
    if (refresh_on_exit) {
        if (ui_library_on_exit(&app))
            fprintf(stderr, "Exit library refresh: %s\n", ec_last_error());
        else
            fprintf(stderr, "Exit library refresh handed off; see library-exit-scan.log.\n");
    }
}

static int handle_event(int type, int parameter1, int parameter2) {
    /* Lifecycle traces survive a frozen screen in ui.log. Never log keyboard
     * contents; limit touch traces so normal use cannot flood the log. */
    static unsigned touches;
    bool trace = type == EVT_INIT || type == EVT_SHOW || type == EVT_HIDE ||
                 type == EVT_FOREGROUND || type == EVT_BACKGROUND || type == EVT_FOCUS ||
                 type == EVT_ACTIVATE || type == EVT_EXIT || type == EVT_ORIENTATION;
    if (type == EVT_POINTERUP && touches++ < 32) trace = true;
    if (trace) {
        fprintf(stderr, "UI event %d begin pid=%ld foreground=%d editor=%d\n",
                type, (long)getpid(), app.foreground, app.editor.field);
    }
    switch (type) {
    case EVT_INIT:
        fprintf(stderr, "Device: %s firmware: %s API: %u\n", GetDeviceModel(), GetSoftwareVersion(),
                ec_firmware_api(GetSoftwareVersion()));
        if (!ec_device_supported(GetDeviceModel(), GetSoftwareVersion())) {
            rejected_device = true;
            Message(ICON_WARNING, "Easy Connect", "Unknown model or unsupported firmware API. See the installation guide.", 3000);
            CloseApp();
            break;
        }
        if (ui_update_on_start(&app)) { CloseApp(); break; }
        /* Full-screen layout: the system panel otherwise offsets scanout and
         * wraps bottom rows to the top after firmware refreshes. */
        SetPanelType(PANEL_DISABLED);
        app.screen_width = ScreenWidth();
        app.screen_height = ScreenHeight();
        if (!ui_prepare_toolbar(&app)) {
            Message(ICON_WARNING, "Easy Connect", "The current theme is missing native toolbar resources.", 3000);
            CloseApp();
        } else if (open_fonts()) {
            update_fingerprint();
        }
        break;
    case EVT_SHOW:
    case EVT_FOREGROUND:
    case EVT_ACTIVATE:
    case EVT_FOCUS:
        /* Returning through Home/task switching need not send EVT_SHOW. */
        SetPanelType(PANEL_DISABLED);
        app.screen_width = ScreenWidth();
        app.screen_height = ScreenHeight();
        if (!ui_prepare_toolbar(&app)) { CloseApp(); break; }
        (void)ec_background_poll(&app.config, &app.services);
        app.foreground = true;
        app.library_handoff = false;
        poll_library();
        redraw();
        ClearTimer(refresh_status);
        SetHardTimer("easy-connect-status", refresh_status, ui_update_active() ? 100 : UI_STATUS_REFRESH_MS);
        break;
    case EVT_REPAINT:
        redraw();
        break;
    case EVT_ORIENTATION:
        app.screen_width = ScreenWidth();
        app.screen_height = ScreenHeight();
        if (!ui_prepare_toolbar(&app)) { CloseApp(); break; }
        redraw();
        break;
    case EVT_POINTERUP:
        handle_pointer_up(parameter1, parameter2);
        break;
    case EVT_KEYPRESS:
        if (parameter1 == KEY_BACK) {
            CloseApp();
        }
        break;
    case EVT_HIDE:
        /* Opening InkView's keyboard also hides the application surface. */
        if (!app.modal && app.editor.field == UI_EDIT_NONE) {
            enter_background();
        }
        break;
    case EVT_BACKGROUND:
        enter_background();
        break;
    case EVT_EXIT:
        close_application();
        break;
    }
    if (trace) {
        fprintf(stderr, "UI event %d end foreground=%d\n", type, app.foreground);
    }
    return 0;
}

int main(void) {
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "Easy Connect %s (%s) build %s %s pid=%ld\n", ec_version(), ec_family(), __DATE__, __TIME__, (long)getpid());
    umask(0077);
    const char *root = EC_DEFAULT_ROOT;
#ifdef EC_UI_TEST
    if (getenv("EC_UI_ROOT")) {
        root = getenv("EC_UI_ROOT");
    }
#endif
    if (ec_config_load(&app.config, root) || ec_background_poll(&app.config, &app.services)) {
        return 1;
    }
    app.services_open = true;
    app.ftp_rootless = geteuid() != 0;
    struct passwd *user = getpwuid(geteuid());
    snprintf(app.ssh_user, sizeof app.ssh_user, "%s", user ? user->pw_name : "unknown");
    InkViewMain(handle_event);
    close_application();
    return rejected_device ? 1 : app.restart_update ? 75 : 0;
}
