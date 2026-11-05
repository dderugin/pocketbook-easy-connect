#include "update.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
int ec_update_lock(void) {
    char directory[128], path[EC_PATH];
    snprintf(directory, sizeof directory, "/tmp/easy-connect-%lu", (unsigned long)geteuid());
    if (ec_private_dir(directory) || ec_path(path, sizeof path, directory, "update.lock"))
        return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat st;
    if (fd < 0)
        return ec_error("Cannot open update lock.");
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        flock(fd, LOCK_EX | LOCK_NB)) {
        close(fd);
        return ec_error("Another update is already in progress.");
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}
int ec_update_paths(const ec_config *c, char *out, const char *suffix) {
    /* A trailing slash or dot component would put a staging directory inside
     * the installation and make recursive snapshot copying unsafe. */
    size_t length = strlen(c->root);
    struct stat root;
    const char *name = strrchr(c->root, '/');
    if (length < 2 || c->root[0] != '/' || c->root[length - 1] == '/' || !name ||
        !strcmp(name + 1, ".") || !strcmp(name + 1, "..") || lstat(c->root, &root) ||
        !S_ISDIR(root.st_mode))
        return ec_error(
            "Update installation root must be a real directory with no trailing slash.");
    /* Siblings survive the two directory renames, including a power interruption. */
    int n = snprintf(out, EC_PATH, "%s-ota-%s", c->root, suffix);
    return n < 0 || n >= EC_PATH ? ec_error("Update path is too long.") : 0;
}
int ec_update_remove(const char *path) {
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    DIR *d = opendir(path);
    if (!d)
        return -1;
    int result = 0;
    struct dirent *e;
    char child[EC_PATH];
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            if (ec_path(child, sizeof child, path, e->d_name) || ec_update_remove(child))
                result = -1;
        }
    closedir(d);
    return result ? result : rmdir(path);
}
int ec_update_write(const char *path, const void *data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return ec_error("Cannot create update file: %s", strerror(errno));
    const unsigned char *p = data;
    int result = 0;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            result = -1;
            break;
        }
        p += n;
        size -= (size_t)n;
    }
    if (fsync(fd))
        result = -1;
    if (close(fd))
        result = -1;
    return result ? ec_error("Cannot save update. Check free storage.") : 0;
}
int ec_update_read(const char *path, char *out, size_t size) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size >= size) {
        close(fd);
        return -1;
    }
    size_t used = 0;
    int result = 0;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, out + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            result = -1;
            break;
        }
        used += (size_t)n;
    }
    close(fd);
    out[used] = 0;
    return result || memchr(out, 0, used) ? -1 : 0;
}
int ec_update_hash(const char *path, char hex[65]) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > EC_UPDATE_LIMIT) {
        close(fd);
        return -1;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char buffer[32768], hash[32];
    unsigned count = 0;
    int ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    ssize_t n;
    while (ok && (n = read(fd, buffer, sizeof buffer)) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = 0;
            break;
        }
        ok = EVP_DigestUpdate(ctx, buffer, (size_t)n);
    }
    close(fd);
    if (ok)
        ok = EVP_DigestFinal_ex(ctx, hash, &count);
    EVP_MD_CTX_free(ctx);
    if (!ok || count != 32)
        return ec_error("Cannot verify update checksum.");
    for (int i = 0; i < 32; ++i)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return 0;
}
int ec_update_copy(const char *source, const char *target) {
    struct stat st;
    if (lstat(source, &st))
        return -1;
    if (S_ISLNK(st.st_mode)) {
        char link[EC_PATH];
        ssize_t n = readlink(source, link, sizeof link - 1);
        if (n < 0 || (size_t)n >= sizeof link - 1)
            return -1;
        link[n] = 0;
        return symlink(link, target);
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(target, st.st_mode & 0777))
            return -1;
        DIR *d = opendir(source);
        if (!d)
            return -1;
        int result = 0;
        struct dirent *e;
        char a[EC_PATH], b[EC_PATH];
        while (!result && (e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                result = ec_path(a, sizeof a, source, e->d_name) ||
                         ec_path(b, sizeof b, target, e->d_name) || ec_update_copy(a, b);
            }
        closedir(d);
        return result;
    }
    if (!S_ISREG(st.st_mode))
        return -1;
    int in = open(source, O_RDONLY | O_NOFOLLOW | O_NONBLOCK), out = -1, result = 0;
    if (in < 0)
        return -1;
    out = open(target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, st.st_mode & 0777);
    if (out < 0) {
        close(in);
        return -1;
    }
    unsigned char buffer[32768];
    ssize_t n;
    while ((n = read(in, buffer, sizeof buffer)) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            result = -1;
            break;
        }
        size_t offset = 0;
        while (offset < (size_t)n) {
            ssize_t written = write(out, buffer + offset, (size_t)n - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                result = -1;
                break;
            }
            offset += (size_t)written;
        }
        if (result)
            break;
    }
    if (fsync(out))
        result = -1;
    if (close(out))
        result = -1;
    close(in);
    return result;
}
