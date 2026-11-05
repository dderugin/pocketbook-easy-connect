#include "ui.h"
#include "device.h"
#include "update.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static struct {
    ui_app *app;
    ec_update_transfer *transfer;
    ec_update_release release;
    int lock, phase, last_percent;
    bool progress;
    char download[EC_PATH];
} update = {.lock = -1};
enum { IDLE, CHECKING, CONFIRMING, DOWNLOADING, READY };
static void notice(ui_app *app, const char *text) {
    for (int i = 0; i < UI_TAB_COUNT; ++i)
        snprintf(app->notices[i], sizeof app->notices[i], "%s", text);
}
static void restore(int button) {
    (void)button;
    if (!update.app)
        return;
    update.app->modal = false;
    if (update.app->foreground)
        ui_draw(update.app);
}
static void cleanup(void) {
    ec_update_free(update.transfer);
    update.transfer = NULL;
    if (update.progress) {
        CloseProgressbar();
        update.progress = false;
    }
    if (update.lock >= 0) {
        if (*update.download)
            unlink(update.download);
        close(update.lock);
        update.lock = -1;
    }
    update.phase = IDLE;
}
static void result(int icon, const char *text) {
    /* ec_last_error points at shared storage; copy it before cleanup calls. */
    char message[512];
    snprintf(message, sizeof message, "%s", text);
    cleanup();
    update.app->modal = true;
    Dialog(icon, "Easy Connect updates", message, "OK", NULL, restore);
}
void ui_update_cancel(ui_app *app) {
    if (update.phase == CHECKING || update.phase == DOWNLOADING) {
        cleanup();
        notice(app, "Update cancelled. Installation unchanged.");
        app->modal = false;
    }
}
static void cancel(int button) {
    (void)button;
    ui_update_cancel(update.app);
    restore(0);
}
static void restart(int button) {
    ui_app *app = update.app;
    update.phase = IDLE;
    if (button != 1) {
        restore(0);
        return;
    }
    /* Only this explicit confirmation ends active services and transfers. */
    if (ec_background_stop(&app->config, &app->services, "ssh") ||
        ec_background_stop(&app->config, &app->services, "ftp") ||
        ec_update_request(&app->config, GetDeviceModel(), GetSoftwareVersion())) {
        result(ICON_WARNING, ec_last_error());
        return;
    }
    app->restart_update = true;
    CloseApp();
}
static void offer_restart(void) {
    cleanup();
    update.phase = READY;
    update.app->modal = true;
    char text[512];
    snprintf(
        text, sizeof text,
        "Version %s is verified and ready. Restart now to install it?\n\nRestarting stops SSH, FTP "
        "and active connections. Settings, passwords and host keys are preserved. Choose Later to "
        "keep services running; installation waits until a launch with both services stopped.",
        update.release.version);
    Dialog(ICON_INFORMATION, "Easy Connect updates", text, "Restart now", "Later", restart);
}
static const char *origin(void) {
#ifdef EC_UI_TEST
    const char *url = getenv("EC_UI_UPDATE_ORIGIN");
    if (url)
        return url;
#endif
    return EC_UPDATE_ORIGIN;
}
static void download(int button) {
    if (button != 1) {
        cleanup();
        restore(0);
        return;
    }
    char url[1024];
    snprintf(url, sizeof url, "%sdownloads/easy-connect/%s/" EC_UPDATE_PACKAGE, origin(),
             update.release.version);
    update.phase = DOWNLOADING;
    update.last_percent = -1;
    update.transfer = ec_update_fetch(url, update.download, EC_UPDATE_LIMIT);
    if (!update.transfer) {
        result(ICON_WARNING, ec_last_error());
        return;
    }
    OpenProgressbar(ICON_INFORMATION, "Easy Connect updates", "Downloading update...", 0, cancel);
    update.progress = true;
}
void ui_update_check(ui_app *app) {
    if (app->modal)
        return;
    update.app = app;
    if (!ec_update_pending(&app->config, GetDeviceModel(), GetSoftwareVersion(), &update.release)) {
        offer_restart();
        return;
    }
    if (!app->network_available) {
        result(ICON_WARNING,
               "Connect to Wi-Fi in the reader settings, then check for updates again.");
        return;
    }
    update.lock = ec_update_lock();
    if (update.lock < 0) {
        result(ICON_WARNING, ec_last_error());
        return;
    }
    if (ec_update_paths(&app->config, update.download, "download.zip")) {
        result(ICON_WARNING, ec_last_error());
        return;
    }
    unlink(update.download);
    char url[1024];
    snprintf(url, sizeof url, "%s" EC_UPDATE_CHANNEL, origin());
    update.transfer = ec_update_fetch(url, NULL, EC_UPDATE_MANIFEST_LIMIT);
    if (!update.transfer) {
        result(ICON_WARNING, ec_last_error());
        return;
    }
    app->modal = true;
    update.phase = CHECKING;
    OpenProgressbar(ICON_INFORMATION, "Easy Connect updates", "Checking for updates...", 0, cancel);
    update.progress = true;
}
void ui_update_version(ui_app *app) {
    if (app->modal)
        return;
    update.app = app;
    app->modal = true;
    char text[128];
    snprintf(text, sizeof text, "Easy Connect %s\n%s", ec_version(), ec_family());
    Dialog(ICON_INFORMATION, "Version", text, "OK", NULL, restore);
}
bool ui_update_active(void) {
    return update.phase == CHECKING || update.phase == DOWNLOADING;
}
void ui_update_poll(void) {
    if (!ui_update_active())
        return;
    int done = ec_update_poll(update.transfer);
    if (update.phase == DOWNLOADING) {
        unsigned long long bytes = ec_update_received(update.transfer),
                           total = ec_update_total(update.transfer);
        int percent = total ? (int)(bytes * 100 / total) : 0;
        if (percent > 100)
            percent = 100;
        if (percent != update.last_percent || done) {
            char text[128];
            snprintf(text, sizeof text, "Downloading %s: %llu KB", update.release.version,
                     bytes / 1024);
            UpdateProgressbar(text, percent);
            update.last_percent = percent;
        }
    }
    if (!done)
        return;
    if (done < 0) {
        result(ICON_WARNING, ec_last_error());
        return;
    }
    if (update.phase == CHECKING) {
        if (ec_update_parse(ec_update_body(update.transfer), GetDeviceModel(), GetSoftwareVersion(),
                            &update.release)) {
            result(ICON_WARNING, ec_last_error());
            return;
        }
        ec_update_free(update.transfer);
        update.transfer = NULL;
        CloseProgressbar();
        update.progress = false;
        if (!ec_update_newer(update.release.version, ec_version())) {
            char text[128];
            snprintf(text, sizeof text, "You have the latest version (%s).", ec_version());
            result(ICON_INFORMATION, text);
            return;
        }
        char text[256];
        snprintf(text, sizeof text,
                 "Installed: %s\nAvailable: %s\nDownload this update? Services keep running while "
                 "it downloads. You can choose when to restart after verification.",
                 ec_version(), update.release.version);
        update.phase = CONFIRMING;
        Dialog(ICON_INFORMATION, "Easy Connect updates", text, "Download", "Cancel", download);
        return;
    }
    ec_update_free(update.transfer);
    update.transfer = NULL;
    UpdateProgressbar("Verifying and preparing update...", 100);
    if (ec_update_stage(&update.app->config, update.download, &update.release, GetDeviceModel(),
                        GetSoftwareVersion())) {
        result(ICON_WARNING, ec_last_error());
        return;
    }
    offer_restart();
}
bool ui_update_on_start(ui_app *app) {
    ec_update_release release;
    if (ec_update_pending(&app->config, GetDeviceModel(), GetSoftwareVersion(), &release))
        return false;
    if (!getenv("EC_UPDATE_SKIP") && !app->services.ssh && !app->services.ftp &&
        !ec_update_request(&app->config, GetDeviceModel(), GetSoftwareVersion())) {
        app->restart_update = true;
        return true;
    }
    notice(app, "Update ready. Check for updates to choose when to restart and install it.");
    return false;
}
void ui_update_close(void) {
    cleanup();
}
