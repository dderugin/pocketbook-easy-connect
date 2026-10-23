#include "ec.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char last_error[512];

/* Each key has a named bit so duplicate detection does not depend on literals. */
enum config_setting {
    SETTING_SSH_PORT = 1u << 0,
    SETTING_FTP_PORT = 1u << 1,
    SETTING_PASSIVE_MIN = 1u << 2,
    SETTING_PASSIVE_MAX = 1u << 3,
    SETTING_SSH_AUTH = 1u << 4,
    SETTING_BIND_ADDRESS = 1u << 5,
    SETTING_FTP_ROOT = 1u << 6,
    SETTING_STATE_DIR = 1u << 7,
    SETTING_FTP_USER = 1u << 8,
    SETTING_FTP_AUTH = 1u << 9
};

const char *ec_last_error(void) {
    return last_error;
}

int ec_error(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(last_error, sizeof last_error, format, ap);
    fprintf(stderr, "easy-connect: %s\n", last_error);
    va_end(ap);
    return -1;
}

int ec_path(char *out, size_t size, const char *base, const char *name) {
    int n = snprintf(out, size, "%s/%s", base, name);
    return n < 0 || (size_t)n >= size ? ec_error("path too long") : 0;
}

/* Runtime directories must have real Unix permissions; do not relax on FAT. */
int ec_private_dir(const char *path) {
    struct stat metadata;
    if (mkdir(path, 0700) && errno != EEXIST) {
        return ec_error("mkdir %s: %s", path, strerror(errno));
    }
    if (lstat(path, &metadata) || !S_ISDIR(metadata.st_mode) || metadata.st_uid != geteuid() ||
        (metadata.st_mode & 0077)) {
        return ec_error("%s must be an owned, private directory", path);
    }
    return 0;
}

int ec_copy_private(const char *source, const char *destination, int nonempty) {
    char buffer[4096];
    struct stat metadata;
    ssize_t n;
    size_t total = 0;
    int src = open(source, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (src < 0) {
        return ec_error("open %s: %s", source, strerror(errno));
    }
    if (fstat(src, &metadata) || !S_ISREG(metadata.st_mode) ||
        metadata.st_size > EC_PRIVATE_FILE_MAX_BYTES) {
        close(src);
        return ec_error("%s must be a regular file of at most 1 MiB", source);
    }
    int dst = open(destination, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (dst < 0) {
        close(src);
        return ec_error("open %s: %s", destination, strerror(errno));
    }
    int result = 0;
    while ((n = read(src, buffer, sizeof buffer)) > 0) {
        total += (size_t)n;
        if (total > EC_PRIVATE_FILE_MAX_BYTES) {
            result = -1;
            break;
        }
        ssize_t offset = 0;
        while (offset < n) {
            ssize_t written = write(dst, buffer + offset, (size_t)(n - offset));
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                result = -1;
                break;
            }
            offset += written;
        }
        if (result) {
            break;
        }
    }
    if (n < 0 || (nonempty && !total) || fsync(dst)) {
        result = -1;
    }
    close(src);
    close(dst);
    if (result) {
        unlink(destination);
        return ec_error("copy failed: %s", source);
    }
    return 0;
}

static char *trim_whitespace(char *text) {
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) {
        text[--n] = 0;
    }
    return text;
}

static int parse_port(const char *text, unsigned *out) {
    char *end;
    for (const char *cursor = text; *cursor; ++cursor) {
        if (!isdigit((unsigned char)*cursor)) {
            return ec_error("invalid port: %s", text);
        }
    }
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || !*text || *end || n < EC_PORT_MIN || n > EC_PORT_MAX) {
        return ec_error("invalid unprivileged port: %s", text);
    }
    *out = (unsigned)n;
    return 0;
}

