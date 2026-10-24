#include "ec.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

enum {
    SERVICE_POLL_INTERVAL_MS = 20,
    SERVICE_START_TIMEOUT_MS = 2000,
    SERVICE_STOP_TIMEOUT_MS = 3000,
    SERVICE_TERMINATE_GRACE_MS = 500,
    LISTENER_PROBE_TIMEOUT_US = 10000
};

static void pause_ms(unsigned ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {
    }
}

static int child_status(pid_t pid) {
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
        return ec_error("child command failed; see service log");
    }
    return 0;
}

static int run_command(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        return ec_error("fork: %s", strerror(errno));
    }
    if (!pid) {
        execv(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    return child_status(pid);
}

int ec_services_open(ec_config *config, ec_services *services) {
    memset(services, 0, sizeof *services);
    services->lock_fd = -1;
    char lock_directory[EC_PATH], path[EC_PATH];
    snprintf(lock_directory, sizeof lock_directory, "/tmp/easy-connect-%lu",
             (unsigned long)geteuid());
    if (ec_private_dir(lock_directory) || ec_path(path, sizeof path, lock_directory, "lock")) {
        return -1;
    }
    services->lock_fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (services->lock_fd < 0) {
        return ec_error("open lock: %s", strerror(errno));
    }
    fcntl(services->lock_fd, F_SETFD, FD_CLOEXEC);
    if (flock(services->lock_fd, LOCK_EX | LOCK_NB)) {
        close(services->lock_fd);
        services->lock_fd = -1;
        return ec_error("another Easy Connect session is running");
    }
    if (ec_path(config->runtime, sizeof config->runtime, lock_directory, "session.XXXXXX") ||
        !mkdtemp(config->runtime)) {
        close(services->lock_fd);
        services->lock_fd = -1;
        return ec_error("cannot create private session directory");
    }
#ifdef __linux__
    /* Adopt daemon session grandchildren so closing the app closes sessions. */
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0)) {
        ec_services_close(config, services);
        return ec_error("kernel lacks child subreaper support");
    }
#endif
    return 0;
}

static int stage_credential(ec_config *config, const char *name, int required) {
    char source[EC_PATH], target[EC_PATH];
    if (ec_path(source, sizeof source, config->state, name) ||
        ec_path(target, sizeof target, config->runtime, name)) {
        return -1;
    }
    return ec_copy_private(source, target, required);
}

static int prepare_host_key(ec_config *config) {
    char key[EC_PATH], tool[EC_PATH];
    if (ec_path(key, sizeof key, config->state, "host_ed25519") ||
        ec_path(tool, sizeof tool, config->root, "bin/dropbearkey")) {
        return -1;
    }
    struct stat metadata;
    if (lstat(config->state, &metadata)) {
        if (errno != ENOENT || mkdir(config->state, 0700)) {
            return ec_error("create state directory: %s", strerror(errno));
        }
    } else if (!S_ISDIR(metadata.st_mode)) {
        return ec_error("state directory must not be a symlink");
    }
    if (lstat(key, &metadata)) {
        if (errno != ENOENT) {
            return ec_error("stat host key: %s", strerror(errno));
        }
        char temp[EC_PATH];
        if (ec_path(temp, sizeof temp, config->runtime, "new_host_ed25519")) {
            return -1;
        }
        char *argv[] = {tool, "-t", "ed25519", "-f", temp, NULL};
        if (run_command(argv)) {
            return -1;
        }
        /* O_EXCL prevents overwriting a key installed concurrently over USB. */
        int fd = open(key, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) {
            unlink(temp);
            return ec_error("create host key: %s", strerror(errno));
        }
        close(fd);
        if (ec_copy_private(temp, key, 1)) {
            unlink(temp);
            unlink(key);
            return -1;
        }
        unlink(temp);
    }
    return stage_credential(config, "host_ed25519", 1);
}

