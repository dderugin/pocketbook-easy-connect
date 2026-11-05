/* Shared model and firmware policy, matching CoolReader's API-level admission. */
#include "device.h"
#include "ec_platform.h"
#include <ctype.h>
#include <limits.h>
#include <string.h>

static bool number(const char *begin, const char *end, unsigned *value) {
    *value = 0;
    if (begin == end || end - begin > 10) return false;
    for (const char *p = begin; p < end; ++p) {
        if (*p < '0' || *p > '9' || *value > (UINT_MAX - (unsigned)(*p - '0')) / 10) return false;
        *value = *value * 10 + (unsigned)(*p - '0');
    }
    return true;
}

unsigned ec_firmware_api(const char *software) {
    if (!software || strlen(software) > 256) return 0;
    const char *dots[3] = {NULL, NULL, NULL};
    for (const char *p = software; *p; ++p) {
        if (*p == '.') { dots[0] = dots[1]; dots[1] = dots[2]; dots[2] = p; }
    }
    if (!dots[0] || dots[0] == software) return 0;
    unsigned major, minor, build;
    if (!number(dots[0] + 1, dots[1], &major) || !number(dots[1] + 1, dots[2], &minor) ||
        !number(dots[2] + 1, software + strlen(software), &build) ||
        !major || major > UINT_MAX / 100 - 1 || minor > 99) return 0;
    unsigned api = major * 100 + minor;
    return api >= 611 ? 611 : api >= 610 ? 610 : 0;
}

const ec_device_profile *ec_device_identify(const char *description) {
    if (!description || strlen(description) >= 128) return NULL;
    const char *begin = description, *end = description + strlen(description);
    const char *parenthesis = strstr(description, "(PB");
    if (parenthesis && strchr(parenthesis, ')')) {
        begin = parenthesis + 1; end = strchr(parenthesis, ')');
    }
    char normalized[128]; size_t length = 0;
    for (const char *p = begin; p < end; ++p)
        if (!isspace((unsigned char)*p)) normalized[length++] = *p;
    normalized[length] = 0;
    const char *model = normalized;
    if (!strncmp(model, "PocketBook", 10)) model += 10;
    if (!strncmp(model, "PB", 2)) model += 2;
    for (char *p = normalized + (model - normalized); *p; ++p) *p = (char)toupper((unsigned char)*p);
    for (size_t i = 0; i < sizeof ec_devices / sizeof ec_devices[0]; ++i)
        if (!strcmp(model, ec_devices[i].model)) return &ec_devices[i];
    return NULL;
}

bool ec_device_supported(const char *description, const char *software) {
    const ec_device_profile *device = ec_device_identify(description);
    return device && ec_firmware_api(software) >= device->minimum_api;
}

const char *ec_version(void) { return EC_VERSION; }
const char *ec_family(void) { return EC_FAMILY; }