int ec_config_load(ec_config *config, const char *root) {
    memset(config, 0, sizeof *config);
    if (!realpath(root, config->root)) {
        return ec_error("root %s: %s", root, strerror(errno));
    }
    if (ec_path(config->state, sizeof config->state, config->root, "state")) {
        return -1;
    }
    strcpy(config->bind_address, "0.0.0.0");
    strcpy(config->ftp_root, "/mnt/ext1");
    struct passwd *current_user = getpwuid(geteuid());
    if (!current_user || strlen(current_user->pw_name) >= sizeof config->ftp_user) {
        return ec_error("cannot determine the launching account");
    }
    strcpy(config->ftp_user, current_user->pw_name);
    config->ssh_port = 2222;
    config->ftp_port = 2121;
    config->pasv_min = 50000;
    config->pasv_max = 50009;
    config->auth = EC_KEYS;
    char path[EC_PATH], line[EC_PATH + 100];
    if (ec_path(path, sizeof path, config->root, "easy-connect.conf")) {
        return -1;
    }
    FILE *file = fopen(path, "r");
    if (!file && errno == ENOENT) {
        /* Package defaults never overwrite saved settings on an upgrade. */
        if (ec_path(path, sizeof path, config->root, "easy-connect.conf.default")) {
            return -1;
        }
        file = fopen(path, "r");
    }
    if (!file) {
        return errno == ENOENT ? 0 : ec_error("read config: %s", strerror(errno));
    }
    unsigned line_number = 0, seen = 0;
    int result = 0;
    while (fgets(line, sizeof line, file)) {
        ++line_number;
        if (!strchr(line, '\n') && !feof(file)) {
            result = -1;
            break;
        }
        char *key = trim_whitespace(line);
        if (!*key || *key == '#') {
            continue;
        }
        char *value = strchr(key, '=');
        if (!value) {
            result = -1;
            break;
        }
        *value++ = 0;
        key = trim_whitespace(key);
        value = trim_whitespace(value);
        unsigned flag = 0;
        if (!strcmp(key, "ssh_port")) {
            flag = SETTING_SSH_PORT;
            result = parse_port(value, &config->ssh_port);
        } else if (!strcmp(key, "ftp_port")) {
            flag = SETTING_FTP_PORT;
            result = parse_port(value, &config->ftp_port);
        } else if (!strcmp(key, "pasv_min")) {
            flag = SETTING_PASSIVE_MIN;
            result = parse_port(value, &config->pasv_min);
        } else if (!strcmp(key, "pasv_max")) {
            flag = SETTING_PASSIVE_MAX;
            result = parse_port(value, &config->pasv_max);
        } else if (!strcmp(key, "auth")) {
            flag = SETTING_SSH_AUTH;
            if (!strcmp(value, "key")) {
                config->auth = EC_KEYS;
            } else if (!strcmp(value, "password")) {
                config->auth = EC_PASSWORD;
            } else if (!strcmp(value, "both")) {
                config->auth = EC_BOTH;
            } else {
                result = -1;
            }
        } else if (!strcmp(key, "ftp_auth")) {
            flag = SETTING_FTP_AUTH;
            if (!strcmp(value, "password")) {
                config->ftp_auth = EC_FTP_PASSWORD;
            } else if (!strcmp(value, "anonymous")) {
                config->ftp_auth = EC_FTP_ANONYMOUS;
            } else {
                result = -1;
            }
        } else if (!strcmp(key, "bind_address")) {
            struct in_addr address;
            flag = SETTING_BIND_ADDRESS;
            if (strlen(value) >= sizeof config->bind_address ||
                inet_pton(AF_INET, value, &address) != 1) {
                result = -1;
            } else {
                strcpy(config->bind_address, value);
            }
        } else if (!strcmp(key, "ftp_root") || !strcmp(key, "state_dir")) {
            flag = !strcmp(key, "ftp_root") ? SETTING_FTP_ROOT : SETTING_STATE_DIR;
            if (*value != '/' || strlen(value) >= EC_PATH || strchr(value, '\r')) {
                result = -1;
            } else {
                strcpy(flag == SETTING_FTP_ROOT ? config->ftp_root : config->state, value);
            }
        } else if (!strcmp(key, "ftp_user")) {
            flag = SETTING_FTP_USER;
            if (!*value || strlen(value) >= sizeof config->ftp_user) {
                result = -1;
            } else {
                for (const char *cursor = value; *cursor; ++cursor) {
                    if (!isalnum((unsigned char)*cursor) && *cursor != '_' && *cursor != '-' &&
                        *cursor != '.') {
                        result = -1;
                    }
                }
                if (!result && strcmp(value, "current")) {
                    strcpy(config->ftp_user, value);
                }
            }
        } else {
            result = -1;
        }
        if (result || (seen & flag)) {
            result = -1;
            break;
        }
        seen |= flag;
    }
    if (ferror(file)) {
        result = -1;
    }
    fclose(file);
    if (result) {
        return ec_error("invalid or duplicate config setting on line %u", line_number);
    }
    return ec_config_validate(config);
}

