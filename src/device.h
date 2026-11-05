#ifndef EASY_CONNECT_DEVICE_H
#define EASY_CONNECT_DEVICE_H
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *model;
    const char *name;
    unsigned minimum_api;
} ec_device_profile;

unsigned ec_firmware_api(const char *software);
const ec_device_profile *ec_device_identify(const char *description);
bool ec_device_supported(const char *description, const char *software);
const char *ec_version(void);
const char *ec_family(void);
#endif
