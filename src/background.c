/* A headless owner keeps services alive while InkView is closed. The private
 * Unix socket controls owned supervisors; clients never signal stored PIDs. */
#include "ec.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { BG_MAGIC = 0x45434731, BG_STATUS = 1, BG_START_SSH, BG_START_FTP, BG_STOP_SSH, BG_STOP_FTP };
typedef struct { uint32_t magic, operation; char root[EC_PATH]; } bg_request;
typedef struct {
    uint32_t magic;
    int32_t result, ssh, ftp, owner;
    ec_config config;
    char error[512];
} bg_response;
static volatile sig_atomic_t background_stopping;

static void background_stop(int signal_number) {
    (void)signal_number;
    background_stopping = 1;
}

int ec_detach_process_io(int log_fd) {
    int input = open("/dev/null", O_RDONLY);
    if (input < 0 || dup2(input, STDIN_FILENO) < 0 ||
        dup2(log_fd, STDOUT_FILENO) < 0 || dup2(log_fd, STDERR_FILENO) < 0) return -1;
    if (input > 2) close(input);
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) directory = opendir("/dev/fd");
    if (!directory) return -1;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        char *end;
        long fd = strtol(entry->d_name, &end, 10);
        if (*entry->d_name && !*end && fd > 2 && fd != log_fd && fd != dirfd(directory))
            close((int)fd);
    }
    closedir(directory);
    return 0;
}

static int control_path(char *path, size_t size, const char *name) {
    char directory[128];
    snprintf(directory, sizeof directory, "/tmp/easy-connect-%lu", (unsigned long)geteuid());
    if (ec_private_dir(directory)) return -1;
    return ec_path(path, size, directory, name);
}

static int local_socket(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct timeval timeout = {8, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    return fd;
}

static bool owned_peer(int fd) {
#ifdef __linux__
    struct ucred credentials;
    socklen_t size = sizeof credentials;
    return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) && credentials.uid == geteuid();
#else
    uid_t uid; gid_t gid;
    return !getpeereid(fd, &uid, &gid) && uid == geteuid();
#endif
}

static int transfer(int fd, void *buffer, size_t size, bool sending) {
    char *cursor = buffer;
    while (size) {
#ifdef MSG_NOSIGNAL
        int flags = MSG_NOSIGNAL;
#else
        int flags = 0;
#endif
        ssize_t count = sending ? send(fd, cursor, size, flags) : recv(fd, cursor, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        cursor += count;
        size -= (size_t)count;
    }
    return 0;
}

/* -2 means no controller; all other failures are real errors, never a reason to
 * remove another process's socket or trust an unverified PID. */
static int connect_controller(void) {
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    if (control_path(address.sun_path, sizeof address.sun_path, "control.sock")) return -1;
    struct stat metadata;
    if (lstat(address.sun_path, &metadata)) return errno == ENOENT ? -2 : -1;
    if (!S_ISSOCK(metadata.st_mode) || metadata.st_uid != geteuid() || (metadata.st_mode & 0077))
        return ec_error("background controller socket must be owned and private");
    int fd = local_socket();
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof address)) {
        int error = errno;
        close(fd);
        return error == ENOENT || error == ECONNREFUSED ? -2 : -1;
    }
    if (!owned_peer(fd)) {
        close(fd);
        return ec_error("background controller has another owner");
    }
    return fd;
}

static int launch_controller(const char *root, const char *program) {
    char log[EC_PATH];
    if (control_path(log, sizeof log, "controller.log")) return -1;
    int fd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat metadata;
    if (fd < 0) return ec_error("cannot open background controller log");
    if (fd < 3) {
        int original = fd;
        fd = fcntl(original, F_DUPFD_CLOEXEC, 3);
        close(original);
        if (fd < 0) return ec_error("cannot retain background controller log");
    }
    if (fstat(fd, &metadata) || !S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() ||
        (metadata.st_mode & 0077)) {
        close(fd);
        return ec_error("background controller log must be owned and private");
    }
    pid_t child = fork();
    if (!child) {
        if (setsid() < 0) _exit(1);
        pid_t worker = fork();
        if (worker < 0) _exit(1);
        if (worker > 0) _exit(0);
        if (ec_detach_process_io(fd)) _exit(1);
        close(fd);
        execl(program, program, "--root", root, "background-controller", (char *)NULL);
        perror("background controller");
        _exit(127);
    }
    close(fd);
    if (child < 0) return ec_error("cannot fork background controller");
    int status;
    pid_t reaped;
    do { reaped = waitpid(child, &status, 0); } while (reaped < 0 && errno == EINTR);
    return reaped == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 :
           ec_error("cannot detach background controller");
}

static int call_controller(ec_config *config, ec_services *services, unsigned operation,
                            const char *program) {
    int fd = connect_controller();
    if (fd == -2 && program) {
        if (launch_controller(config->root, program)) return -1;
        for (int i = 0; i < 150 && fd == -2; ++i) {
            struct timespec delay = {0, 20000000};
            nanosleep(&delay, NULL);
            fd = connect_controller();
        }
    }
    if (fd == -2 && !program) {
        memset(services, 0, sizeof *services);
        services->lock_fd = -1;
        config->runtime[0] = 0;
        return 0;
    }
    if (fd < 0) return ec_error("cannot reach background controller; see /tmp/easy-connect-%lu/controller.log",
                                (unsigned long)geteuid());
    bg_request request = {.magic = BG_MAGIC, .operation = operation};
    snprintf(request.root, sizeof request.root, "%s", config->root);
    bg_response response;
    int result = transfer(fd, &request, sizeof request, true) ||
                 transfer(fd, &response, sizeof response, false);
    close(fd);
    if (result || response.magic != BG_MAGIC || response.error[sizeof response.error - 1])
        return ec_error("background controller protocol failed");
    if (response.config.root[sizeof response.config.root - 1] ||
        strcmp(response.config.root, request.root))
        return ec_error("%s", response.result ? response.error : "background controller installation mismatch");
    *config = response.config;
    services->ssh = response.ssh;
    services->ftp = response.ftp;
    services->lock_fd = -1;
    services->controller = response.owner;
    if (response.result) return ec_error("%s", response.error);
    return 0;
}

