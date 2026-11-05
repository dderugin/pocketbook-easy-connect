#include "update.h"
#include "device.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int child_path(char *out, const char *base, const char *name) {
    return ec_path(out, EC_PATH, base, name);
}
static int ready(const ec_config *config, char *stage, ec_update_release *release,
                 const char *model, const char *firmware) {
    char path[EC_PATH], text[EC_UPDATE_MANIFEST_LIMIT + 1];
    struct stat directory;
    if (ec_update_paths(config, stage, "new") || lstat(stage, &directory) ||
        !S_ISDIR(directory.st_mode) || child_path(path, stage, ".ready") ||
        ec_update_read(path, text, sizeof text))
        return -1;
    return ec_update_parse(text, model, firmware, release);
}
int ec_update_pending(const ec_config *config, const char *model, const char *firmware,
                      ec_update_release *release) {
    char stage[EC_PATH];
    if (ready(config, stage, release, model, firmware))
        return -1;
    return ec_update_newer(release->version, ec_version()) ? 0 : -1;
}
int ec_update_stage(const ec_config *config, const char *zip, const ec_update_release *release,
                    const char *model, const char *firmware) {
    ec_update_release parsed;
    if (ec_update_parse(release->text, model, firmware, &parsed) ||
        strcmp(parsed.checksum, release->checksum) || strcmp(parsed.version, release->version) ||
        !ec_update_newer(parsed.version, ec_version()))
        return ec_error("Update is not newer than this installation.");
    if (ec_update_archive(zip, release, model, firmware, NULL))
        return -1;
    char temporary[EC_PATH], stage[EC_PATH], path[EC_PATH];
    if (ec_update_paths(config, temporary, "download.XXXXXX") ||
        ec_update_paths(config, stage, "new") || !mkdtemp(temporary))
        return ec_error("Cannot create update staging folder.");
    int result = -1;
    if (child_path(path, temporary, "package.zip") || ec_update_copy(zip, path) ||
        ec_update_archive(path, release, model, firmware, NULL))
        goto done;
    if (child_path(path, temporary, ".ready") ||
        ec_update_write(path, release->text, strlen(release->text)))
        goto done;
    if (ec_update_remove(stage) || rename(temporary, stage))
        goto done;
    sync();
    result = 0;
done:
    ec_update_remove(temporary);
    return result ? ec_error("Could not stage the update. Check free storage.") : 0;
}
int ec_update_request(const ec_config *config, const char *model, const char *firmware) {
    if (!ec_device_supported(model, firmware) || strchr(model, '\n') || strchr(firmware, '\n'))
        return -1;
    char stage[EC_PATH], path[EC_PATH], text[512];
    ec_update_release release;
    if (ready(config, stage, &release, model, firmware) || child_path(path, stage, ".install"))
        return -1;
    snprintf(text, sizeof text, "%s\n%s\n", model, firmware);
    if (unlink(path) && errno != ENOENT)
        return -1;
    return ec_update_write(path, text, strlen(text));
}
static int acquire(ec_config *config, ec_services *services) {
    /* Same exclusive lock as the foreground and background service owners. */
    for (int i = 0; ec_services_open(config, services); ++i) {
        ec_services remote;
        if (i >= 60 || ec_background_poll(config, &remote) || remote.ssh || remote.ftp)
            return ec_error("Stop both SSH and FTP before installing or rolling back an update.");
        struct timespec delay = {0, 50000000};
        nanosleep(&delay, NULL);
    }
    return 0;
}
int ec_update_install(ec_config *config) {
    int lock = ec_update_lock();
    if (lock < 0)
        return -1;
    ec_services services;
    int result = -1;
    bool owned = false;
    char stage[EC_PATH], merged[EC_PATH] = {0}, backup[EC_PATH], path[EC_PATH], zip[EC_PATH],
                         identity[512];
    if (ec_update_paths(config, stage, "new") || ec_update_paths(config, merged, "merged") ||
        ec_update_paths(config, backup, "old") || child_path(path, stage, ".install") ||
        ec_update_read(path, identity, sizeof identity))
        goto done;
    char *firmware = strchr(identity, '\n');
    if (!firmware)
        goto done;
    *firmware++ = 0;
    char *end = strchr(firmware, '\n');
    if (!end || end[1])
        goto done;
    *end = 0;
    ec_update_release release;
    if (ready(config, stage, &release, identity, firmware) ||
        !ec_update_newer(release.version, ec_version()) || child_path(zip, stage, "package.zip") ||
        ec_update_archive(zip, &release, identity, firmware, NULL))
        goto done;
    if (acquire(config, &services))
        goto done;
    owned = true;
    if (ec_update_remove(merged) || ec_update_copy(config->root, merged))
        goto storage;
    /* Never follow an existing bin/default symlink while overlaying runtime files. */
    if (child_path(path, merged, "bin") || ec_update_remove(path) || mkdir(path, 0755) ||
        child_path(path, merged, "easy-connect.conf.default") || ec_update_remove(path))
        goto storage;
    /* The stable launcher is installed once over USB; OTA replaces app runtime only. */
    if (ec_update_archive(zip, &release, identity, firmware, merged))
        goto done;
    if (child_path(path, merged, "EasyConnect.app") || unlink(path))
        goto storage;
    if (child_path(path, merged, ".ota-first-run") || ec_update_remove(path) ||
        ec_update_write(path, release.version, strlen(release.version)))
        goto storage;
    sync();
    if (ec_update_remove(backup) || rename(config->root, backup))
        goto storage;
    if (rename(merged, config->root)) {
        if (rename(backup, config->root))
            ec_error("Update switch interrupted. Relaunch Easy Connect to recover.");
        else
            ec_error("Could not install update. Previous installation restored.");
        goto done;
    }
    sync();
    ec_update_remove(stage);
    result = 0;
    goto done;
storage:
    ec_error("Cannot prepare update. Check free space and storage access; installation unchanged.");
done:
    if (owned)
        ec_services_close(config, &services);
    if (result && *merged)
        ec_update_remove(merged);
    close(lock);
    return result;
}
int ec_update_rollback(ec_config *config) {
    int lock = ec_update_lock();
    if (lock < 0)
        return -1;
    ec_services services;
    int result = -1;
    bool owned = false;
    char backup[EC_PATH], failed[EC_PATH], merged[EC_PATH] = {0}, marker[EC_PATH], source[EC_PATH],
                                           target[EC_PATH];
    struct stat st;
    if (ec_update_paths(config, backup, "old") || ec_update_paths(config, failed, "failed") ||
        ec_update_paths(config, merged, "rollback") ||
        child_path(marker, config->root, ".ota-first-run") || lstat(marker, &st) ||
        !S_ISREG(st.st_mode))
        goto done;
    if (acquire(config, &services))
        goto done;
    owned = true;
    /* Restore the previous runtime around the latest user state, including
     * credentials or settings changed during the failed version's first run. */
    if (ec_update_remove(merged) || ec_update_copy(config->root, merged) ||
        child_path(source, backup, "bin") || child_path(target, merged, "bin") ||
        ec_update_remove(target) || ec_update_copy(source, target))
        goto done;
    if (child_path(source, backup, "easy-connect.conf.default") ||
        child_path(target, merged, "easy-connect.conf.default") || ec_update_remove(target))
        goto done;
    if (!lstat(source, &st) && ec_update_copy(source, target))
        goto done;
    if (child_path(marker, merged, ".ota-first-run") || ec_update_remove(marker) ||
        child_path(marker, merged, ".ota-rollback-ready") || ec_update_remove(marker) ||
        ec_update_write(marker, "ready\n", 6))
        goto done;
    sync();
    if (ec_update_remove(failed) || rename(config->root, failed))
        goto done;
    if (rename(merged, config->root)) {
        rename(failed, config->root);
        goto done;
    }
    if (child_path(marker, config->root, ".ota-rollback-ready"))
        goto done;
    if (!ec_update_paths(config, source, "new"))
        ec_update_remove(source);
    sync();
    unlink(marker);
    sync();
    result = 0;
done:
    if (owned)
        ec_services_close(config, &services);
    if (result && *merged)
        ec_update_remove(merged);
    close(lock);
    return result;
}
