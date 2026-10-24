/* Own a temporary firmware scanner instance without touching the system scanner. */
#include "ui.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

enum { SCAN_TIMEOUT_MS = 120000, SCAN_TICK_MS = 100, SCAN_TIMEOUT_EXIT = 124 };
static volatile sig_atomic_t stopping;

static void stop_requested(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

static void pause_tick(void) {
    struct timespec delay = {0, SCAN_TICK_MS * 1000000};
    nanosleep(&delay, NULL);
}

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Keep a partial line so the completion marker can straddle read boundaries. */
static bool scan_finished(int fd, off_t *offset, char *tail, size_t *carried) {
    static const char marker[] = "Scan finished";
    char buffer[4096];
    size_t keep = *carried < sizeof marker ? *carried : 0;
    for (int i = 0; i < 64; ++i) {
        memcpy(buffer, tail, keep);
        ssize_t count = pread(fd, buffer + keep, sizeof buffer - keep - 1, *offset);
        if (count <= 0) {
            return false;
        }
        *offset += count;
        size_t used = keep + (size_t)count;
        buffer[used] = 0;
        if (strstr(buffer, marker)) {
            return true;
        }
        keep = used < sizeof marker - 2 ? used : sizeof marker - 2;
        *carried = keep;
        memcpy(tail, buffer + used - keep, keep);
    }
    return false;
}

static void finish_child(pid_t child) {
    /* An unreaped child owns its PID throughout this shutdown sequence. */
    kill(child, SIGTERM);
    for (int i = 0; i < 10; ++i) {
        int status;
        if (waitpid(child, &status, WNOHANG) == child) {
            return;
        }
        pause_tick();
    }
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
}

static void supervise_scan(const char *scanner, int log_fd, unsigned timeout_ms, pid_t parent) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = stop_requested;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    stopping = 0;
    setpgid(0, 0);
#ifdef __linux__
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent) {
        _exit(1);
    }
#else
    (void)parent;
#endif
    pid_t supervisor = getpid();
    pid_t child = fork();
    if (child < 0) {
        _exit(1);
    }
    if (!child) {
        action.sa_handler = SIG_DFL;
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
#ifdef __linux__
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != supervisor) {
            _exit(1);
        }
#else
        (void)supervisor;
#endif
        int input = open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0 ||
            dup2(log_fd, STDOUT_FILENO) < 0 || dup2(log_fd, STDERR_FILENO) < 0) {
            _exit(1);
        }
        if (input > 2) {
            close(input);
        }
        close(log_fd);
        execl(scanner, scanner, (char *)NULL);
        perror("library scanner");
        _exit(127);
    }
    long long deadline = monotonic_ms() + timeout_ms;
    off_t offset = 0;
    char tail[32];
    size_t carried = 0;
    while (!stopping) {
        int status;
        pid_t reaped = waitpid(child, &status, WNOHANG);
        if (reaped == child) {
            _exit(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1);
        }
        if (reaped < 0 && errno != EINTR) {
            _exit(1);
        }
        if (scan_finished(log_fd, &offset, tail, &carried)) {
            finish_child(child);
            _exit(0);
        }
        if (monotonic_ms() >= deadline) {
            finish_child(child);
            _exit(SCAN_TIMEOUT_EXIT);
        }
        pause_tick();
    }
    finish_child(child);
    _exit(1);
}

int ui_library_start(ui_app *app) {
    const char *scanner = SYSTEMSCANNER;
    unsigned timeout_ms = SCAN_TIMEOUT_MS;
#ifdef EC_UI_TEST
    /* Tests use a disposable executable, never the host's firmware paths. */
    scanner = getenv("EC_UI_SCANNER");
    if (getenv("EC_UI_SCAN_TIMEOUT_MS")) {
        timeout_ms = (unsigned)strtoul(getenv("EC_UI_SCAN_TIMEOUT_MS"), NULL, 10);
    }
#endif
    if (!scanner || access(scanner, X_OK)) {
        return ec_error("Library scanner unavailable on this firmware.");
    }
    char path[EC_PATH];
    if (ec_path(path, sizeof path, app->config.root, "library-scan.log")) {
        return -1;
    }
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat metadata;
    if (fd < 0) {
        return ec_error("Cannot open library-scan.log: %s", strerror(errno));
    }
    if (fstat(fd, &metadata) || !S_ISREG(metadata.st_mode) || ftruncate(fd, 0)) {
        close(fd);
        return ec_error("Cannot create a regular library scan log.");
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    pid_t parent = getpid();
    pid_t pid = fork();
    if (!pid) {
        supervise_scan(scanner, fd, timeout_ms, parent);
    }
    close(fd);
    if (pid < 0) {
        return ec_error("Cannot start library refresh: %s", strerror(errno));
    }
    app->library_refresh = pid;
    return 0;
}

int ui_library_poll(ui_app *app) {
    if (!app->library_refresh) {
        return 0;
    }
    int status;
    pid_t reaped = waitpid(app->library_refresh, &status, WNOHANG);
    if (!reaped || (reaped < 0 && errno == EINTR)) {
        return 0;
    }
    app->library_refresh = 0;
    if (reaped > 0 && WIFEXITED(status)) {
        if (WEXITSTATUS(status) == 0) {
            return 1;
        }
        if (WEXITSTATUS(status) == SCAN_TIMEOUT_EXIT) {
            return ec_error("Library refresh timed out; see library-scan.log.");
        }
    }
    return ec_error("Library refresh failed; see library-scan.log.");
}

void ui_library_stop(ui_app *app) {
    pid_t child = app->library_refresh;
    if (!child) return;
    int status;
    pid_t reaped = waitpid(child, &status, WNOHANG);
    if (reaped == 0) {
        kill(child, SIGTERM);
        /* Allow the supervisor to finish its scanner's own grace period. */
        for (int i = 0; i < 30; ++i) {
            reaped = waitpid(child, &status, WNOHANG);
            if (reaped == child || (reaped < 0 && errno == ECHILD)) break;
            pause_tick();
        }
        if (reaped == 0) {
            kill(child, SIGKILL);
            while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    app->library_refresh = 0;
}
