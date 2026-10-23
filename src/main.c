#include "ec.h"
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t stopping = 0;

static void stop(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

int main(int argc, char **argv) {
    umask(0077);
    const char *root = EC_DEFAULT_ROOT;
    int argument_index = 1;
    if (argc > 2 && !strcmp(argv[1], "--root")) {
        root = argv[2];
        argument_index = 3;
    }
    if (argc <= argument_index) {
        fprintf(stderr,
                "Usage: easy-connect [--root DIRECTORY] doctor|fingerprint|serve ssh|ftp|both\n");
        return 2;
    }
    ec_config config;
    if (ec_config_load(&config, root)) {
        return 1;
    }
    const char *command = argv[argument_index++];
    if (!strcmp(command, "doctor") && argc == argument_index) {
        return ec_doctor(&config, stdout) != 0;
    }
    if (!strcmp(command, "fingerprint") && argc == argument_index) {
        return ec_fingerprint(&config) != 0;
    }
    if (strcmp(command, "serve") || argc != argument_index + 1 ||
        (strcmp(argv[argument_index], "ssh") && strcmp(argv[argument_index], "ftp") &&
         strcmp(argv[argument_index], "both"))) {
        return 2;
    }
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = stop;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    ec_services services;
    if (ec_services_open(&config, &services)) {
        return 1;
    }
    int result = 0;
    if (strcmp(argv[argument_index], "ftp")) {
        result = ec_start(&config, &services, "ssh");
    }
    if (!result && strcmp(argv[argument_index], "ssh")) {
        result = ec_start(&config, &services, "ftp");
    }
    while (!result && !stopping) {
        ec_poll(&services);
        if ((!strcmp(argv[argument_index], "ssh") && !services.ssh) ||
            (!strcmp(argv[argument_index], "ftp") && !services.ftp) ||
            (!strcmp(argv[argument_index], "both") && (!services.ssh || !services.ftp))) {
            result = -1;
            break;
        }
        struct timespec delay = {0, 100000000};
        nanosleep(&delay, NULL);
    }
    ec_services_close(&config, &services);
    return result != 0;
}