int ec_config_validate(const ec_config *config) {
    const unsigned ports[] = {config->ssh_port, config->ftp_port, config->pasv_min,
                              config->pasv_max};
    for (unsigned i = 0; i < sizeof ports / sizeof ports[0]; ++i) {
        if (ports[i] < EC_PORT_MIN || ports[i] > EC_PORT_MAX) {
            return ec_error("ports must be between 1024 and 65535");
        }
    }
    struct in_addr address;
    if (inet_pton(AF_INET, config->bind_address, &address) != 1) {
        return ec_error("invalid bind address");
    }
    if (config->auth < EC_KEYS || config->auth > EC_BOTH || config->ftp_auth < EC_FTP_PASSWORD ||
        config->ftp_auth > EC_FTP_ANONYMOUS) {
        return ec_error("invalid access type");
    }
    if (*config->state != '/' || *config->ftp_root != '/' || strpbrk(config->state, "\r\n") ||
        strpbrk(config->ftp_root, "\r\n")) {
        return ec_error("directories must be absolute paths without newlines");
    }
    if (!*config->ftp_user || strpbrk(config->ftp_user, "\r\n=")) {
        return ec_error("invalid FTP account");
    }
    if (config->ssh_port == config->ftp_port || config->pasv_min > config->pasv_max ||
        (config->ssh_port >= config->pasv_min && config->ssh_port <= config->pasv_max) ||
        (config->ftp_port >= config->pasv_min && config->ftp_port <= config->pasv_max)) {
        return ec_error("service and passive ports must not overlap");
    }
    return 0;
}

int ec_config_save(const ec_config *config) {
    char temporary[EC_PATH], target[EC_PATH];
    if (ec_config_validate(config) ||
        ec_path(temporary, sizeof temporary, config->root, ".easy-connect.conf.XXXXXX") ||
        ec_path(target, sizeof target, config->root, "easy-connect.conf")) {
        return -1;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) {
        return ec_error("cannot create settings file: %s", strerror(errno));
    }
    FILE *out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(temporary);
        return ec_error("cannot open settings file");
    }
    int failed = fprintf(out,
                         "# Saved by Easy Connect\nauth=%s\nftp_auth=%s\nbind_address=%s\n"
                         "ssh_port=%u\nftp_port=%u\npasv_min=%u\npasv_max=%u\n"
                         "ftp_user=%s\nftp_root=%s\nstate_dir=%s\n",
                         config->auth == EC_KEYS       ? "key"
                         : config->auth == EC_PASSWORD ? "password"
                                                       : "both",
                         config->ftp_auth == EC_FTP_PASSWORD ? "password" : "anonymous",
                         config->bind_address, config->ssh_port, config->ftp_port, config->pasv_min,
                         config->pasv_max, config->ftp_user, config->ftp_root, config->state) < 0;
    if (fflush(out) || fsync(fd)) {
        failed = 1;
    }
    if (fclose(out)) {
        failed = 1;
    }
    if (failed || rename(temporary, target)) {
        unlink(temporary);
        return ec_error("could not save settings; previous settings retained");
    }
    return 0;
}
