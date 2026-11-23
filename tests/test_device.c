#include "device.h"
#include <assert.h>
#include <string.h>
int main(void) {
    assert(ec_device_supported("PocketBook 632 (PB632)", "U632.6.11.9999"));
    assert(ec_device_supported("PocketBook 634", "U634.6.10.9999"));
    assert(ec_device_supported("PB700", "U700.6.12.1"));
    assert(ec_device_supported(" PB743g ", "U743G.6.11.9999"));
    assert(ec_device_supported("PB743K3", "U743k3.6.10.2854"));
    assert(ec_device_supported(" PB743k3 ", "U743k3.6.10.9999"));
    assert(ec_device_supported("PocketBook 743K3 (PB743K3)", "U743k3.6.11.1"));
    assert(ec_device_supported("PocketBook 618 (PB618)", "U618.6.10.9999"));
    assert(!ec_device_supported("PB618", "U618.6.9.9999"));
    assert(ec_device_supported("PocketBook 619 (PB619)", "U619.6.10.9999"));
    assert(!ec_device_supported("PB619", "U619.6.9.9999"));
    assert(ec_device_supported("PocketBook 628 (PB628)", "U628.6.10.9999"));
    assert(!ec_device_supported("PB628", "U628.6.9.9999"));
    assert(ec_device_supported("PocketBook 629 (PB629)", "U629.6.10.9999"));
    assert(!ec_device_supported("PB629", "U629.6.9.9999"));
    assert(ec_device_supported("PocketBook 633 (PB633)", "U633.6.10.9999"));
    assert(!ec_device_supported("PB633", "U633.6.9.9999"));
    assert(ec_device_supported("PocketBook 634K3 (PB634K3)", "U634K3.6.10.9999"));
    assert(!ec_device_supported("PB634K3", "U634K3.6.9.9999"));
    assert(ec_device_supported("PocketBook 700K3 (PB700K3)", "U700K3.6.10.9999"));
    assert(!ec_device_supported("PB700K3", "U700K3.6.9.9999"));
    assert(ec_device_supported("PocketBook 710 (PB710)", "U710.6.10.9999"));
    assert(!ec_device_supported("PB710", "U710.6.9.9999"));
    assert(ec_device_supported("PocketBook 740 (PB740)", "U740.6.10.9999"));
    assert(!ec_device_supported("PB740", "U740.6.9.9999"));
    assert(ec_device_supported("PocketBook 740-2 (PB740-2)", "U740-2.6.10.9999"));
    assert(!ec_device_supported("PB740-2", "U740-2.6.9.9999"));
    assert(ec_device_supported("PocketBook 740-3 (PB740-3)", "U740-3.6.10.9999"));
    assert(!ec_device_supported("PB740-3", "U740-3.6.9.9999"));
    assert(ec_device_supported("PocketBook 741 (PB741)", "U741.6.10.9999"));
    assert(!ec_device_supported("PB741", "U741.6.9.9999"));
    assert(ec_device_supported("PocketBook 743C (PB743C)", "U743C.6.10.9999"));
    assert(!ec_device_supported("PB743C", "U743C.6.9.9999"));
    assert(ec_device_supported("PocketBook 970 (PB970)", "U970.6.10.9999"));
    assert(!ec_device_supported("PB970", "U970.6.9.9999"));
    assert(ec_device_supported("PocketBook 1040 (PB1040)", "U1040.6.10.9999"));
    assert(!ec_device_supported("PB1040", "U1040.6.9.9999"));
    const ec_device_profile *color = ec_device_identify("PocketBook 743k3");
    assert(color && !strcmp(color->model, "743K3") && !strcmp(color->name, "InkPad Color 3"));
    assert(ec_firmware_api("U743k3.6.10.2854") == 610);
    assert(!ec_device_supported("PB743K3", "U743k3.6.8.3671"));
    assert(!ec_device_supported("PB743K", "U743k3.6.10.2854"));
    assert(!ec_device_supported("PB1030", "U1030.6.11.1"));
    assert(!ec_device_supported("PB743G", "U743g.6.9.1"));
    assert(!ec_device_supported(NULL, "U632.6.11.1"));
    assert(ec_firmware_api("U743g.7.0.1") == 611);
    const char *invalid[] = {NULL, "", "6.11.1", "U743g.6.11", "U743g.6.11.x",
        "U743g.6.11.1 ", "U743g.999999999999.11.1", "U743g.6.111.1", "U743g.6.9.1"};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) assert(!ec_firmware_api(invalid[i]));
    assert(!strcmp(ec_version(), "0.3.3"));
    assert(!strcmp(ec_family(), "pocketbook-armv7-softfp"));
    return 0;
}
