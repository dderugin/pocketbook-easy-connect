/* Timer-driven HTTPS transfers, using the same libcurl multi approach as CoolReader. */
#include "update.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
struct ec_update_transfer {
    CURL *easy;
    CURLM *multi;
    FILE *file;
    char *body, *path;
    size_t limit, used;
    unsigned long long total;
    bool done, success;
    char error[CURL_ERROR_SIZE];
};
static size_t receive(char *data, size_t size, size_t count, void *context) {
    ec_update_transfer *t = context;
    if (size && count > SIZE_MAX / size)
        return 0;
    size_t bytes = size * count;
    if (bytes > t->limit - t->used) {
        ec_error("Update response exceeds its size limit.");
        return 0;
    }
    if (t->file) {
        size_t n = fwrite(data, 1, bytes, t->file);
        t->used += n;
        return n;
    }
    memcpy(t->body + t->used, data, bytes);
    t->used += bytes;
    t->body[t->used] = 0;
    return bytes;
}
void ec_update_free(ec_update_transfer *t) {
    if (!t)
        return;
    if (t->multi && t->easy)
        curl_multi_remove_handle(t->multi, t->easy);
    if (t->easy)
        curl_easy_cleanup(t->easy);
    if (t->multi)
        curl_multi_cleanup(t->multi);
    if (t->file)
        fclose(t->file);
    if (!t->success && t->path)
        unlink(t->path);
    free(t->path);
    free(t->body);
    free(t);
}
ec_update_transfer *ec_update_fetch(const char *url, const char *output, size_t limit) {
    if (!url || strncmp(url, "https://", 8) || !limit || limit > EC_UPDATE_LIMIT) {
        ec_error("Only HTTPS update downloads are allowed.");
        return NULL;
    }
    static bool initialized;
    if (!initialized) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            ec_error("Cannot initialize HTTPS.");
            return NULL;
        }
        initialized = true;
    }
    ec_update_transfer *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->limit = limit;
    if (output) {
        int fd = open(output, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) {
            ec_error("Cannot create update download: %s", strerror(errno));
            goto fail;
        }
        t->path = strdup(output);
        t->file = fdopen(fd, "wb");
        if (!t->file) {
            close(fd);
            unlink(output);
            goto fail;
        }
        if (!t->path)
            goto fail;
    } else {
        t->body = calloc(limit + 1, 1);
        if (!t->body)
            goto fail;
    }
    t->easy = curl_easy_init();
    t->multi = curl_multi_init();
    if (!t->easy || !t->multi)
        goto fail;
#define OPTION(key, value)                                                                         \
    do {                                                                                           \
        if (curl_easy_setopt(t->easy, key, value) != CURLE_OK) {                                   \
            ec_error("Firmware HTTPS library does not support safe updates.");                     \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)
    OPTION(CURLOPT_URL, url);
    OPTION(CURLOPT_ERRORBUFFER, t->error);
    OPTION(CURLOPT_WRITEFUNCTION, receive);
    OPTION(CURLOPT_WRITEDATA, t);
    OPTION(CURLOPT_FAILONERROR, 1L);
    OPTION(CURLOPT_FOLLOWLOCATION, 1L);
    OPTION(CURLOPT_PROTOCOLS_STR, "https");
    OPTION(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    OPTION(CURLOPT_SSL_VERIFYHOST, 2L);
    OPTION(CURLOPT_SSL_VERIFYPEER, 1L);
    const char *ca = getenv("SSL_CERT_FILE");
    if (ca && *ca)
        OPTION(CURLOPT_CAINFO, ca);
    else if (!access("/ebrmain/share/ssl/certs/ca-certificates.crt", R_OK))
        OPTION(CURLOPT_CAINFO, "/ebrmain/share/ssl/certs/ca-certificates.crt");
    OPTION(CURLOPT_MAXREDIRS, 5L);
    OPTION(CURLOPT_CONNECTTIMEOUT, 15L);
    OPTION(CURLOPT_TIMEOUT, output ? 600L : 60L);
    OPTION(CURLOPT_LOW_SPEED_LIMIT, 128L);
    OPTION(CURLOPT_LOW_SPEED_TIME, 30L);
    OPTION(CURLOPT_NOSIGNAL, 1L);
    OPTION(CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)limit);
    OPTION(CURLOPT_USERAGENT, "EasyConnect/PocketBook updater");
#undef OPTION
    if (curl_multi_add_handle(t->multi, t->easy) != CURLM_OK)
        goto fail;
    return t;
fail:
    ec_update_free(t);
    return NULL;
}
int ec_update_poll(ec_update_transfer *t) {
    if (!t)
        return -1;
    if (t->done)
        return t->success ? 1 : -1;
    int running = 0;
    CURLMcode code = curl_multi_perform(t->multi, &running);
    if (code != CURLM_OK) {
        t->done = true;
        return ec_error("Update connection failed: %s", curl_multi_strerror(code));
    }
    curl_off_t size = 0;
    if (curl_easy_getinfo(t->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &size) == CURLE_OK &&
        size > 0)
        t->total = (unsigned long long)size;
    int remaining;
    CURLMsg *message;
    while ((message = curl_multi_info_read(t->multi, &remaining))) {
        if (message->msg != CURLMSG_DONE)
            continue;
        t->done = true;
        t->success = message->data.result == CURLE_OK;
        if (t->file) {
            int failed = fflush(t->file);
            if (fsync(fileno(t->file)))
                failed = 1;
            if (fclose(t->file))
                failed = 1;
            t->file = NULL;
            if (failed) {
                t->success = false;
                return ec_error("Could not save update. Check free storage.");
            }
        }
        /* Embedded NULs must not let a valid prefix hide a different manifest. */
        if (t->body && memchr(t->body, 0, t->used))
            t->success = false;
        if (!t->success)
            return ec_error("Update download failed: %s",
                            *t->error ? t->error : curl_easy_strerror(message->data.result));
        return 1;
    }
    return 0;
}
const char *ec_update_body(const ec_update_transfer *t) {
    return t && t->body ? t->body : "";
}
unsigned long long ec_update_received(const ec_update_transfer *t) {
    return t ? t->used : 0;
}
unsigned long long ec_update_total(const ec_update_transfer *t) {
    return t ? t->total : 0;
}