static int check_port_available(ec_config *config, unsigned port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), rc;
    if (fd < 0) {
        return ec_error("socket: %s", strerror(errno));
    }
    /* Match the daemons: closed connections must not prevent a restart. */
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse)) {
        int error = errno;
        close(fd);
        return ec_error("set socket reuse: %s", strerror(error));
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, config->bind_address, &addr.sin_addr);
    rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    int error = errno;
    close(fd);
    return rc ? ec_error("cannot bind %s:%u: %s", config->bind_address, port, strerror(error)) : 0;
}

static int port_is_listening(ec_config *config, unsigned port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET,
              !strcmp(config->bind_address, "0.0.0.0") ? "127.0.0.1" : config->bind_address,
              &addr.sin_addr);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set writes;
        struct timeval timeout = {0, LISTENER_PROBE_TIMEOUT_US};
        FD_ZERO(&writes);
        FD_SET(fd, &writes);
        if (select(fd + 1, NULL, &writes, NULL, &timeout) > 0) {
            int err = 0;
            socklen_t size = sizeof err;
            if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &size) && !err) {
                rc = 0;
            }
        }
    }
    close(fd);
    return rc == 0;
}

static volatile sig_atomic_t service_stopping = 0;

static void request_service_stop(int sig) {
    (void)sig;
    service_stopping = 1;
}

static void signal_direct_children(int sig) {
#ifdef __linux__
    char path[128];
    snprintf(path, sizeof path, "/proc/self/task/%ld/children", (long)getpid());
    FILE *file = fopen(path, "r");
    if (file) {
        long pid;
        while (fscanf(file, "%ld", &pid) == 1) {
            if (pid > 1) {
                kill((pid_t)pid, sig);
            }
        }
        fclose(file);
    }
#else
    (void)sig;
#endif
}

/* One supervisor per daemon owns its session descendants on Linux. */
static void supervise(char *const argv[], pid_t expected_parent) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = request_service_stop;
    service_stopping = 0;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
#ifdef __linux__
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) || prctl(PR_SET_PDEATHSIG, SIGTERM) ||
        getppid() != expected_parent) {
        _exit(126);
    }
#else
    (void)expected_parent;
#endif
    pid_t parent = getpid(), daemon = fork();
    if (daemon < 0) {
        _exit(126);
    }
    if (!daemon) {
        action.sa_handler = SIG_DFL;
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
#ifdef __linux__
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent) {
            _exit(126);
        }
#else
        (void)parent;
#endif
        execv(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    int status = 0, code = 0, shutdown_ticks = 0;
    while (1) {
        pid_t child;
        while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
            if (child == daemon) {
                daemon = 0;
                code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
                service_stopping = 1;
            }
        }
        if (child < 0 && errno == ECHILD) {
            _exit(code);
        }
        if (service_stopping) {
            int sig = shutdown_ticks++ < SERVICE_TERMINATE_GRACE_MS / SERVICE_POLL_INTERVAL_MS
                          ? SIGTERM
                          : SIGKILL;
            if (daemon > 0) {
                kill(daemon, sig);
            }
            signal_direct_children(sig);
        }
        pause_ms(SERVICE_POLL_INTERVAL_MS);
    }
}

