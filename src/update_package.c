/* Read only our seven-file ZIP format. Never invoke a shell or external unzip. */
#include "update.h"
#include "device.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
const char *const ec_update_members[EC_UPDATE_FILES] = {
    "applications/EasyConnect.app",
    "system/share/easy-connect/easy-connect.conf.default",
    "system/share/easy-connect/bin/easy-connect",
    "system/share/easy-connect/bin/easy-connect.app",
    "system/share/easy-connect/bin/dropbear",
    "system/share/easy-connect/bin/dropbearkey",
    "system/share/easy-connect/bin/vsftpd"};
const char *const ec_update_targets[EC_UPDATE_FILES] = {
    "EasyConnect.app", "easy-connect.conf.default", "bin/easy-connect", "bin/easy-connect.app",
    "bin/dropbear",    "bin/dropbearkey",           "bin/vsftpd"};
static uint32_t u16(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}
static uint32_t u32(const unsigned char *p) {
    return u16(p) | u16(p + 2) << 16;
}
int ec_update_archive(const char *zip, const ec_update_release *release, const char *model,
                      const char *firmware, const char *output) {
    int result = -1, fd = open(zip, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    unsigned char *data = NULL, *unpacked = NULL;
    if (fd < 0)
        return ec_error("Cannot read downloaded update.");
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 22 || st.st_size > EC_UPDATE_LIMIT)
        goto done;
    size_t size = (size_t)st.st_size, used = 0;
    data = malloc(size);
    if (!data)
        goto done;
    while (used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto done;
        used += (size_t)n;
    }
    unsigned char hash[32];
    unsigned hash_size;
    char hex[65];
    if (!EVP_Digest(data, size, hash, &hash_size, EVP_sha256(), NULL) || hash_size != 32)
        goto done;
    for (int i = 0; i < 32; ++i)
        snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    if (strcmp(hex, release->checksum)) {
        ec_error("Update checksum mismatch. Download again.");
        goto cleanup;
    }
    size_t end = size - 22, lower = size > 65557 ? size - 65557 : 0;
    while (u32(data + end) != 0x06054b50 || end + 22 + u16(data + end + 20) != size) {
        if (end == lower)
            goto done;
        --end;
    }
    if (u16(data + end + 4) || u16(data + end + 6) || u16(data + end + 8) != 7 ||
        u16(data + end + 10) != 7 || u32(data + end + 12) > end ||
        u32(data + end + 16) != end - u32(data + end + 12))
        goto done;
    size_t comment_size = u16(data + end + 20);
    if (!comment_size || comment_size > EC_UPDATE_MANIFEST_LIMIT - 128 ||
        memchr(data + end + 22, 0, comment_size))
        goto done;
    char comment[EC_UPDATE_MANIFEST_LIMIT + 1], channel[EC_UPDATE_MANIFEST_LIMIT + 1];
    memcpy(comment, data + end + 22, comment_size);
    comment[comment_size] = 0;
    /* The ZIP comment identifies package/version/models without Library-visible files. */
    char *rules = strchr(comment, '\n');
    if (!rules)
        goto done;
    *rules++ = 0;
    if (strcmp(comment, "EC-PACKAGE-1"))
        goto done;
    char *revision = strchr(rules, '\n');
    if (!revision)
        goto done;
    *revision++ = 0;
    char *models = strchr(revision, '\n');
    if (!models)
        goto done;
    *models++ = 0;
    if (strcmp(rules, ec_family()) || strcmp(revision, release->version))
        goto done;
    int length = snprintf(channel, sizeof channel, "EC-OTA-SHARED-1\n%s\n%s\n%s\n%s", rules,
                          revision, release->checksum, models);
    ec_update_release package;
    if (length < 0 || (size_t)length >= sizeof channel ||
        ec_update_parse(channel, model, firmware, &package))
        goto done;
    size_t cursor = u32(data + end + 16), local_end = 0, total = 0;
    unsigned seen = 0;
    for (unsigned entry = 0; entry < 7; ++entry) {
        if (cursor > end || end - cursor < 46 || u32(data + cursor) != 0x02014b50)
            goto done;
        const unsigned char *c = data + cursor;
        unsigned flags = u16(c + 8), method = u16(c + 10), name_size = u16(c + 28),
                 extra = u16(c + 30), note = u16(c + 32);
        size_t compressed = u32(c + 20), plain = u32(c + 24), offset = u32(c + 42);
        unsigned mode = u32(c + 38) >> 16;
        if (flags & ~0x800u || method > 8 || (method != 0 && method != 8) || u16(c + 34) ||
            (mode & 0170000 && (mode & 0170000) != 0100000) || !plain || plain > 16 * 1024 * 1024 ||
            name_size > 128 || cursor + 46 + name_size + extra + note > end ||
            offset != local_end || offset >= u32(data + end + 16) ||
            u32(data + end + 16) - offset < 30 || u32(data + offset) != 0x04034b50)
            goto done;
        unsigned index;
        for (index = 0; index < 7; ++index)
            if (strlen(ec_update_members[index]) == name_size &&
                !memcmp(c + 46, ec_update_members[index], name_size))
                break;
        if (index == 7 || seen & (1u << index))
            goto done;
        seen |= 1u << index;
        total += plain;
        if (total > 64 * 1024 * 1024)
            goto done;
        const unsigned char *local = data + offset;
        if (u16(local + 6) != flags || u16(local + 8) != method || u32(local + 14) != u32(c + 16) ||
            u32(local + 18) != compressed || u32(local + 22) != plain ||
            u16(local + 26) != name_size)
            goto done;
        size_t body = offset + 30 + name_size + u16(local + 28), central = u32(data + end + 16);
        if (body > central || compressed > central - body || memcmp(local + 30, c + 46, name_size))
            goto done;
        local_end = body + compressed;
        unpacked = malloc(plain);
        if (!unpacked)
            goto done;
        if (method == 0) {
            if (compressed != plain)
                goto done;
            memcpy(unpacked, data + body, plain);
        } else {
            z_stream stream = {0};
            stream.next_in = data + body;
            stream.avail_in = (uInt)compressed;
            stream.next_out = unpacked;
            stream.avail_out = (uInt)plain;
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
                goto done;
            int code = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            if (code != Z_STREAM_END || stream.total_in != compressed || stream.total_out != plain)
                goto done;
        }
        if ((uint32_t)crc32(0, unpacked, (uInt)plain) != u32(c + 16))
            goto done;
        if (index >= 2 &&
            (plain < 52 || memcmp(unpacked, "\177ELF\1\1\1", 7) || u16(unpacked + 18) != 40 ||
             (u16(unpacked + 16) != 2 && u16(unpacked + 16) != 3) || u32(unpacked + 36) & 0x400u))
            goto done;
        if (index == 0 &&
            (plain < 10 || memcmp(unpacked, "#!/bin/sh\n", 10) || memchr(unpacked, 0, plain)))
            goto done;
        if (output) {
            char path[EC_PATH];
            if (ec_path(path, sizeof path, output, ec_update_targets[index]) ||
                ec_update_write(path, unpacked, plain))
                goto cleanup;
            if (index != 1 && chmod(path, 0755))
                goto done;
        }
        free(unpacked);
        unpacked = NULL;
        cursor += 46 + name_size + extra + note;
    }
    if (cursor != end || local_end != u32(data + end + 16) || seen != 127)
        goto done;
    result = 0;
    goto cleanup;
done:
    ec_error("Invalid, incomplete or incompatible Easy Connect update archive.");
cleanup:
    close(fd);
    free(data);
    free(unpacked);
    return result;
}
