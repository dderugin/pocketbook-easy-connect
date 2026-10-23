#include "ec.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *password_file(const char *service) {
    if (!strcmp(service, "ssh")) {
        return "password.hash";
    }
    if (!strcmp(service, "ftp")) {
        return "ftp-password.hash";
    }
    return NULL;
}

int ec_has_password(const ec_config *config, const char *service) {
    char path[EC_PATH];
    const char *name = password_file(service);
    struct stat metadata;
    return name && !ec_path(path, sizeof path, config->state, name) && !lstat(path, &metadata) &&
           S_ISREG(metadata.st_mode) && metadata.st_size > 0;
}

int ec_set_password(const ec_config *config, const char *service, const char *password) {
    const char *name = password_file(service);
    size_t length = strlen(password);
    if (!name) {
        return ec_error("unknown password service");
    }
    if (length < EC_PASSWORD_MIN_BYTES || length > EC_PASSWORD_MAX_BYTES ||
        strpbrk(password, "\r\n")) {
        return ec_error("password must contain 12-1024 bytes without newlines");
    }
    struct stat metadata;
    if (lstat(config->state, &metadata)) {
        if (errno != ENOENT || mkdir(config->state, 0700)) {
            return ec_error("cannot create credential directory");
        }
    } else if (!S_ISDIR(metadata.st_mode)) {
        return ec_error("credential directory must not be a symlink");
    }
    unsigned char salt[EC_PASSWORD_SALT_BYTES], derived[EC_PASSWORD_HASH_BYTES];
    if (RAND_bytes(salt, sizeof salt) != 1 ||
        PKCS5_PBKDF2_HMAC(password, (int)length, salt, sizeof salt, EC_PASSWORD_ITERATIONS,
                          EVP_sha256(), sizeof derived, derived) != 1) {
        return ec_error("could not generate password verifier");
    }
    char temporary[EC_PATH], target[EC_PATH];
    if (ec_path(temporary, sizeof temporary, config->state, ".password.XXXXXX") ||
        ec_path(target, sizeof target, config->state, name)) {
        OPENSSL_cleanse(derived, sizeof derived);
        return -1;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) {
        OPENSSL_cleanse(derived, sizeof derived);
        return ec_error("cannot create password file");
    }
    FILE *out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(temporary);
        OPENSSL_cleanse(derived, sizeof derived);
        return ec_error("cannot open password file");
    }
    fprintf(out, "ec1$%d$", EC_PASSWORD_ITERATIONS);
    for (unsigned i = 0; i < sizeof salt; ++i) {
        fprintf(out, "%02x", salt[i]);
    }
    fputc('$', out);
    for (unsigned i = 0; i < sizeof derived; ++i) {
        fprintf(out, "%02x", derived[i]);
    }
    fputc('\n', out);
    OPENSSL_cleanse(derived, sizeof derived);
    int failed = ferror(out);
    if (fflush(out) || fsync(fd)) {
        failed = 1;
    }
    if (fclose(out)) {
        failed = 1;
    }
    if (failed || rename(temporary, target)) {
        unlink(temporary);
        return ec_error("could not save password; previous password retained");
    }
    return 0;
}

unsigned ec_key_count(const ec_config *config) {
    char path[EC_PATH], line[8192];
    if (ec_path(path, sizeof path, config->state, "authorized_keys")) {
        return 0;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    struct stat metadata;
    if (fd < 0) {
        return 0;
    }
    if (fstat(fd, &metadata) || !S_ISREG(metadata.st_mode) ||
        metadata.st_size > EC_PRIVATE_FILE_MAX_BYTES) {
        close(fd);
        return 0;
    }
    FILE *in = fdopen(fd, "r");
    if (!in) {
        close(fd);
        return 0;
    }
    unsigned count = 0;
    while (fgets(line, sizeof line, in)) {
        char *cursor = line;
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor && *cursor != '#' && *cursor != '\r' && *cursor != '\n') {
            ++count;
        }
        if (!strchr(line, '\n')) {
            while (fgets(line, sizeof line, in) && !strchr(line, '\n')) {
            }
        }
    }
    fclose(in);
    return count;
}