static int launch_daemon(ec_config *config, pid_t *slot, char *const argv[], const char *name,
                         unsigned port) {
    char log[EC_PATH], filename[64], password[EC_PATH];
    snprintf(filename, sizeof filename, "%s.log", name);
    if (ec_path(log, sizeof log, config->runtime, filename) ||
        ec_path(password, sizeof password, config->runtime,
                !strcmp(name, "ftp") ? "ftp-password.hash" : "password.hash")) {
        return -1;
    }
    if (access(argv[0], X_OK)) {
        return ec_error("missing executable %s", argv[0]);
    }
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return ec_error("create log: %s", strerror(errno));
    }
    pid_t expected_parent = getpid();
    pid_t pid = fork();
    if (!pid) {
        setpgid(0, 0);
        int input = open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0 ||
            dup2(fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        if (input > 2) {
            close(input);
        }
        if (fd > 2) {
            close(fd);
        }
        setenv("EASY_CONNECT_PASSWORD_FILE", password, 1);
        supervise(argv, expected_parent);
    }
    close(fd);
    if (pid < 0) {
        return ec_error("fork: %s", strerror(errno));
    }
    setpgid(pid, pid);
    *slot = pid;
    for (int i = 0; i < SERVICE_START_TIMEOUT_MS / SERVICE_POLL_INTERVAL_MS; ++i) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            *slot = 0;
            return ec_error("%s exited during startup; inspect %s", name, log);
        }
        if (port_is_listening(config, port)) {
            printf("%s listening on %s:%u; log %s\n", name, config->bind_address, port, log);
            fflush(stdout);
            return 0;
        }
        pause_ms(SERVICE_POLL_INTERVAL_MS);
    }
    /* Keep ownership of the child until stop_all can reap it. */
    return ec_error("%s did not open its port; inspect %s", name, log);
}

static int start_ssh(ec_config *config, ec_services *services) {
    char daemon[EC_PATH], key[EC_PATH], keys[EC_PATH], address[96], pid_file[EC_PATH];
    if (services->ssh) {
        return ec_error("SSH already running");
    }
    if (check_port_available(config, config->ssh_port) || prepare_host_key(config)) {
        return -1;
    }
    if (ec_path(daemon, sizeof daemon, config->root, "bin/dropbear") ||
        ec_path(key, sizeof key, config->runtime, "host_ed25519") ||
        ec_path(keys, sizeof keys, config->runtime, "authorized_keys") ||
        ec_path(pid_file, sizeof pid_file, config->runtime, "dropbear.pid")) {
        return -1;
    }
    if (config->auth != EC_PASSWORD) {
        if (stage_credential(config, "authorized_keys", 1)) {
            return -1;
        }
    } else {
        int fd = open(keys, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            return ec_error("cannot create empty authorized_keys");
        }
        close(fd);
    }
    if (config->auth != EC_KEYS && stage_credential(config, "password.hash", 1)) {
        return -1;
    }
    snprintf(address, sizeof address, "%s:%u", config->bind_address, config->ssh_port);
    char *argv[] = {daemon,
                    "-F",
                    "-E", /* Foreground, log to stderr. */
                    "-p",
                    address,
                    "-r",
                    key,
                    "-D",
                    config->runtime,
                    "-P",
                    pid_file,
                    "-T",
                    "3", /* Authentication attempts. */
                    "-I",
                    "900", /* Idle timeout in seconds. */
                    "-j",
                    "-k", /* Disable SSH port forwarding. */
                    config->auth == EC_KEYS ? "-s" : NULL,
                    NULL};
    return launch_daemon(config, &services->ssh, argv, "ssh", config->ssh_port);
}

/* Group configuration by purpose so each format argument stays near its key. */
static void write_ftp_config(FILE *file, const ec_config *config, int rootless,
                             const char *user_list_path, const char *chroot_directory) {
    int anonymous = config->ftp_auth == EC_FTP_ANONYMOUS;
    fprintf(file,
            "listen=YES\n"
            "background=NO\n"
            "listen_address=%s\n"
            "listen_port=%u\n",
            config->bind_address, config->ftp_port);
    fprintf(file,
            "anonymous_enable=%s\n"
            "local_enable=%s\n",
            anonymous ? "YES" : "NO", anonymous ? "NO" : "YES");
    fputs("write_enable=YES\n"
          "no_anon_password=YES\n"
          "anon_upload_enable=YES\n"
          "anon_mkdir_write_enable=YES\n"
          "anon_other_write_enable=YES\n"
          "anon_world_readable_only=NO\n"
          "anon_umask=022\n",
          file);
    fprintf(file,
            "chroot_local_user=%s\n"
            "allow_writeable_chroot=YES\n"
            "local_root=%s\n"
            "run_as_launching_user=%s\n"
            "one_process_model=%s\n",
            rootless ? "NO" : "YES", config->ftp_root, rootless ? "YES" : "NO",
            rootless ? "YES" : "NO");
    fprintf(file,
            "ftp_username=%s\n"
            "anon_root=%s\n"
            "nopriv_user=%s\n"
            "secure_chroot_dir=%s\n",
            config->ftp_user, config->ftp_root, config->ftp_user, chroot_directory);
    fprintf(file,
            "userlist_enable=YES\n"
            "userlist_deny=NO\n"
            "userlist_file=%s\n"
            "check_shell=NO\n",
            user_list_path);
    fprintf(file,
            "pasv_enable=YES\n"
            "port_enable=NO\n"
            "pasv_min_port=%u\n"
            "pasv_max_port=%u\n",
            config->pasv_min, config->pasv_max);
    fprintf(file,
            "max_clients=3\n"
            "max_per_ip=2\n"
            "idle_session_timeout=300\n"
            "local_umask=022\n"
            "session_support=NO\n"
            "xferlog_enable=NO\n"
            "seccomp_sandbox=%s\n",
            rootless ? "NO" : "YES");
}

