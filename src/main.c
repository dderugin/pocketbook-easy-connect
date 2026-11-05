#include "ec.h"
#include "device.h"
#include "update.h"
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
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        printf("Easy Connect %s (%s)\n", ec_version(), ec_family());
        return 0;
    }
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
    if (!strcmp(command, "update-install") && argc == argument_index)
        return ec_update_install(&config) != 0;
    if (!strcmp(command, "update-rollback") && argc == argument_index)
        return ec_update_rollback(&config) != 0;
    if (!strcmp(command, "doctor") && argc == argument_index) {
        return ec_doctor(&config, stdout) != 0;
    }
    if (!strcmp(command, "fingerprint") && argc == argument_index) {
        return ec_fingerprint(&config) != 0;
    }
    if (!strcmp(command, "background-controller") && argc == argument_index)
        return ec_background_serve(&config);
    if (!strcmp(command, "background-status") && argc == argument_index) {
        ec_services remote;
        if (ec_background_poll(&config, &remote)) return 1;
        printf("ssh=%ld ftp=%ld owner=%ld runtime=%s\n", (long)remote.ssh, (long)remote.ftp, (long)remote.controller, config.runtime);
        return 0;
    }
    if (!strcmp(command, "background-start") && argc == argument_index + 1) {
        const char *name = argv[argument_index];
        if (strcmp(name, "ssh") && strcmp(name, "ftp")) return 2;
        char program[EC_PATH];
        ec_services remote;
        if (ec_path(program, sizeof program, config.root, "bin/easy-connect")) return 1;
        return ec_background_start(&config, &remote, name, program) != 0;
    }
    if (!strcmp(command, "background-stop") && argc == argument_index + 1) {
        const char *name = argv[argument_index];
        if (strcmp(name, "ssh") && strcmp(name, "ftp") && strcmp(name, "both")) return 2;
        ec_services remote;
        if (strcmp(name, "ftp") && ec_background_stop(&config, &remote, "ssh")) return 1;
        if (strcmp(name, "ssh") && ec_background_stop(&config, &remote, "ftp")) return 1;
        return 0;
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
