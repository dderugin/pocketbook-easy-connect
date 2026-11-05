#include "update.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "newer"))
        return ec_update_newer(argv[2], argv[3]) ? 0 : 1;
    if (argc >= 3 && !strcmp(argv[1], "fetch")) {
        ec_update_transfer *t = ec_update_fetch(argv[2], argc > 3 ? argv[3] : NULL,
                                                argc > 4 ? (size_t)atol(argv[4]) : 16384);
        if (!t)
            return 1;
        int status;
        while (!(status = ec_update_poll(t))) {
            struct timespec delay = {0, 1000000};
            nanosleep(&delay, NULL);
        }
        if (status > 0)
            puts(ec_update_body(t));
        ec_update_free(t);
        return status < 0;
    }
    if (argc < 6)
        return 2;
    ec_config config;
    if (ec_config_load(&config, argv[2]))
        return 1;
    char text[EC_UPDATE_MANIFEST_LIMIT + 1];
    ec_update_release release;
    if (ec_update_read(argv[3], text, sizeof text) ||
        ec_update_parse(text, argv[4], argv[5], &release))
        return 1;
    if (!strcmp(argv[1], "parse")) {
        puts(release.version);
        return 0;
    }
    if (argc != 7)
        return 2;
    if (!strcmp(argv[1], "archive"))
        return ec_update_archive(argv[6], &release, argv[4], argv[5], NULL) != 0;
    if (!strcmp(argv[1], "stage")) {
        int lock = ec_update_lock();
        if (lock < 0)
            return 1;
        int result = ec_update_stage(&config, argv[6], &release, argv[4], argv[5]);
        close(lock);
        return result != 0;
    }
    if (!strcmp(argv[1], "request"))
        return ec_update_request(&config, argv[4], argv[5]) != 0;
    return 2;
}