static int start_ftp(ec_config *config, ec_services *services) {
    if (services->ftp) {
        return ec_error("FTP already running");
    }
    int rootless = geteuid() != 0;
    struct passwd *account = getpwnam(config->ftp_user);
    if (!account || account->pw_uid == 0) {
        return ec_error("FTP requires an existing non-root account: %s", config->ftp_user);
    }
    if (rootless && account->pw_uid != geteuid()) {
        return ec_error("FTP must use the launching account; set ftp_user=current");
    }
    char daemon[EC_PATH], config_path[EC_PATH], user_list_path[EC_PATH], chroot_directory[EC_PATH];
    if (check_port_available(config, config->ftp_port) ||
        ec_path(daemon, sizeof daemon, config->root, "bin/vsftpd") ||
        ec_path(config_path, sizeof config_path, config->runtime, "vsftpd.conf") ||
        ec_path(user_list_path, sizeof user_list_path, config->runtime, "ftp-users") ||
        ec_path(chroot_directory, sizeof chroot_directory, config->runtime, "empty")) {
        return -1;
    }
    int anonymous = config->ftp_auth == EC_FTP_ANONYMOUS;
    if (!anonymous && stage_credential(config, "ftp-password.hash", 1)) {
        return -1;
    }
    struct stat metadata;
    if (lstat(config->ftp_root, &metadata) || !S_ISDIR(metadata.st_mode)) {
        return ec_error("FTP root must be an existing directory, not a symlink");
    }
    if (ec_private_dir(chroot_directory)) {
        return -1;
    }
    FILE *file = fopen(user_list_path, "w");
    if (!file) {
        return ec_error("cannot write FTP user list");
    }
    fprintf(file, "%s\n", anonymous ? "anonymous\nftp" : config->ftp_user);
    if (fclose(file)) {
        return ec_error("cannot save FTP user list");
    }
    file = fopen(config_path, "w");
    if (!file) {
        return ec_error("cannot write FTP config");
    }
    write_ftp_config(file, config, rootless, user_list_path, chroot_directory);
    if (fclose(file)) {
        return ec_error("cannot save FTP config");
    }
    char *argv[] = {daemon, config_path, NULL};
    return launch_daemon(config, &services->ftp, argv, "ftp", config->ftp_port);
}

int ec_start(ec_config *config, ec_services *services, const char *name) {
    if (!strcmp(name, "ssh")) {
        return start_ssh(config, services);
    }
    if (!strcmp(name, "ftp")) {
        return start_ftp(config, services);
    }
    return ec_error("unknown service: %s", name);
}

void ec_poll(ec_services *services) {
    int status;
    if (services->ssh && waitpid(services->ssh, &status, WNOHANG) == services->ssh) {
        fprintf(stderr, "SSH listener exited\n");
        services->ssh = 0;
    }
    if (services->ftp && waitpid(services->ftp, &status, WNOHANG) == services->ftp) {
        fprintf(stderr, "FTP listener exited\n");
        services->ftp = 0;
    }
}

