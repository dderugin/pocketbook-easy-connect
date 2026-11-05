#include "device.h"
#include <assert.h>
#include <string.h>
int main(void) {
    assert(ec_device_supported("PocketBook 632 (PB632)", "U632.6.11.9999"));
    assert(ec_device_supported("PocketBook 634", "U634.6.10.9999"));
    assert(ec_device_supported("PB700", "U700.6.12.1"));
    assert(ec_device_supported(" PB743g ", "U743G.6.11.9999"));
    assert(!ec_device_supported("PB1030", "U1030.6.11.1"));
    assert(!ec_device_supported("PB743G", "U743g.6.9.1"));
    assert(!ec_device_supported(NULL, "U632.6.11.1"));
    assert(ec_firmware_api("U743g.7.0.1") == 611);
    const char *invalid[] = {NULL, "", "6.11.1", "U743g.6.11", "U743g.6.11.x",
        "U743g.6.11.1 ", "U743g.999999999999.11.1", "U743g.6.111.1", "U743g.6.9.1"};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) assert(!ec_firmware_api(invalid[i]));
    assert(!strcmp(ec_version(), "0.3.0"));
    assert(!strcmp(ec_family(), "pocketbook-armv7-softfp"));
    return 0;
}
