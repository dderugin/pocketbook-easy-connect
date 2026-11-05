/* Stable Easy Connect channel, intentionally separate from CoolReader's channel. */
#include "update.h"
#include "device.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
static bool version(const char *s, unsigned v[3]) {
    if (!s || !*s || strlen(s) >= 24)
        return false;
    for (int i = 0; i < 3; ++i) {
        const char *start = s;
        v[i] = 0;
        while (*s >= '0' && *s <= '9') {
            if (s - start >= 6)
                return false;
            v[i] = v[i] * 10 + (unsigned)(*s++ - '0');
        }
        if (s == start || (s - start > 1 && *start == '0') || (i < 2 ? *s++ != '.' : *s != 0))
            return false;
    }
    return true;
}
bool ec_update_newer(const char *candidate, const char *installed) {
    unsigned a[3], b[3];
    if (!version(candidate, a) || !version(installed, b))
        return false;
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i])
            return a[i] > b[i];
    return false;
}
int ec_update_parse(const char *text, const char *model, const char *firmware,
                    ec_update_release *release) {
    size_t length = text ? strlen(text) : 0;
    if (!length || length > EC_UPDATE_MANIFEST_LIMIT || !ec_device_supported(model, firmware))
        return ec_error("Invalid update information or unsupported reader.");
    char copy[EC_UPDATE_MANIFEST_LIMIT + 1];
    memcpy(copy, text, length + 1);
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)copy[i] < 32 && !strchr("\r\n\t", copy[i]))
            return ec_error("Invalid update information.");
    char *save = NULL, *words[262];
    size_t n = 0;
    for (char *word = strtok_r(copy, " \t\r\n", &save); word;
         word = strtok_r(NULL, " \t\r\n", &save)) {
        if (n == 262)
            return ec_error("Update model registry is too large.");
        words[n++] = word;
    }
    unsigned parts[3];
    if (n < 6 || n % 2 || strcmp(words[0], "EC-OTA-SHARED-1") || strcmp(words[1], ec_family()) ||
        !version(words[2], parts) || strlen(words[3]) != 64 ||
        strspn(words[3], "0123456789abcdef") != 64)
        return ec_error("Invalid Easy Connect update channel.");
    const ec_device_profile *device = ec_device_identify(model);
    bool compatible = false;
    for (size_t i = 4; i < n; i += 2) {
        if (!*words[i] || strlen(words[i]) > 16 ||
            strspn(words[i], "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") != strlen(words[i]) ||
            !*words[i + 1] || strlen(words[i + 1]) > 4 ||
            strspn(words[i + 1], "0123456789") != strlen(words[i + 1]))
            return ec_error("Invalid update compatibility rule.");
        unsigned minimum = (unsigned)strtoul(words[i + 1], NULL, 10);
        if (minimum < 610 || minimum > 9999)
            return ec_error("Invalid update firmware API.");
        for (size_t j = 4; j < i; j += 2)
            if (!strcmp(words[i], words[j]))
                return ec_error("Duplicate update model.");
        if (!strcmp(words[i], device->model) && ec_firmware_api(firmware) >= minimum)
            compatible = true;
    }
    if (!compatible)
        return ec_error("No compatible update for this reader and firmware API.");
    memset(release, 0, sizeof *release);
    strcpy(release->version, words[2]);
    strcpy(release->checksum, words[3]);
    strcpy(release->text, text);
    return 0;
}