int ec_stop(ec_config *config, ec_services *services, const char *name) {
    (void)config;
    pid_t *slot = !strcmp(name, "ssh")   ? &services->ssh
                  : !strcmp(name, "ftp") ? &services->ftp
                                         : NULL;
    if (!slot) {
        return ec_error("unknown service: %s", name);
    }
    if (!*slot) {
        return 0;
    }
    int status;
    pid_t already_reaped = waitpid(*slot, &status, WNOHANG);
    if (already_reaped == *slot || (already_reaped < 0 && errno == ECHILD)) {
        *slot = 0;
        return 0;
    }
    /* Signal only this supervisor's process group; the other daemon continues. */
    kill(-*slot, SIGTERM);
    for (int i = 0; i < SERVICE_STOP_TIMEOUT_MS / SERVICE_POLL_INTERVAL_MS; ++i) {
        pid_t reaped = waitpid(*slot, &status, WNOHANG);
        if (reaped == *slot || (reaped < 0 && errno == ECHILD)) {
            *slot = 0;
            return 0;
        }
        pause_ms(SERVICE_POLL_INTERVAL_MS);
    }
    return ec_error("%s is still stopping; try again", name);
}

void ec_stop_all(ec_config *config, ec_services *services) {
    /* Each supervisor reaps and terminates its own session descendants.
     * InkView can have helper children too: never signal or reap them here. */
    ec_stop(config, services, "ssh");
    ec_stop(config, services, "ftp");
}

void ec_services_close(ec_config *config, ec_services *services) {
    ec_stop_all(config, services);
    /* Logs deliberately survive until reboot for diagnostics; secrets do not. */
    const char *secrets[] = {"authorized_keys", "password.hash", "ftp-password.hash",
                             "host_ed25519", NULL};
    char path[EC_PATH];
    if (*config->runtime) {
        for (int i = 0; secrets[i]; ++i) {
            if (!ec_path(path, sizeof path, config->runtime, secrets[i])) {
                unlink(path);
            }
        }
    }
    if (services->lock_fd >= 0) {
        close(services->lock_fd);
        services->lock_fd = -1;
    }
}

int ec_fingerprint(const ec_config *config) {
    char tool[EC_PATH], key[EC_PATH];
    if (ec_path(tool, sizeof tool, config->root, "bin/dropbearkey") ||
        ec_path(key, sizeof key, config->state, "host_ed25519")) {
        return -1;
    }
    char *argv[] = {tool, "-y", "-f", key, NULL};
    return run_command(argv);
}

int ec_key_info(const ec_config *config, char *out, size_t size) {
    char tool[EC_PATH], key[EC_PATH];
    int channel[2];
    if (!size) {
        return -1;
    }
    *out = 0;
    if (ec_path(tool, sizeof tool, config->root, "bin/dropbearkey") ||
        ec_path(key, sizeof key, config->state, "host_ed25519") || pipe(channel)) {
        return -1;
    }
    pid_t pid = fork();
    if (!pid) {
        close(channel[0]);
        if (dup2(channel[1], STDOUT_FILENO) < 0) {
            _exit(126);
        }
        close(channel[1]);
        execl(tool, tool, "-y", "-f", key, (char *)NULL);
        _exit(127);
    }
    close(channel[1]);
    if (pid < 0) {
        close(channel[0]);
        return -1;
    }
    char buffer[1024];
    size_t used = 0;
    ssize_t n;
    /* Drain fully even if the caller requests only a short output. */
    while ((n = read(channel[0], buffer, sizeof buffer)) > 0) {
        size_t take = (size_t)n;
        if (take > size - 1 - used) {
            take = size - 1 - used;
        }
        memcpy(out + used, buffer, take);
        used += take;
    }
    out[used] = 0;
    close(channel[0]);
    return child_status(pid);
}