int ec_background_poll(ec_config *config, ec_services *services) {
    return call_controller(config, services, BG_STATUS, NULL);
}

int ec_background_start(ec_config *config, ec_services *services, const char *name,
                         const char *program) {
    return call_controller(config, services, !strcmp(name, "ssh") ? BG_START_SSH : BG_START_FTP, program);
}

int ec_background_stop(ec_config *config, ec_services *services, const char *name) {
    return call_controller(config, services, !strcmp(name, "ssh") ? BG_STOP_SSH : BG_STOP_FTP, NULL);
}

/* Saved changes for stopped services become visible after reopening. Preserve
 * the configuration actually used by every listener that is still running. */
static int reload_config(ec_config *config, const ec_services *services) {
    ec_config fresh;
    if (ec_config_load(&fresh, config->root)) return -1;
    snprintf(fresh.runtime, sizeof fresh.runtime, "%s", config->runtime);
    if (services->ssh || services->ftp) {
        snprintf(fresh.bind_address, sizeof fresh.bind_address, "%s", config->bind_address);
        snprintf(fresh.state, sizeof fresh.state, "%s", config->state);
    }
    if (services->ssh) { fresh.ssh_port = config->ssh_port; fresh.auth = config->auth; }
    if (services->ftp) {
        fresh.ftp_port = config->ftp_port; fresh.ftp_auth = config->ftp_auth;
        fresh.pasv_min = config->pasv_min; fresh.pasv_max = config->pasv_max;
        snprintf(fresh.ftp_root, sizeof fresh.ftp_root, "%s", config->ftp_root);
        snprintf(fresh.ftp_user, sizeof fresh.ftp_user, "%s", config->ftp_user);
    }
    if (ec_config_validate(&fresh)) return -1;
    *config = fresh;
    return 0;
}

int ec_background_serve(ec_config *config) {
    ec_services services;
    if (ec_services_open(config, &services)) return 1;
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    int listener = -1, result = 1;
    if (control_path(address.sun_path, sizeof address.sun_path, "control.sock")) goto done;
    listener = local_socket();
    if (listener < 0) goto done;
    /* Only the exclusive service owner may replace a stale endpoint. */
    struct stat metadata;
    if (!lstat(address.sun_path, &metadata) &&
        (!S_ISSOCK(metadata.st_mode) || metadata.st_uid != geteuid())) goto done;
    if (unlink(address.sun_path) && errno != ENOENT) goto done;
    if (bind(listener, (struct sockaddr *)&address, sizeof address) ||
        chmod(address.sun_path, 0600) || listen(listener, 4)) goto done;
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = background_stop;
    background_stopping = 0;
    sigaction(SIGTERM, &action, NULL); sigaction(SIGINT, &action, NULL); sigaction(SIGHUP, &action, NULL);
    time_t startup_deadline = time(NULL) + 5;
    bool handled = false;
    result = 0;
    while (!background_stopping) {
        ec_poll(&services);
        if (!services.ssh && !services.ftp && (handled || time(NULL) > startup_deadline)) break;
        fd_set reads;
        FD_ZERO(&reads); FD_SET(listener, &reads);
        struct timeval delay = {1, 0};
        if (select(listener + 1, &reads, NULL, NULL, &delay) <= 0) continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) continue;
        struct timeval timeout = {8, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        fcntl(client, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        int enabled = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
        bg_request request;
        if (!owned_peer(client) || transfer(client, &request, sizeof request, false) ||
            request.magic != BG_MAGIC || request.root[sizeof request.root - 1]) {
            close(client); continue;
        }
        ec_poll(&services);
        bg_response response = {.magic = BG_MAGIC, .owner = (int32_t)getpid()};
        if (strcmp(request.root, config->root))
            response.result = ec_error("background services belong to another Easy Connect installation");
        else if (request.operation == BG_STATUS) {
            /* Invalid saved edits must not terminate existing connections. */
            (void)reload_config(config, &services);
        } else if (request.operation == BG_START_SSH || request.operation == BG_START_FTP) {
            response.result = reload_config(config, &services);
            if (!response.result)
                response.result = ec_start(config, &services, request.operation == BG_START_SSH ? "ssh" : "ftp");
        } else if (request.operation == BG_STOP_SSH || request.operation == BG_STOP_FTP)
            response.result = ec_stop(config, &services, request.operation == BG_STOP_SSH ? "ssh" : "ftp");
        else response.result = ec_error("unknown background controller operation");
        response.config = *config;
        response.ssh = (int32_t)services.ssh; response.ftp = (int32_t)services.ftp;
        if (response.result) snprintf(response.error, sizeof response.error, "%s", ec_last_error());
        (void)transfer(client, &response, sizeof response, true);
        close(client);
        if (!strcmp(request.root, config->root) && request.operation >= BG_START_SSH &&
            request.operation <= BG_STOP_FTP) handled = true;
    }
    unlink(address.sun_path);
done:
    if (listener >= 0) close(listener);
    ec_services_close(config, &services);
    return result;
}
